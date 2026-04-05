/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
 */

#define _GNU_SOURCE
#include <linux/limits.h>
#include <sys/select.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <linux/sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

static volatile sig_atomic_t g_should_stop = 0;
static volatile sig_atomic_t g_child_exited = 0;

static void handle_sigchild(int sig) {
    (void)sig;
    g_child_exited = 1;
}
 static void handle_shutdown(int sig) {
    (void)sig;
    g_should_stop = 1;
}

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

typedef struct {
    int read_fd;
    char container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *log_buffer;
} pipe_reader_args_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    // wait while full
    while(buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }

    //woke up due to shutdown so dont insert
    if(buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }
    // insert (circular)
    
    buffer->items[buffer->head] = *item;
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    // wake a consumer
    // pthread_cond_signal() restarts one of the threads that are waiting on the condition variable cond.  
    // If no threads are waiting on cond, nothing happens.   If  several  threads  are
    // waiting on cond, exactly one is restarted, but it is not specified which.

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
     /* NOTE :  pthread_cond_wait()  atomically  unlocks the mutex (as per pthread_unlock_mutex()) and waits for the condition variable cond to be signaled.  The thread execution is suspended and
     does not consume any CPU time until the condition variable is signaled.  The mutex must be locked by the calling thread on entrance to pthread_cond_wait().   Before  returning  to
     the calling thread, pthread_cond_wait() re-acquires mutex (as per pthread_mutex_lock()). */
}

