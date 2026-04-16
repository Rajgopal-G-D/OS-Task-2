#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ================= UI COLORS ================= */
#define RESET  "\033[0m"
#define GREEN  "\033[32m"
#define RED    "\033[31m"
#define YELLOW "\033[33m"
#define CYAN   "\033[36m"
#define BOLD   "\033[1m"

/* ================= CONFIG ================= */
#define SOCK_PATH      "/tmp/mini_runtime.sock"
#define LOG_DIR        "logs"
#define STACK_SIZE     (1024 * 1024)
#define MAX_CONTAINERS 64
#define ID_LEN         32

/* ================= COMMANDS ================= */
enum {
    CMD_START = 1,
    CMD_PS,
    CMD_STOP,
    CMD_LOGS,
    CMD_RUN
};

/* ================= STRUCTS ================= */
typedef struct {
    int  cmd;
    char id[ID_LEN];
    char rootfs[PATH_MAX];
    char command[256];
    int  soft_mib;
    int  hard_mib;
    int  graceful;
} request_t;

typedef struct {
    int  status;   /* 0=data  1=error  2=end-sentinel */
    char msg[512];
} response_t;

/*
 * child_args_t MUST be a separate heap allocation from the clone stack.
 *
 * Why: clone() receives the TOP of the stack buffer as the child's initial
 * stack pointer and the stack grows DOWNWARD.  If we embedded args at the
 * bottom (low address) of the same buffer, the child's own call frames
 * would overwrite them long before execl() is reached.
 */
typedef struct {
    char id[ID_LEN];
    char rootfs[PATH_MAX];
    char cmd[256];
    int  err_pipe_wr;  /* write-end of error-reporting pipe (O_CLOEXEC) */
} child_args_t;

typedef struct {
    char         id[ID_LEN];
    pid_t        pid;
    time_t       start;
    int          alive;
    int          soft_mib;
    int          hard_mib;
    int          run_fd;   /* kept open for CMD_RUN foreground wait */
    void        *stack;    /* freed in sigchld_handler after child exits */
    child_args_t *args;    /* freed in sigchld_handler after child exits */
} container_t;

/* ================= GLOBALS ================= */
static container_t containers[MAX_CONTAINERS];
static int         container_count = 0;
static int         server_sock     = -1;

/* ================= HELPERS ================= */
static container_t *find_container(const char *id) {
    for (int i = 0; i < container_count; i++)
        if (!strcmp(containers[i].id, id))
            return &containers[i];
    return NULL;
}

static void fmt_uptime(time_t start, char *out, size_t len) {
    int t = (int)(time(NULL) - start);
    snprintf(out, len, "%02d:%02d:%02d", t / 3600, (t % 3600) / 60, t % 60);
}

static void send_response(int fd, int status, const char *msg) {
    response_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.status = status;
    snprintf(resp.msg, sizeof(resp.msg), "%s", msg);
    write(fd, &resp, sizeof(resp));
}

/* ================= CHILD PROCESS ================= */

/*
 * Report an error over the pipe and return 1.
 * Because err_pipe_wr is O_CLOEXEC, a successful execl() closes it
 * automatically; the parent's read() then returns 0 bytes = success.
 */
#define CHILD_ERR(msg) do {                                  \
        const char *_m = (msg);                              \
        write(c->err_pipe_wr, _m, strlen(_m));               \
        return 1;                                            \
    } while (0)

static int child_fn(void *arg) {
    child_args_t *c = (child_args_t *)arg;

    if (sethostname(c->id, strlen(c->id)) < 0)
        CHILD_ERR("sethostname failed");

    if (chroot(c->rootfs) != 0)
        CHILD_ERR("chroot failed: check rootfs path exists");

    if (chdir("/") != 0)
        CHILD_ERR("chdir / failed after chroot");

    /* /proc may already exist in the rootfs */
    if (mkdir("/proc", 0555) < 0 && errno != EEXIST)
        CHILD_ERR("mkdir /proc failed");

    if (mount("proc", "/proc", "proc", 0, NULL) != 0)
        CHILD_ERR("mount /proc failed: need CAP_SYS_ADMIN");

    execl("/bin/sh", "sh", "-c", c->cmd, NULL);
    CHILD_ERR("execl /bin/sh failed: /bin/sh missing in rootfs");
    return 1;
}
#undef CHILD_ERR

