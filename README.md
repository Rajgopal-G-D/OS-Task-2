# Multi-Container Runtime with Kernel Memory Monitor

A lightweight container runtime built in C for Linux, featuring a **supervisor-based architecture**, **CLI control interface**, and a **kernel-space memory monitor**.

This project is built entirely from scratch without Docker or any container library — it directly uses Linux kernel primitives like `clone()`, `chroot()`, `pipes`, `UNIX sockets`, and `ioctl` to implement container isolation, inter-process communication, and memory enforcement.

---

## 📌 Overview

This project implements a minimal container runtime that allows users to:

- Launch and manage multiple isolated containers concurrently
- Interact with the runtime through a command-line interface (CLI)
- Capture and view per-container logs (stdout and stderr)
- Enforce memory limits on containers using a kernel-loadable module

The system is split into two major components:

- **User-space Supervisor (`engine.c`)** — A long-running daemon that manages container lifecycle, handles CLI commands over a UNIX socket, and captures logs through pipes.
- **Kernel-space Memory Monitor (`monitor.c`)** — A Linux kernel module that tracks registered processes, periodically checks their RSS (Resident Set Size) memory usage, logs warnings on soft limit breach, and forcibly kills processes that exceed the hard limit.

These two components communicate via `ioctl` calls through a shared interface defined in `monitor_ioctl.h`.

---

## ⚙️ Requirements

Before you begin, make sure your environment meets the following requirements:

| Requirement | Details |
|---|---|
| OS | Ubuntu 22.04 or Ubuntu 24.04 (running in a VM is recommended) |
| Secure Boot | Must be **disabled** — kernel modules cannot be loaded with Secure Boot on |
| Compiler | GCC (GNU C Compiler) |
| Build tool | Make |
| Kernel headers | Must match your currently running kernel version |

---

### ⬇️ Installing Dependencies

```bash
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r)
```

> `build-essential` installs GCC, Make, and other essential compilation tools.  
> `linux-headers-$(uname -r)` installs headers that match your exact running kernel — required to compile `monitor.c` as a kernel module.

To verify everything installed correctly:

```bash
gcc --version
make --version
ls /lib/modules/$(uname -r)/build
```

The last command should print a valid path without errors, confirming kernel headers are in place.

---

## 📁 Project Structure

```
boilerplate/
├── engine.c              # User-space supervisor + CLI entry point
├── monitor.c             # Kernel module: memory monitoring logic
├── monitor_ioctl.h       # Shared header: ioctl command definitions
├── Makefile              # Build rules for both engine and kernel module
├── logs/                 # Auto-created directory; stores per-container log files
├── rootfs-*/             # Root filesystem directories used as container environments
```

### File Descriptions

- **`engine.c`** — The heart of the user-space side. When run as `supervisor`, it becomes a daemon listening on a UNIX socket. When invoked with commands like `start`, `stop`, `ps`, or `logs`, it acts as a CLI client that sends those commands to the running supervisor.

- **`monitor.c`** — A Linux Loadable Kernel Module (LKM). Once inserted with `insmod`, it runs a kernel thread that periodically scans registered PIDs, reads their memory usage from `/proc/<pid>/status`, and takes action based on configured thresholds.

- **`monitor_ioctl.h`** — Defines the `ioctl` request codes and data structures shared between `engine.c` (user space) and `monitor.c` (kernel space). Both files include this header to stay in sync.

- **`Makefile`** — Has two targets: one for compiling `engine.c` into an executable using GCC, and one for building `monitor.c` into a kernel object (`.ko`) using the kernel build system (`kbuild`).

- **`logs/`** — Created automatically by the supervisor when it starts. Each container gets a file named `<container_id>.log` inside this directory.

- **`rootfs-*/`** — These are minimal Linux root filesystems (containing `/bin`, `/lib`, etc.) that serve as the isolated environment for each container. You can create one using `debootstrap` or by extracting a minimal Alpine Linux tarball.

---

## 🧠 Architecture

The system is designed around two separate communication paths — one for control commands, one for log data — to keep concerns cleanly separated.

