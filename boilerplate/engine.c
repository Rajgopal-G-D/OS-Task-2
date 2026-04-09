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
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16

/* ===================== TYPES ===================== */

typedef enum { CMD_START, CMD_RUN, CMD_PS, CMD_LOGS, CMD_STOP } command_kind_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head, tail, count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    pid_t pid;
    int running;
    struct container_record *next;
} container_record;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[256];
    int log_write_fd;
} child_config_t;

typedef struct {
    int monitor_fd;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    container_record *containers;
} supervisor_ctx_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[256];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
} control_request_t;

/* ===================== GLOBAL ===================== */

static supervisor_ctx_t *global_ctx = NULL;

/* ===================== BUFFER ===================== */

int bounded_buffer_init(bounded_buffer_t *b)
{
    memset(b, 0, sizeof(*b));
    pthread_mutex_init(&b->mutex, NULL);
    pthread_cond_init(&b->not_empty, NULL);
    pthread_cond_init(&b->not_full, NULL);
    return 0;
}

int bounded_buffer_push(bounded_buffer_t *b, const log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);

    while (b->count == LOG_BUFFER_CAPACITY && !b->shutting_down)
        pthread_cond_wait(&b->not_full, &b->mutex);

    if (b->shutting_down) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }

    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
    b->count++;

    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

int bounded_buffer_pop(bounded_buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);

    while (b->count == 0 && !b->shutting_down)
        pthread_cond_wait(&b->not_empty, &b->mutex);

    if (b->count == 0 && b->shutting_down) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }

    *item = b->items[b->head];
    b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
    b->count--;

    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/* ===================== LOGGER ===================== */

void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = arg;
    log_item_t item;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);

        int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd >= 0) {
            write(fd, item.data, item.length);
            close(fd);
        }
    }
    return NULL;
}

/* ===================== SIGNAL HANDLING ===================== */

void handle_sigchld(int sig)
{
    (void)sig;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        container_record *cur = global_ctx->containers;
        while (cur) {
            if (cur->pid == pid) {
                cur->running = 0;
                break;
            }
            cur = cur->next;
        }
    }
}

void handle_shutdown(int sig)
{
    (void)sig;
    printf("Shutting down supervisor...\n");

    container_record *cur = global_ctx->containers;
    while (cur) {
        if (cur->running)
            kill(cur->pid, SIGKILL);
        cur = cur->next;
    }

    exit(0);
}

/* ===================== CHILD ===================== */

int child_fn(void *arg)
{
    child_config_t *cfg = arg;

    sethostname(cfg->id, strlen(cfg->id));
    chdir(cfg->rootfs);
    chroot(cfg->rootfs);

    mkdir("/proc", 0555);
    mount("proc", "/proc", "proc", 0, NULL);

    dup2(cfg->log_write_fd, STDOUT_FILENO);
    dup2(cfg->log_write_fd, STDERR_FILENO);

    execl("/bin/sh", "sh", "-c", cfg->command, NULL);

    perror("exec failed");
    return 1;
}

/* ===================== SUPERVISOR ===================== */

static int run_supervisor()
{
    supervisor_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    global_ctx = &ctx;

    mkdir(LOG_DIR, 0755);

    signal(SIGCHLD, handle_sigchld);
    signal(SIGINT, handle_shutdown);
    signal(SIGTERM, handle_shutdown);

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    bounded_buffer_init(&ctx.log_buffer);
    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);

    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    unlink(CONTROL_PATH);
    bind(server, (struct sockaddr *)&addr, sizeof(addr));
    listen(server, 5);

    printf("Supervisor running...\n");

    while (1) {
        int client = accept(server, NULL, NULL);
        control_request_t req;
        read(client, &req, sizeof(req));

        if (req.kind == CMD_PS) {
            container_record *cur = ctx.containers;
            while (cur) {
                printf("%s PID=%d %s\n",
                       cur->id,
                       cur->pid,
                       cur->running ? "RUNNING" : "STOPPED");
                cur = cur->next;
            }
        }

        else if (req.kind == CMD_STOP) {
            container_record *cur = ctx.containers;
            while (cur) {
                if (strcmp(cur->id, req.container_id) == 0 && cur->running) {
                    kill(cur->pid, SIGTERM);
                    cur->running = 0;
                    break;
                }
                cur = cur->next;
            }
        }

        else if (req.kind == CMD_LOGS) {
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req.container_id);
            system((char[]){0}); // simple placeholder
            char cmd[PATH_MAX + 10];
            snprintf(cmd, sizeof(cmd), "cat %s", path);
            system(cmd);
        }

        else if (req.kind == CMD_START || req.kind == CMD_RUN) {

            int pipefd[2];
            pipe(pipefd);

            child_config_t cfg;
            strcpy(cfg.id, req.container_id);
            strcpy(cfg.rootfs, req.rootfs);
            strcpy(cfg.command, req.command);
            cfg.log_write_fd = pipefd[1];

            void *stack = malloc(STACK_SIZE);

            pid_t pid = clone(child_fn, stack + STACK_SIZE,
                              CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                              &cfg);

            struct monitor_request mreq = {
                .pid = pid,
                .soft_limit_bytes = req.soft_limit_bytes,
                .hard_limit_bytes = req.hard_limit_bytes
            };
            strcpy(mreq.container_id, req.container_id);
            ioctl(ctx.monitor_fd, MONITOR_REGISTER, &mreq);

            container_record *node = malloc(sizeof(*node));
            strcpy(node->id, req.container_id);
            node->pid = pid;
            node->running = 1;
            node->next = ctx.containers;
            ctx.containers = node;

            close(pipefd[1]);

            if (req.kind == CMD_RUN) {
                waitpid(pid, NULL, 0);
                node->running = 0;
            }

            char buf[LOG_CHUNK_SIZE];
            int n;
            while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
                log_item_t item;
                strcpy(item.container_id, req.container_id);
                memcpy(item.data, buf, n);
                item.length = n;
                bounded_buffer_push(&ctx.log_buffer, &item);
            }
        }

        close(client);
    }
}

/* ===================== CLIENT ===================== */

int send_request(control_request_t *req)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    write(sock, req, sizeof(*req));
    close(sock);
    return 0;
}

/* ===================== MAIN ===================== */

int main(int argc, char *argv[])
{
    if (argc < 2) return 1;

    if (strcmp(argv[1], "supervisor") == 0)
        return run_supervisor();

    control_request_t req = {0};

    if (strcmp(argv[1], "start") == 0) req.kind = CMD_START;
    else if (strcmp(argv[1], "run") == 0) req.kind = CMD_RUN;
    else if (strcmp(argv[1], "ps") == 0) req.kind = CMD_PS;
    else if (strcmp(argv[1], "logs") == 0) req.kind = CMD_LOGS;
    else if (strcmp(argv[1], "stop") == 0) req.kind = CMD_STOP;

    if (argc >= 3) strcpy(req.container_id, argv[2]);
    if (argc >= 4) strcpy(req.rootfs, argv[3]);
    if (argc >= 5) strcpy(req.command, argv[4]);

    req.soft_limit_bytes = 40UL << 20;
    req.hard_limit_bytes = 64UL << 20;

    return send_request(&req);
}