/* ================= SIGNAL HANDLERS ================= */

static void sigchld_handler(int sig) {
    (void)sig;
    int   saved_errno = errno;
    int   wstatus;
    pid_t pid;

    while ((pid = waitpid(-1, &wstatus, WNOHANG)) > 0) {
        for (int i = 0; i < container_count; i++) {
            if (containers[i].pid != pid)
                continue;

            containers[i].alive = 0;

            /*
             * Child is fully gone — now safe to release its stack and args.
             */
            free(containers[i].stack);
            containers[i].stack = NULL;
            free(containers[i].args);
            containers[i].args = NULL;

            int code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
            printf(YELLOW "[EXIT]" RESET
                   " container=%s pid=%d exit_code=%d\n",
                   containers[i].id, pid, code);
            fflush(stdout);

            if (containers[i].run_fd != -1) {
                response_t resp;
                memset(&resp, 0, sizeof(resp));
                resp.status = 0;
                snprintf(resp.msg, sizeof(resp.msg),
                         "Container %s exited (code=%d)\n",
                         containers[i].id, code);
                write(containers[i].run_fd, &resp, sizeof(resp));
                close(containers[i].run_fd);
                containers[i].run_fd = -1;
            }
        }
    }
    errno = saved_errno;
}

static void shutdown_handler(int sig) {
    (void)sig;
    printf(BOLD YELLOW
           "\n[SHUTDOWN] stopping all containers...\n" RESET);
    fflush(stdout);

    for (int i = 0; i < container_count; i++)
        if (containers[i].alive)
            kill(containers[i].pid, SIGTERM);

    time_t deadline = time(NULL) + 3;
    while (time(NULL) < deadline) {
        int any = 0;
        for (int i = 0; i < container_count; i++)
            if (containers[i].alive) { any = 1; break; }
        if (!any) break;
        usleep(100000);
    }

    for (int i = 0; i < container_count; i++) {
        if (containers[i].alive) {
            printf(YELLOW "[FORCE-KILL]" RESET " %s\n", containers[i].id);
            kill(containers[i].pid, SIGKILL);
        }
    }

    if (server_sock != -1) close(server_sock);
    unlink(SOCK_PATH);
    printf(GREEN "[SUPERVISOR] clean exit.\n" RESET);
    fflush(stdout);
    _exit(0);
}

/* ================= COMMAND HANDLERS ================= */

static void handle_start(int client_fd, const request_t *r, int foreground) {
    if (container_count >= MAX_CONTAINERS) {
        send_response(client_fd, 1, "Too many containers");
        if (!foreground) close(client_fd);
        return;
    }
    if (find_container(r->id)) {
        send_response(client_fd, 1, "Container ID already exists");
        if (!foreground) close(client_fd);
        return;
    }

    int epipe[2];
    if (pipe2(epipe, O_CLOEXEC) < 0) {
        send_response(client_fd, 1, "pipe2 failed");
        if (!foreground) close(client_fd);
        return;
    }

    void *stack = malloc(STACK_SIZE);
    if (!stack) {
        close(epipe[0]); close(epipe[1]);
        send_response(client_fd, 1, "malloc stack failed");
        if (!foreground) close(client_fd);
        return;
    }

    child_args_t *ch = malloc(sizeof(child_args_t));
    if (!ch) {
        free(stack);
        close(epipe[0]); close(epipe[1]);
        send_response(client_fd, 1, "malloc args failed");
        if (!foreground) close(client_fd);
        return;
    }

    snprintf(ch->id,     sizeof(ch->id),     "%s", r->id);
    snprintf(ch->rootfs, sizeof(ch->rootfs), "%s", r->rootfs);

    /* ==================== ONLY CHANGE ==================== */
    if (foreground)
        snprintf(ch->cmd, sizeof(ch->cmd), "%s", r->command);
    else
        snprintf(ch->cmd, sizeof(ch->cmd), "%s; while true; do sleep 1000;done", r->command);
    /* ==================================================== */

    ch->err_pipe_wr = epipe[1];

    pid_t pid = clone(child_fn,
                      (char *)stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      ch);

    close(epipe[1]);

    if (pid < 0) {
        char err[160];
        snprintf(err, sizeof(err), "clone failed: %s", strerror(errno));
        free(stack); free(ch);
        close(epipe[0]);
        send_response(client_fd, 1, err);
        if (!foreground) close(client_fd);
        return;
    }

    char child_err[256] = {0};
    ssize_t nr = read(epipe[0], child_err, sizeof(child_err) - 1);
    close(epipe[0]);

    if (nr > 0) {
        waitpid(pid, NULL, 0);
        free(stack); free(ch);
        char err[320];
        snprintf(err, sizeof(err), "Container failed to start: %s", child_err);
        send_response(client_fd, 1, err);
        if (!foreground) close(client_fd);
        return;
    }

    container_t *ct = &containers[container_count++];
    memset(ct, 0, sizeof(*ct));
    snprintf(ct->id, sizeof(ct->id), "%s", r->id);
    ct->pid      = pid;
    ct->start    = time(NULL);
    ct->alive    = 1;
    ct->soft_mib = r->soft_mib;
    ct->hard_mib = r->hard_mib;
    ct->run_fd   = foreground ? client_fd : -1;
    ct->stack    = stack;
    ct->args     = ch;

    printf(GREEN "[OK]" RESET
           " started container=%s pid=%d soft=%dMiB hard=%dMiB\n",
           r->id, pid, r->soft_mib, r->hard_mib);
    fflush(stdout);

    if (!foreground) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Started %s (pid=%d)\n", r->id, pid);
        send_response(client_fd, 0, msg);
        close(client_fd);
    }
}