```
CLI (Client)
     │
     │  sends command strings over UNIX socket
     ▼
UNIX Socket (/tmp/mini_runtime.sock)
     │
     ▼
Supervisor (engine.c)                        ← runs as daemon (sudo ./engine supervisor)
 ├── Container creation via clone()
 ├── Logging via pipes (stdout + stderr)
 ├── State tracking (ID, PID, status)
 └── Signal handling (SIGCHLD, SIGTERM)
     │
     │  registers container PIDs via ioctl
     ▼
Kernel Module (monitor.c)                    ← loaded via sudo insmod monitor.ko
 ├── Maintains list of tracked PIDs
 ├── Periodically reads RSS from /proc
 ├── Soft limit breach → logs warning to dmesg
 └── Hard limit breach → sends SIGKILL to process
```

### Why Two Separate Paths?

- **Control path (UNIX socket):** Used for CLI commands like `start`, `stop`, `ps`. These are low-frequency, text-based messages. A UNIX domain socket is simple, fast, and secure for local IPC.
- **Logging path (pipes):** Used for continuous stdout/stderr capture from containers. Pipes are the right tool here because they are streaming, file-descriptor-based, and can be redirected into log files efficiently without involving the socket.

Mixing these two would complicate the supervisor logic and make it harder to scale, so they are kept completely independent.

---

## 🔄 System Working

### 1. Container Creation

When you run `./engine start <id> <rootfs> "<command>"`, the supervisor does the following:

1. Calls `clone()` with the following namespace flags:
   - `CLONE_NEWUTS` — gives the container its own hostname (UTS namespace)
   - `CLONE_NEWNS` — gives the container its own mount namespace so mounts do not affect the host
2. Inside the child process (the container), calls `chroot(<rootfs>)` to change the root directory to the provided filesystem path, followed by `chdir("/")`.
3. Executes the provided command using `execvp()`.
4. Sets up a pipe before `clone()` so the supervisor can read the container's stdout/stderr and write it to the log file.

> **Note:** `CLONE_NEWPID` (PID namespace) is intentionally **not used** because it can cause kernel panics in certain VM configurations. See the Design Decisions section for details.
> <img width="799" height="163" alt="1" src="https://github.com/user-attachments/assets/84bd3e63-0621-4399-8872-dbf66de6608e" />


### 2. Supervisor Daemon

The supervisor is the central component of the runtime. Here is what happens when you run `sudo ./engine supervisor`:

1. It creates the UNIX socket at `/tmp/mini_runtime.sock` and starts listening.
2. It maintains an in-memory array of container records, each storing:
   - Container ID (string)
   - PID of the container process
   - Current state (`running`, `stopped`, `exited`)
3. It enters an event loop, accepting connections from CLI clients and processing commands.
4. For each `start` command, it forks a container, sets up a logging pipe, and stores the container's metadata.
5. It handles `SIGCHLD` to reap exited child processes and prevent zombies.
6. On `SIGINT` or `SIGTERM`, it gracefully stops all running containers and cleans up the socket file.
   <img width="1366" height="768" alt="image" src="https://github.com/user-attachments/assets/0ef45acd-3b77-4395-a64d-5e59dd972a4a" />
   <img width="1366" height="768" alt="image" src="https://github.com/user-attachments/assets/26007ccd-1757-4d33-9f7e-49664e3787f5" />
   <img width="1366" height="768" alt="image" src="https://github.com/user-attachments/assets/59863842-a632-459d-8d57-218cf46b299d" />
   <img width="1366" height="768" alt="image" src="https://github.com/user-attachments/assets/2a01bae3-bc6f-4bdd-9200-ba837ea0d3f3" />



The supervisor must remain running in a terminal for CLI commands to work. Without it, there is nothing listening on the socket and all CLI commands will fail.

### 3. Logging (Path A — Pipes)

Before creating a container, the supervisor calls `pipe()` to create a read-write file descriptor pair.

- The **write end** is passed to the container as its `stdout` and `stderr` (via `dup2()`).
- The **read end** is held by the supervisor, which uses a background loop to drain the pipe and append data to `logs/<container_id>.log`.

This means container output is captured transparently without the container itself needing to know anything about the logging system.

To view logs at any time:

```bash
./engine logs <container_id>
```

