# Multi-Container Runtime

## Team Information

* Name 1 : SUMUKH KURADI KALKURA
* SRN 1 : PES1UG24CS244
* Name 2 : KARNATI UDAY KEERTHAN REDDY
* SRN 2 : PES1UG24CS217

---

## Project Summary

This project implements a lightweight container runtime in C with:

* Supervisor process
* Kernel memory monitor
* Multiple containers
* Memory limits (soft + hard)
* CPU scheduling experiments

---

## Setup & Installation

### Install Dependencies

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

---

### Setup Root Filesystem

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
```

---

## Build

```bash
cd boilerplate
make clean
make
```

---

## Load Kernel Module

```bash
sudo insmod monitor.ko
lsmod | grep monitor
```

---

## Start Supervisor (Terminal 1)

```bash
sudo ./engine supervisor ./rootfs-base
```

**Screenshot 1 – Supervisor Running**
SHOW:

* `[supervisor] ready`
* socket path

<img width="1009" height="155" alt="Pasted Graphic 23" src="https://github.com/user-attachments/assets/cf37fa44-fe0a-4f5c-a060-4f0683873215" />


---

## Container Operations (Terminal 2)

---

### Start Containers

```bash
sudo ./engine start alpha ./rootfs-alpha /cpu_hog
sudo ./engine start beta ./rootfs-beta /cpu_hog
```

**Screenshot 2 – Multi Container Start**
SHOW:

* both containers started
* PIDs visible

<img width="1148" height="135" alt="Pasted Graphic 24" src="https://github.com/user-attachments/assets/b3baeba9-c65e-4698-aaeb-a18d033f0f72" />


---

### View Containers

```bash
sudo ./engine ps
```

**Screenshot 3 – Container Table**
SHOW:

* alpha + beta
* PID, STATE, memory columns

<img width="808" height="114" alt="container table" src="https://github.com/user-attachments/assets/f4e65653-b2a9-4b93-9da4-5780fdf05492" />


---

### Logs

```bash
sudo ./engine logs alpha
```

**Screenshot 4 – Logging Output**
SHOW:

* cpu_hog alive lines
* done message

<img width="965" height="334" alt="Pasted Graphic 26" src="https://github.com/user-attachments/assets/992079aa-3d2a-48cf-b01e-4dfb3093f51b" />


---

### CLI + IPC

```bash
sudo ./engine start beta ./rootfs-beta /cpu_hog
sudo ./engine ps
sudo ./engine stop beta
sudo ./engine ps
```

**Screenshot 5 – CLI Interaction**
SHOW:

* start + stop working
* updated container state

<img width="1160" height="527" alt="11604" src="https://github.com/user-attachments/assets/85245147-e373-4676-9e9f-bab8321a9d8f" />


---

## Memory Limit Testing

### Run Memory Test

```bash
sudo ./engine start memtest ./rootfs-alpha /memory_hog --soft-mib 48 --hard-mib 80
```

### Soft Limit

```bash
sudo dmesg | grep "SOFT LIMIT"
```

**Screenshot 6 – Soft Limit**
SHOW:

* "SOFT LIMIT" line

<img width="1216" height="205" alt="Pasted Graphic 28" src="https://github.com/user-attachments/assets/dd977f9b-62e7-4bcc-b476-8517411de8a8" />


---

### Hard Limit

```bash
sudo dmesg | tail -20
```

**Screenshot 7 – Hard Limit**
SHOW:

* "HARD LIMIT"
* process killed

<img width="1219" height="512" alt="N8et103191ous70 haresse" src="https://github.com/user-attachments/assets/cf257a68-4bf1-41d4-8ef9-7b6003aa04b7" />


---

## Scheduling Experiment

```bash
sudo ./engine start hipri ./rootfs-alpha /cpu_hog --nice -5
sudo ./engine start lopri ./rootfs-beta /cpu_hog --nice 15
pidstat -u 1 12
```

**Screenshot 8 – Scheduling**
SHOW:

* both cpu_hog processes
* %CPU difference

<img width="1204" height="585" alt="Pasted Graphic 20" src="https://github.com/user-attachments/assets/f776b81e-5c40-4226-97b1-25354d36b48d" />


---

## Cleanup

```bash
ps aux | grep -E 'engine|defunct|zombie'
sudo rmmod monitor
sudo dmesg | tail -5
```

**Screenshot 9 – Clean Teardown**
SHOW:

* no zombies
* "Module unloaded"

<img width="1220" height="591" alt="image" src="https://github.com/user-attachments/assets/ef46f81f-60ac-41da-aea2-7301b563da64" />


## 4. Engineering Analysis

---

### 4.1 Isolation Mechanisms

Isolation in this runtime is achieved through a combination of **Linux namespaces** and **filesystem separation**.

* **PID Namespace (`CLONE_NEWPID`)**

  * Each container gets its own process tree.
  * Processes inside the container see themselves starting from PID 1.
  * Prevents visibility into host or other container processes.

* **UTS Namespace (`CLONE_NEWUTS`)**

  * Allows each container to have its own hostname.
  * Demonstrates logical system separation.

* **Mount Namespace (`CLONE_NEWNS`)**

  * Ensures mount operations inside a container do not affect the host.
  * Provides isolation for filesystem views.

* **Filesystem Isolation (`chroot` / `pivot_root`)**

  * Each container runs inside its own **rootfs copy**.
  * Prevents access to host filesystem.
  * Ensures write operations are isolated per container.

**Key Insight:**
Even with all these mechanisms, containers **still share the same host kernel**.
This is why **kernel-level enforcement (memory monitor)** is critical — user-space isolation alone is not sufficient for safety.

---

### 4.2 Supervisor and Process Lifecycle

The **supervisor acts as the central authority** for managing all containers, similar to an `init` system.

Responsibilities:

* **Container Creation**

  * Uses `clone()` with namespace flags.
  * Initializes container environment and rootfs.

* **Metadata Tracking**

  * Stores:

    * Container ID
    * PID
    * Start time
    * State (running, exited, stopped, killed)
    * Memory limits
    * Exit status

* **Process Reaping**

  * Uses `SIGCHLD` handler + `waitpid()`
  * Ensures no zombie processes remain

* **Lifecycle Management**

  * Differentiates:

    * Normal exit
    * Manual stop
    * Hard-limit kill
  * Updates container state accordingly

**Why Supervisor is Important:**

* Centralized control
* Prevents orphan/zombie processes
* Enables multi-container orchestration

---

### 4.3 IPC, Threads, and Synchronization

The system uses **multiple IPC mechanisms**, each chosen based on purpose.

#### Control Path (CLI → Supervisor)

* **Unix Domain Socket**
* Provides:

  * Reliable local communication
  * Request-response model
  * Low overhead

#### Data Path (Container → Supervisor)

* **Pipes**
* Used to capture:

  * `stdout`
  * `stderr`
* Streams data into logging system

#### Kernel Communication

* **`ioctl` via `/dev/container_monitor`**
* Sends:

  * PID
  * Memory limits
* Enables kernel-space tracking

---

### Logging Design (Bounded Buffer)

* Uses **Producer–Consumer model**

* Components:

  * Producer → reads from pipe
  * Consumer → writes to log file

* Synchronization:

  * **Mutex** → protects shared buffer
  * **Condition Variables** → coordinate threads

#### Why This Matters:

Without synchronization:

* Race conditions corrupt buffer
* Logs get lost
* Deadlocks possible

#### Bounded Buffer Advantage:

* Prevents unlimited memory usage
* Handles bursty logging safely
* Ensures controlled throughput

---

### 4.4 Memory Management and Enforcement

Memory tracking is based on **RSS (Resident Set Size)**:

* Represents actual physical memory used
* Does NOT include:

  * swapped memory
  * unused virtual memory

---

### Two-Level Policy

#### Soft Limit

* Logs warning when exceeded
* Does NOT kill process
* Used for monitoring/debugging

#### Hard Limit

* Enforced via kernel module
* Sends `SIGKILL`
* Guarantees system protection

---

### Why Kernel-Space Enforcement?

User-space alone is insufficient because:

* It can be delayed or blocked
* Cannot reliably kill misbehaving processes under heavy load

Kernel module advantages:

* Runs with highest priority
* Immediate enforcement
* Cannot be bypassed by user processes

---

### 4.5 Scheduling Behavior

The runtime leverages Linux’s **Completely Fair Scheduler (CFS)**.

* Controlled using **nice values**

  * Lower nice → higher priority
  * Higher nice → lower priority

---

### Observed Behavior

* High-priority container:

  * Receives more CPU time
  * Completes faster

* Low-priority container:

  * Gets less CPU share
  * Still progresses (no starvation)

---

**Key Insight:**
CFS balances:

* **Fairness** → no starvation
* **Responsiveness** → prioritizes interactive/high-priority tasks

---

## 5. Design Decisions and Challenges

---

### Key Design Choices

| Component      | Decision       | Reason             | Tradeoff                    |
| -------------- | -------------- | ------------------ | --------------------------- |
| Control IPC    | Unix socket    | Simple + reliable  | Slight complexity           |
| Logging        | Bounded buffer | Prevent overflow   | Memory overhead             |
| Kernel monitor | LKM            | Strong enforcement | Requires root               |
| Isolation      | chroot         | Easy to implement  | Less secure than pivot_root |

---

### Major Challenges

* Ensuring **correct cleanup order**

  * stop → reap → unregister → flush logs

* Handling **race conditions**

  * concurrent logging threads

* Debugging **kernel-user interaction**

  * ioctl correctness
  * dmesg verification

* Preventing:

  * zombie processes
  * stale metadata
  * memory leaks

---

## 6. Scheduling Experiment Results

Two CPU-bound workloads were executed with different priorities.

| Container | Nice Value | Behavior                           |
| --------- | ---------- | ---------------------------------- |
| Alpha     | -20        | High CPU usage, faster completion  |
| Beta      | 19         | Lower CPU share, slower completion |

---

### Interpretation

* CFS assigns **weighted CPU share**
* High-priority process dominates CPU
* Low-priority process still executes (no starvation)

Confirms:

* Linux scheduler prioritizes responsiveness
* Maintains fairness across tasks

---

## 7. Final Ubuntu Verification Checklist

```bash
# Build
cd boilerplate
make clean
make
make ci

# Load module
sudo insmod monitor.ko
ls -l /dev/container_monitor

# Start supervisor
sudo ./engine supervisor ./rootfs-base

# Create rootfs copies
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta

# Start containers
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80 --nice -5
sudo ./engine start beta  ./rootfs-beta  /bin/sh --soft-mib 64 --hard-mib 96 --nice 10

# Verify
sudo ./engine ps
sudo ./engine logs alpha

# Stop
sudo ./engine stop alpha
sudo ./engine stop beta

# Check zombies
ps aux | grep -E 'defunct|engine'

# Kernel logs
dmesg | tail -50

# Unload module
sudo rmmod monitor
```

---

### Pass Criteria

* Build succeeds (`make`, `make ci`)
* Kernel module loads correctly
* Containers tracked in `engine ps`
* Soft + hard limit visible in `dmesg`
* No zombie processes after cleanup
* Module unload successful

---