static void handle_ps(int client_fd) {
    char buf[4096];
    int  off = 0;

    off += snprintf(buf + off, sizeof(buf) - off,
                    "%-12s %-8s %-10s %-10s %-10s %-10s\n",
                    "ID", "PID", "STATE", "UPTIME", "SOFT_MiB", "HARD_MiB");

    for (int i = 0; i < container_count; i++) {
        char up[32];
        fmt_uptime(containers[i].start, up, sizeof(up));
        off += snprintf(buf + off, sizeof(buf) - off,
                        "%-12s %-8d %-10s %-10s %-10d %-10d\n",
                        containers[i].id,
                        containers[i].pid,
                        containers[i].alive ? "RUNNING" : "STOPPED",
                        up,
                        containers[i].soft_mib,
                        containers[i].hard_mib);
    }

    send_response(client_fd, 0, buf);
    close(client_fd);
}

static void handle_stop(int client_fd, const request_t *r) {
    container_t *ct = find_container(r->id);
    if (!ct) {
        send_response(client_fd, 1, "Container not found");
        close(client_fd);
        return;
    }
    if (!ct->alive) {
        send_response(client_fd, 1, "Container already stopped");
        close(client_fd);
        return;
    }

    if (r->graceful) {
        /* Graceful path: SIGTERM, wait up to 5 s, SIGKILL fallback */
        kill(ct->pid, SIGTERM);
        printf(YELLOW "[STOP]" RESET
               " SIGTERM -> %s (pid=%d), waiting up to 5s\n",
               r->id, ct->pid);
        fflush(stdout);

        time_t deadline = time(NULL) + 5;
        while (ct->alive && time(NULL) < deadline)
            usleep(100000);

        if (ct->alive) {
            kill(ct->pid, SIGKILL);
            printf(YELLOW "[STOP]" RESET " SIGKILL fallback -> %s\n", r->id);
            fflush(stdout);
        }
    } else {
        /* Forced path: immediate SIGKILL */
        kill(ct->pid, SIGKILL);
        printf(YELLOW "[STOP]" RESET " SIGKILL -> %s (pid=%d)\n",
               r->id, ct->pid);
        fflush(stdout);
    }

    ct->alive = 0;

    char msg[64];
    snprintf(msg, sizeof(msg), "Stopped %s\n", r->id);
    send_response(client_fd, 0, msg);
    close(client_fd);
}

static void handle_logs(int client_fd, const request_t *r) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, r->id);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        char err[128];
        snprintf(err, sizeof(err), "No log file for %s", r->id);
        send_response(client_fd, 1, err);
        close(client_fd);
        return;
    }

    char    chunk[480];
    ssize_t n;
    while ((n = read(fd, chunk, sizeof(chunk) - 1)) > 0) {
        chunk[n] = '\0';
        send_response(client_fd, 0, chunk);
    }
    close(fd);

    send_response(client_fd, 2, ""); /* EOF sentinel */
    close(client_fd);
}

