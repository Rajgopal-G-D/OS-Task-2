#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
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
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE       (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH     "/tmp/mini_runtime.sock"
#define LOG_DIR          "logs"
#define RESPONSE_SIZE    4096

/* ================= TYPES ================= */

typedef enum { CMD_START, CMD_RUN, CMD_PS, CMD_LOGS, CMD_STOP } command_kind_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[256];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
} control_request_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t pid;
    int running;
    struct container_record *next;
} container_record;

/* ================= GLOBALS ================= */

static container_record *containers = NULL;
static int monitor_fd = -1;

/* ================= SIGNAL ================= */

void handle_sigchld(int sig)
{
    (void)sig;
    int saved_errno = errno;   /* preserve errno across signal handler */
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        container_record *cur = containers;
        while (cur) {
            if (cur->pid == pid) {
                cur->running = 0;
                break;
            }
            cur = cur->next;
        }
    }
    errno = saved_errno;
}

/* ================= CHILD ================= */

/*
 * FIX #1 — receives a heap-allocated copy of the request so the pointer
 * remains valid regardless of what the parent's stack does after clone().
 */
int child_fn(void *arg)
{
    control_request_t *req = (control_request_t *)arg;

    sethostname(req->container_id, strlen(req->container_id));

    if (chdir(req->rootfs) < 0) { perror("chdir"); free(req); return 1; }
    if (chroot(req->rootfs) < 0) { perror("chroot"); free(req); return 1; }

    mkdir("/proc", 0555);
    mount("proc", "/proc", "proc", 0, NULL);

    /* Redirect stdout/stderr into the container's log file */
    char logpath[PATH_MAX];
    snprintf(logpath, sizeof(logpath), "/" LOG_DIR "/%s.log", req->container_id);
    int logfd = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (logfd >= 0) {
        dup2(logfd, STDOUT_FILENO);
        dup2(logfd, STDERR_FILENO);
        close(logfd);
    }

    /* Copy command before freeing req (exec will discard memory anyway) */
    char cmd[256];
    strncpy(cmd, req->command, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';
    free(req);

    execl("/bin/sh", "sh", "-c", cmd, NULL);
    perror("exec failed");
    return 1;
}

/* ================= CMD_RUN THREAD ================= */

/*
 * FIX #6 — CMD_RUN offloads waitpid() to a detached thread so the
 * supervisor's accept-loop is never blocked.
 */
typedef struct {
    pid_t pid;
    int   client_fd;
} run_wait_arg_t;

void *run_wait_thread(void *arg)
{
    run_wait_arg_t *wa = (run_wait_arg_t *)arg;
    waitpid(wa->pid, NULL, 0);
    const char *msg = "Finished\n";
    write(wa->client_fd, msg, strlen(msg));
    close(wa->client_fd);
    free(wa);
    return NULL;
}

/* ================= SUPERVISOR ================= */

int run_supervisor(void)
{
    /*
     * FIX #3 — use sigaction with SA_RESTART so that accept() is
     * automatically restarted after SIGCHLD instead of returning EINTR.
     */
    struct sigaction sa = {0};
    sa.sa_handler = handle_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);

    mkdir(LOG_DIR, 0755);

    /* FIX #2 — guard monitor open; warn but continue if unavailable */
    monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (monitor_fd < 0)
        fprintf(stderr, "Warning: /dev/container_monitor unavailable (%s). "
                        "Memory monitoring disabled.\n", strerror(errno));

    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    unlink(CONTROL_PATH);
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(server, 5) < 0) { perror("listen"); return 1; }

    /* FIX #9 — flush so the message actually appears in the terminal */
    printf("Supervisor running...\n");
    fflush(stdout);

    while (1) {
        /* FIX #3 (belt-and-suspenders) — retry on EINTR */
        int client;
        do { client = accept(server, NULL, NULL); } while (client < 0 && errno == EINTR);
        if (client < 0) { perror("accept"); continue; }

        control_request_t req;
        if (read(client, &req, sizeof(req)) != (ssize_t)sizeof(req)) {
            close(client);
            continue;
        }

        /* FIX #7 — track write offset for bounds-safe PS output */
        char response[RESPONSE_SIZE] = {0};
        int  rlen = 0;

        /* ---- START / RUN ---- */
        if (req.kind == CMD_START || req.kind == CMD_RUN) {

            void *stack = malloc(STACK_SIZE);
            if (!stack) {
                snprintf(response, sizeof(response), "ERROR: out of memory\n");
                goto send_response;
            }

            /* FIX #1 — heap-allocate req copy for child_fn */
            control_request_t *req_copy = malloc(sizeof(control_request_t));
            if (!req_copy) {
                free(stack);
                snprintf(response, sizeof(response), "ERROR: out of memory\n");
                goto send_response;
            }
            memcpy(req_copy, &req, sizeof(req));

            pid_t pid = clone(child_fn,
                              (char *)stack + STACK_SIZE,
                              CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                              req_copy);

            /* FIX #4 — check clone() result */
            if (pid < 0) {
                perror("clone");
                free(stack);
                free(req_copy);
                snprintf(response, sizeof(response),
                         "ERROR: clone failed: %s\n", strerror(errno));
                goto send_response;
            }

            /* FIX #2 — guard ioctl */
            if (monitor_fd >= 0) {
                struct monitor_request mreq = {
                    .pid              = pid,
                    .soft_limit_bytes = req.soft_limit_bytes,
                    .hard_limit_bytes = req.hard_limit_bytes
                };
                strncpy(mreq.container_id, req.container_id,
                        sizeof(mreq.container_id) - 1);
                ioctl(monitor_fd, MONITOR_REGISTER, &mreq);
            }

            container_record *node = malloc(sizeof(container_record));
            if (!node) {
                snprintf(response, sizeof(response), "ERROR: out of memory\n");
                goto send_response;
            }
            strncpy(node->id, req.container_id, CONTAINER_ID_LEN - 1);
            node->pid     = pid;
            node->running = 1;
            node->next    = containers;
            containers    = node;

            snprintf(response, sizeof(response),
                     "OK: container %s started (pid=%d)\n",
                     req.container_id, pid);

            if (req.kind == CMD_RUN) {
                /* FIX #6 — non-blocking wait via detached thread */
                run_wait_arg_t *wa = malloc(sizeof(run_wait_arg_t));
                if (wa) {
                    wa->pid       = pid;
                    wa->client_fd = client;
                    pthread_t tid;
                    pthread_create(&tid, NULL, run_wait_thread, wa);
                    pthread_detach(tid);
                    write(client, response, strlen(response));
                    continue;  /* thread owns client fd now */
                }
                /* fallback: block (shouldn't normally reach here) */
                waitpid(pid, NULL, 0);
                strncat(response, "Finished\n", sizeof(response) - strlen(response) - 1);
            }

        /* ---- PS ---- */
        } else if (req.kind == CMD_PS) {

            container_record *cur = containers;
            while (cur) {
                char line[128];
                int n = snprintf(line, sizeof(line), "%s pid=%d %s\n",
                                 cur->id, cur->pid,
                                 cur->running ? "RUNNING" : "STOPPED");
                /* FIX #7 — bounds-checked strcat */
                if (rlen + n < RESPONSE_SIZE - 1) {
                    memcpy(response + rlen, line, n);
                    rlen += n;
                } else {
                    strncpy(response + rlen, "...(truncated)\n",
                            RESPONSE_SIZE - rlen - 1);
                    break;
                }
                cur = cur->next;
            }
            if (rlen == 0)
                snprintf(response, sizeof(response), "(no containers)\n");

        /* ---- LOGS ---- */
        } else if (req.kind == CMD_LOGS) {

            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req.container_id);
            FILE *f = fopen(path, "r");
            if (!f) {
                snprintf(response, sizeof(response), "No logs found\n");
            } else {
                fread(response, 1, sizeof(response) - 1, f);
                fclose(f);
            }

        /* ---- STOP ---- */
        } else if (req.kind == CMD_STOP) {

            int found = 0;
            container_record *cur = containers;
            while (cur) {
                if (strncmp(cur->id, req.container_id, CONTAINER_ID_LEN) == 0
                    && cur->running) {
                    kill(cur->pid, SIGTERM);
                    cur->running = 0;
                    snprintf(response, sizeof(response), "Stopped %s\n", cur->id);
                    found = 1;
                    break;
                }
                cur = cur->next;
            }
            if (!found)
                snprintf(response, sizeof(response),
                         "ERROR: container %s not found or already stopped\n",
                         req.container_id);
        }