/*
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 *   - wait correctly while the buffer is empty
 *   - return a useful status when shutdown is in progress
 *   - avoid races with producers and shutdown
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    // wait if empty, but keep going if shutdown (drain remaining items)
    while(buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }

    // shutdown AND empty --> nothing left to drain tell caller to exit
    if(buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    //remove form the end (circular)
    *item = buffer->items[buffer->tail];
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    // wake a producer
    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    while(1) {
        // blocks until item is available or shutdown + empty
        int rc = bounded_buffer_pop(&ctx->log_buffer, &item);
        if(rc < 0) {
            break; // shutdown , buffer drained -> exit
        }

        // find the log path for this container
        char log_path[PATH_MAX];
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        //search the linked list for its ID
        while(c != NULL) {
            if(strncmp(c->id, item.container_id, CONTAINER_ID_LEN) == 0) { // compare both the IDs
                strncpy(log_path, c->log_path, PATH_MAX - 1); // copy the path 
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if(c == NULL) {
            // unknown container , discard
            continue;
        }

        //append chunk to log file
        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644); // allows the owner to read and write a file, while others can only read it
        if(fd < 0) {
            perror("open log file");
            continue;
        }

        write(fd, item.data, item.length);
        close(fd);
    }
    return NULL;
}

void *pipe_reader_thread(void *arg)
{
    pipe_reader_args_t *args = (pipe_reader_args_t *)arg;
    log_item_t item;
    ssize_t n;

    while (1) {
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, args->container_id, CONTAINER_ID_LEN - 1);

        n = read(args->read_fd, item.data, LOG_CHUNK_SIZE - 1);
        if (n <= 0)
            break;

        item.length = (size_t)n;
        if (bounded_buffer_push(args->log_buffer, &item) < 0)
            break;
    }

    close(args->read_fd);
    free(args);
    return NULL;
}

/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
int child_fn(void *arg)
{
    // redirecting stdout / stderr to the supervisor logging path
    child_config_t *conf = (child_config_t *)arg;
    
    if( dup2(conf->log_write_fd , STDOUT_FILENO) < 0 || dup2(conf->log_write_fd , STDERR_FILENO) < 0 ) {
        perror("dup2");
        return 1;
    }
    close(conf->log_write_fd);

    // mount /proc into rootfs so that commmands like ps work
    // must be done before chroot after chroot you cant see the host path
    // /proc is a virtual filesystem the kernel provides — it's not on disk. You have to explicitly mount it inside the container's rootfs 
    // so commands like ps, top, cat /proc/meminfo work inside.
    
    char proc_path[PATH_MAX];
    snprintf(proc_path , sizeof(proc_path) , "%s/proc", conf->rootfs);
    mkdir(proc_path, 0555);
    
    if (mount("proc", proc_path , "proc", 0, NULL) < 0) {
        perror("mount proc");
        return 1;
    }

    // chroot = lock the container into its own filesystem so it can't touch the host.
    // Without chdir("/") after, the working directory still points to the old host path even though root changed
    if(chroot(conf->rootfs) < 0) {
        perror("chroot");
        return 1;
    }
    if(chdir("/") < 0) {
        perror("chdir");
        return 1;
    }
    // exec the command
    
    char *argv[] = {conf->command , NULL};
    char *envp[] = {
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        "HOME=/root",
        "TERM=xterm",
        NULL
    };

    execve(conf->command, argv, envp);

    //execve only returns on failure
    perror("execve");
    return 1;

}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /*
     * TODO:
     *   1) open /dev/container_monitor
     *   2) create the control socket / FIFO / shared-memory channel
     *   3) install SIGCHLD / SIGINT / SIGTERM handling
     *   4) spawn the logger thread
     *   5) enter the supervisor event loop
     */

    // TODO 1
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR); 
    if(ctx.monitor_fd < 0) {
        fprintf(stderr, "monitor not available, continuing without it\n");
    }

    // TODO 2 - creating sockets
    struct sockaddr_un addr;
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0); // creates a socket at CONTROL_PATH
    unlink(CONTROL_PATH); // because bind fails if the path already in use
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    int res = bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr));
    if(res < 0) {
        perror("bind");
        return 1;
    }
    listen(ctx.server_fd, 10); // Allow up to 10 clients to wait in queue while the server hasn’t accepted them yet.

    // TODO 3 - signal handling (these should be atomic instuctions)
    struct sigaction sa;
    memset(&sa , 0, sizeof(sa));
    sa.sa_handler = handle_sigchild; // pointer to the signal catching function
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
    sa.sa_handler = handle_shutdown;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);


    //TODO 4 - logger thread part
    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);


    //TODO 5 - event loop
    while (!g_should_stop) {

    // reap any exited children
    if (g_child_exited) {
        g_child_exited = 0;
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            pthread_mutex_lock(&ctx.metadata_lock);
            container_record_t *c = ctx.containers;

            while (c != NULL) {

                if (c->host_pid == pid) {

                    if (WIFEXITED(status)) {
                        c->state = CONTAINER_EXITED;
                        c->exit_code = WEXITSTATUS(status);
                    } else if (WIFSIGNALED(status)) {
                        c->state = CONTAINER_KILLED;
                        c->exit_signal = WTERMSIG(status);
                    }
                    break;
                }
                c = c->next;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
        }
    }

    // wait for a client with 1 second timeout
    fd_set rfds;
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    FD_ZERO(&rfds);
    FD_SET(ctx.server_fd, &rfds);

    if (select(ctx.server_fd + 1, &rfds, NULL, NULL, &tv) <= 0)
        continue;

    int client_fd = accept(ctx.server_fd, NULL, NULL);
    if (client_fd < 0) {
        if (errno == EINTR) continue;
        perror("accept");
        break;
    }

    control_request_t req;
    memset(&req, 0, sizeof(req));
    if (read(client_fd, &req, sizeof(req)) != sizeof(req)) {
        close(client_fd);
        continue;
    }

    control_response_t resp;
    memset(&resp, 0, sizeof(resp));

    if (req.kind == CMD_START || req.kind == CMD_RUN) {

        // create log directory and file path
        char log_path[PATH_MAX];
        mkdir(LOG_DIR, 0755);
        snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, req.container_id);

        // create pipe for container output
        int pipefd[2];
        if (pipe(pipefd) < 0) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message), "pipe failed");
            write(client_fd, &resp, sizeof(resp));
            close(client_fd);
            continue;
        }

        // set up child config
        child_config_t *cfg = malloc(sizeof(child_config_t));
        memset(cfg, 0, sizeof(child_config_t));
        strncpy(cfg->id, req.container_id, CONTAINER_ID_LEN - 1);
        strncpy(cfg->rootfs, req.rootfs, PATH_MAX - 1);
        strncpy(cfg->command, req.command, CHILD_COMMAND_LEN - 1);
        cfg->nice_value = req.nice_value;
        cfg->log_write_fd = pipefd[1];

        // allocate stack for clone (similar to fork)
        char *stack = malloc(STACK_SIZE);
        char *stack_top = stack + STACK_SIZE;

        pid_t pid = clone(child_fn, stack_top,
                          CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                          cfg);

        close(pipefd[1]);  // supervisor closes write end

        if (pid < 0) {
            perror("clone");
            close(pipefd[0]);
            free(cfg);
            free(stack);
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message), "clone failed");
            write(client_fd, &resp, sizeof(resp));
            close(client_fd);
            continue;
        }

        // add container record to metadata list
        container_record_t *record = malloc(sizeof(container_record_t));
        memset(record, 0, sizeof(*record));
        strncpy(record->id, req.container_id, CONTAINER_ID_LEN - 1);
        record->host_pid = pid;
        record->started_at = time(NULL);
        record->state = CONTAINER_RUNNING;
        record->soft_limit_bytes = req.soft_limit_bytes;
        record->hard_limit_bytes = req.hard_limit_bytes;
        strncpy(record->log_path, log_path, PATH_MAX - 1);

        pthread_mutex_lock(&ctx.metadata_lock);
        record->next = ctx.containers;
        ctx.containers = record;
        pthread_mutex_unlock(&ctx.metadata_lock);

        // register with kernel monitor if available
        if (ctx.monitor_fd >= 0) {
            register_with_monitor(ctx.monitor_fd, req.container_id, pid,req.soft_limit_bytes, req.hard_limit_bytes);
        }

        pipe_reader_args_t *reader_args = malloc(sizeof(pipe_reader_args_t));
        reader_args->read_fd = pipefd[0];
        reader_args->log_buffer = &ctx.log_buffer;
        strncpy(reader_args->container_id, req.container_id, CONTAINER_ID_LEN - 1);

        pthread_t reader_thread;
        pthread_create(&reader_thread, NULL, pipe_reader_thread, reader_args);
        pthread_detach(reader_thread);


        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message), "started %s pid=%d", req.container_id, pid);

        if (req.kind == CMD_RUN)
            waitpid(pid, NULL, 0);  // foreground — block until done

    } 
    else if (req.kind == CMD_PS) {

        pthread_mutex_lock(&ctx.metadata_lock);
        container_record_t *c = ctx.containers;
        int off = 0;
        off += snprintf(resp.message + off, sizeof(resp.message) - off,
                        "%-12s %-8s %-10s\n", "ID", "PID", "STATE");
        while (c != NULL && off < (int)sizeof(resp.message) - 1) {
            off += snprintf(resp.message + off, sizeof(resp.message) - off,"%-12s %-8d %-10s\n", c->id, c->host_pid, state_to_string(c->state));
            c = c->next;
        }
        pthread_mutex_unlock(&ctx.metadata_lock);
        resp.status = 0;

    } 
    else if (req.kind == CMD_STOP) {

        pthread_mutex_lock(&ctx.metadata_lock);
        container_record_t *c = ctx.containers;
        while (c != NULL) {
            if (strncmp(c->id, req.container_id, CONTAINER_ID_LEN) == 0) {
                kill(c->host_pid, SIGTERM);
                c->state = CONTAINER_STOPPED;
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&ctx.metadata_lock);
        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message), "stopped %s", req.container_id);

    } 
    else if (req.kind == CMD_LOGS) {

        pthread_mutex_lock(&ctx.metadata_lock);
        container_record_t *c = ctx.containers;
        while (c != NULL) {
            if (strncmp(c->id, req.container_id, CONTAINER_ID_LEN) == 0) {
                strncpy(resp.message, c->log_path, sizeof(resp.message) - 1);
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&ctx.metadata_lock);
        resp.status = 0;
    }

    write(client_fd, &resp, sizeof(resp));
    close(client_fd);
    }

    // cleanup
    fprintf(stderr, "Supervisor shutting down...\n");
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    close(ctx.server_fd);
    unlink(CONTROL_PATH);
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    return 0;
}

/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
static int send_control_request(const control_request_t *req)
{

    int sock_fd;
    struct sockaddr_un addr;
    control_response_t resp;

    // create socket
    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if(sock_fd < 0) {
        perror("socket");
        return 1;
    }
    
    //connect to supervisor
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if(connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect - is the supervisor running (check the status of supervisor)");
        close(sock_fd);
        return 1;
    }

    // send the req
    
    if(write(sock_fd, req, sizeof(*req)) != sizeof(*req)) {
        perror("write");
        close(sock_fd); 
        return 1;
    }

    // read the response
    memset(&resp, 0, sizeof(resp));
    if(read(sock_fd, &resp, sizeof(resp)) != sizeof(resp)) {
        perror("read");
        close(sock_fd);
        return 1;
    }

    // print response to terminal 
    
    printf("%s\n", resp.message);
    close(sock_fd);

    return resp.status == 0 ? 0 : 1;
    
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    /*
     * TODO:
     * The supervisor should respond with container metadata.
     * Keep the rendering format simple enough for demos and debugging.
     */
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}