/* ================= SUPERVISOR MAIN LOOP ================= */
static void supervisor(void) {
    mkdir(LOG_DIR, 0755);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    sa.sa_handler = shutdown_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);

    server_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_sock < 0) { perror("socket"); exit(1); }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    unlink(SOCK_PATH);
    if (bind(server_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    if (listen(server_sock, 10) < 0) {
        perror("listen"); exit(1);
    }

    printf(BOLD CYAN "[SUPERVISOR STARTED] listening on %s\n" RESET, SOCK_PATH);
    fflush(stdout);

    while (1) {
        int client_fd = accept(server_sock, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        request_t r;
        memset(&r, 0, sizeof(r));
        ssize_t n = read(client_fd, &r, sizeof(r));
        if (n != (ssize_t)sizeof(r)) {
            close(client_fd);
            continue;
        }

        switch (r.cmd) {
            case CMD_START: handle_start(client_fd, &r, 0); break;
            case CMD_RUN:   handle_start(client_fd, &r, 1); break;
            case CMD_PS:    handle_ps(client_fd);            break;
            case CMD_STOP:  handle_stop(client_fd, &r);      break;
            case CMD_LOGS:  handle_logs(client_fd, &r);      break;
            default:
                send_response(client_fd, 1, "Unknown command");
                close(client_fd);
        }
    }
}

/* ================= CLIENT ================= */
static void send_request(request_t *r) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); exit(1); }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, RED "Cannot connect to supervisor at %s: %s\n" RESET,
                SOCK_PATH, strerror(errno));
        exit(1);
    }

    if (write(s, r, sizeof(*r)) != (ssize_t)sizeof(*r)) {
        perror("write"); exit(1);
    }

    response_t resp;
    ssize_t n;
    while ((n = read(s, &resp, sizeof(resp))) == (ssize_t)sizeof(resp)) {
        if (resp.status == 1)
            fprintf(stderr, RED "[ERROR] %s\n" RESET, resp.msg);
        else if (resp.status == 0 && resp.msg[0] != '\0')
            printf("%s", resp.msg);

        if (resp.status == 2) break;
    }

    close(s);
}

/* ================= USAGE ================= */
static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s supervisor\n"
        "  %s start <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N]\n"
        "  %s run   <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N]\n"
        "  %s ps\n"
        "  %s stop  <id> [--graceful]\n"
        "  %s logs  <id>\n",
        prog, prog, prog, prog, prog, prog);
}

/* ================= MAIN ================= */
int main(int argc, char *argv[]) {
    if (argc < 2) { usage(argv[0]); return 1; }

    if (!strcmp(argv[1], "supervisor")) {
        supervisor();
        return 0;
    }

    request_t r;
    memset(&r, 0, sizeof(r));

    if      (!strcmp(argv[1], "start")) r.cmd = CMD_START;
    else if (!strcmp(argv[1], "run"))   r.cmd = CMD_RUN;
    else if (!strcmp(argv[1], "ps"))    r.cmd = CMD_PS;
    else if (!strcmp(argv[1], "stop"))  r.cmd = CMD_STOP;
    else if (!strcmp(argv[1], "logs"))  r.cmd = CMD_LOGS;
    else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        usage(argv[0]);
        return 1;
    }

    if (r.cmd != CMD_PS) {
        if (argc < 3) { usage(argv[0]); return 1; }
        snprintf(r.id, sizeof(r.id), "%s", argv[2]);
    }

    if (r.cmd == CMD_START || r.cmd == CMD_RUN) {
        if (argc < 5) { usage(argv[0]); return 1; }
        snprintf(r.rootfs,  sizeof(r.rootfs),  "%s", argv[3]);
        snprintf(r.command, sizeof(r.command), "%s", argv[4]);

        for (int i = 5; i < argc - 1; i++) {
            if (!strcmp(argv[i], "--soft-mib"))
                r.soft_mib = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--hard-mib"))
                r.hard_mib = atoi(argv[++i]);
        }
    }

    if (r.cmd == CMD_STOP) {
        for (int i = 3; i < argc; i++)
            if (!strcmp(argv[i], "--graceful"))
                r.graceful = 1;
    }

    send_request(&r);
    return 0;
}