send_response:
        write(client, response, strlen(response));
        close(client);
    }

    close(server);
    return 0;
}

/* ================= CLIENT ================= */

int send_request(control_request_t *req)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is the supervisor running?)");
        close(sock);
        return 1;
    }

    if (write(sock, req, sizeof(*req)) != (ssize_t)sizeof(*req)) {
        perror("write");
        close(sock);
        return 1;
    }

    char buffer[RESPONSE_SIZE];
    ssize_t n;
    while ((n = read(sock, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[n] = '\0';
        printf("%s", buffer);
    }

    close(sock);
    return 0;
}

/* ================= MAIN ================= */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor\n"
            "  %s start  <id> <rootfs> <command>\n"
            "  %s run    <id> <rootfs> <command>\n"
            "  %s ps\n"
            "  %s logs   <id>\n"
            "  %s stop   <id>\n",
            prog, prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0)
        return run_supervisor();

    control_request_t req = {0};

    if      (strcmp(argv[1], "start") == 0) req.kind = CMD_START;
    else if (strcmp(argv[1], "run")   == 0) req.kind = CMD_RUN;
    else if (strcmp(argv[1], "ps")    == 0) req.kind = CMD_PS;
    else if (strcmp(argv[1], "logs")  == 0) req.kind = CMD_LOGS;
    else if (strcmp(argv[1], "stop")  == 0) req.kind = CMD_STOP;
    else { usage(argv[0]); return 1; }

    if (argc > 2) strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    if (argc > 3) strncpy(req.rootfs,       argv[3], PATH_MAX - 1);
    if (argc > 4) strncpy(req.command,      argv[4], 255);

    req.soft_limit_bytes = 40UL << 20;
    req.hard_limit_bytes = 64UL << 20;

    return send_request(&req);
}