This sends a `logs` command to the supervisor over the UNIX socket, and the supervisor reads and returns the content of the corresponding log file.
<img width="1275" height="806" alt="3" src="https://github.com/user-attachments/assets/570bd047-d8db-4833-acb5-9b5234bd6cb5" />


### 4. Control Plane (Path B — UNIX Socket)

All CLI commands travel through the UNIX domain socket at:

```
/tmp/mini_runtime.sock
```

When you run any CLI command (e.g., `./engine ps`), the `engine` binary:

1. Connects to the socket as a client.
2. Sends a formatted command string (e.g., `"ps\n"` or `"start c1 ./rootfs sleep 20\n"`).
3. Waits for a response from the supervisor.
4. Prints the response to the terminal and exits.

The supervisor receives the command, processes it, and writes a response back through the same socket connection.

Supported commands over the socket:

| Command | What it does |
|---|---|
| `start <id> <rootfs> "<cmd>"` | Creates and starts a container in the background |
| `run <id> <rootfs> "<cmd>"` | Creates a container and attaches to it in the foreground |
| `ps` | Returns a list of all containers with their ID, PID, and state |
| `stop <id>` | Sends SIGTERM (then SIGKILL fallback) to the container process |
| `logs <id>` | Returns the contents of `logs/<id>.log` |
<img width="1305" height="195" alt="4" src="https://github.com/user-attachments/assets/01c962a0-f011-4a06-90f9-d5ed72620168" />

### 5. Kernel Memory Monitor

After a container is started, the supervisor registers the container's PID with the kernel module using an `ioctl()` call:

```c
ioctl(fd, MONITOR_ADD_PID, &pid);
```

The kernel module then:

1. Adds the PID to an internal tracked list.
2. A kernel thread wakes up periodically (e.g., every 2 seconds).
3. For each tracked PID, it reads the process's RSS memory from `/proc/<pid>/status` — specifically the `VmRSS` field.
4. Compares RSS against two configurable thresholds:
   - **Soft limit:** Logs a warning to the kernel ring buffer (`printk` → visible via `dmesg`). The process continues running.
   - **Hard limit:** Sends `SIGKILL` to the process immediately. The process is terminated.
5. When a process exits (PID no longer valid), it is automatically removed from the tracked list.

To observe memory monitoring events in real time:

```bash
sudo dmesg -w
```

---

## 🖥️ CLI Commands

All commands are run from inside the `boilerplate/` directory. The supervisor must already be running before using any of these.

```bash
# Start a container in the background (non-blocking)
# <id>       : a unique name/identifier for this container (e.g., c1, web, db)
# <rootfs>   : path to the root filesystem directory (e.g., ./rootfs)
# "<command>": the command to run inside the container (e.g., "sleep 20")
./engine start <id> <rootfs> "<command>"

# Run a container in the foreground (blocks your terminal until the container exits)
./engine run <id> <rootfs> "<command>"

# List all containers currently tracked by the supervisor
# Output columns: ID | PID | State
./engine ps

# Stop a running container
# Sends SIGTERM first; if the process doesn't exit within the timeout, sends SIGKILL
./engine stop <id>

# View the captured stdout/stderr log for a container
./engine logs <id>
```
---

## 🧪 Testing

Each test below verifies a specific subsystem. Run them in a separate terminal while the supervisor is running in its own terminal.

### Test 1 — Logging Test

Verifies that stdout from the container is captured to the log file:

```bash
# Start a container that continuously outputs "hello"
./engine start c2 ./rootfs "yes hello"

# Wait 2 seconds for some output to accumulate
sleep 2

# View the captured log via the CLI
./engine logs c2
# Expected: many lines of "hello"

# Or check the file directly
cat logs/c2.log | head -20
```

### Test 2 — Stop Test

Verifies that a running container can be stopped cleanly:

```bash
./engine stop c2

# Confirm it is no longer running
./engine ps
# Expected: c2 state shows "stopped" or it is no longer listed
```

### Test 3 — Memory Monitor Test

Verifies that the kernel module detects and responds to memory usage:

```bash
# Start a container running "yes" which generates continuous output and memory activity
./engine start c3 ./rootfs "yes"

# Wait 2 seconds
sleep 2

# Check kernel logs for memory monitoring messages
sudo dmesg | tail -20
# Expected: lines like: [monitor] PID <x>: RSS = <y> kB — soft limit warning
```

To watch kernel log messages in real time in a separate terminal:

```bash
sudo dmesg -w
```

### Test 4 — Zombie Process Check

Verifies that exited containers are properly reaped and do not linger as zombie processes:

```bash
# Start a short-lived container (exits after 1 second)
./engine start c4 ./rootfs "sleep 1"

# Wait for it to exit
sleep 3

# Check for zombie (defunct) processes
ps aux | grep defunct
# Expected: no lines containing "defunct" related to your containers
```

If zombies appear, the `SIGCHLD` handler in `engine.c` is not correctly calling `waitpid()`.

### Test 5 — Multiple Simultaneous Containers

Verifies that multiple containers can run at the same time with independent PIDs and logs:

```bash
./engine start c5 ./rootfs "sleep 30"
./engine start c6 ./rootfs "sleep 30"
./engine start c7 ./rootfs "sleep 30"

./engine ps
# Expected: c5, c6, c7 all listed as "running" with distinct PIDs
```

---

## 🔔 Signal Handling

Signal handling is critical for correctness and preventing resource leaks. Here is how each signal is handled in the supervisor:

| Signal | Handler | Purpose |
|---|---|---|
| `SIGCHLD` | Custom handler | Called when any child process exits. Calls `waitpid()` with `WNOHANG` to reap the child without blocking and updates its state to `exited`. Without this, every exited container becomes a zombie process that holds a PID indefinitely. |
| `SIGINT` | Custom handler | Triggered by Ctrl+C in the supervisor's terminal. Initiates graceful shutdown: iterates over all running containers, sends them SIGTERM, waits briefly, then removes the UNIX socket file and exits the supervisor. |
| `SIGTERM` | Custom handler | Same behaviour as `SIGINT`. Allows the supervisor to be stopped cleanly with `kill <supervisor_pid>`. |

### Container Termination Flow (when `./engine stop <id>` is called)

1. The CLI client sends a `stop <id>` message to the supervisor over the UNIX socket.
2. The supervisor looks up the container's PID from its in-memory state table.
3. Sends `SIGTERM` to the container's PID — giving the process a chance to clean up.
4. Waits up to 5 seconds for the process to exit voluntarily.
5. If the process is still alive after the timeout, sends `SIGKILL` — which cannot be caught or ignored and guarantees immediate termination.
6. Calls `waitpid()` to reap the process and updates its state to `stopped`.

---


## 🎯 Conclusion

This project demonstrates practical, low-level systems programming by building a working container runtime from Linux primitives:

- **OS-level process isolation** using `clone()` with UTS and mount namespaces, and `chroot()` for filesystem isolation — the same fundamental mechanisms used by production container runtimes like `runc`.

- **IPC design with separation of concerns** — control commands travel over a UNIX socket (synchronous, structured), while log data flows through pipes (asynchronous, streaming). This mirrors how production systems separate the control plane from the data plane.

- **Kernel-user communication via `ioctl`** — a shared header defines the interface, and the kernel module exposes a character device that user space can open and invoke `ioctl` on to register PIDs for monitoring.

- **Resource monitoring and enforcement** — the kernel module periodically inspects process memory using kernel APIs, demonstrating how the OS can enforce policies at a level completely below any user-space process.

Together, these techniques form the foundation of how container platforms like Docker and containerd work under the hood.

---

## 👨‍💻 Author

**Praveen Balaji P**

**Rajgopal Deshpande**

---

## 📌 Notes

- **Do not commit `rootfs-*` directories** — they are large filesystem trees and should be listed in `.gitignore`. Anyone cloning the repo should create their own rootfs using `debootstrap` or Alpine as described in the Setup section.
- **Always run the supervisor with `sudo`** — namespace creation and kernel module communication require root privileges.
- **Always load `monitor.ko` before starting the supervisor** — the supervisor attempts to open the kernel module's device file at startup. If the module is not loaded, PID registration will fail silently and containers will not be memory-monitored.
- **To unload the kernel module** after testing: `sudo rmmod monitor`
- **To verify the module is unloaded**: `lsmod | grep monitor` should return no output.
