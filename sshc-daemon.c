/*
 * sshc-daemon.c — 纯 C SSH 复用守护进程 (fork 架构)
 * 编译: cc -O2 -o sshc-daemon sshc-daemon.c
 *
 * 核心优化：
 *   1. exec 路径：pipe+fork+execvp("ssh") + 阻塞 read — 零 shell，零临时文件
 *   2. ensure_master：system() 仅用于建立/检查连接（低频操作，可接受）
 *   3. health：复用 ssh_exec 内联路径
 *
 * 依赖: 仅 POSIX + libc
 */

#define _POSIX_C_SOURCE 200809L
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>

#define MAX_PROFILES   32
#define BUF_SIZE       65536
#define SMALL_BUF      4096

static char app_dir[512];
static char socket_dir[512];
static char daemon_sock_path[512];
static char profiles_path[512];

typedef struct {
    char name[64], host[128], user[32], key[512], password[256];
    int  port;
} Profile;

static Profile profiles[MAX_PROFILES];
static int    profile_count = 0;
static char   default_profile[64] = {0};

static void get_sock(const char *name, char *out, size_t sz) {
    snprintf(out, sz, "%s/ssh-%s.sock", socket_dir, name);
}

extern char **environ;

/* ─── SSH 执行器 (posix_spawnp — 零 shell，零 fork 开销) ─── */

static int ensure_master(int idx) {
    Profile *p = &profiles[idx];
    char sock[512], target[256];
    get_sock(p->name, sock, sizeof(sock));
    snprintf(target, sizeof(target), "%s@%s", p->user, p->host);

    /* 检查 master 是否存活 */
    char ctl[256];
    snprintf(ctl, sizeof(ctl), "ControlPath=%s", sock);
    char *check_argv[] = {
        "ssh", "-o", ctl, "-O", "check", target, NULL
    };
    pid_t pid;
    int dn = open("/dev/null", O_WRONLY);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (dn >= 0) {
        posix_spawn_file_actions_adddup2(&fa, dn, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&fa, dn, STDERR_FILENO);
    }
    int ret = posix_spawnp(&pid, "ssh", &fa, NULL, check_argv, environ);
    if (dn >= 0) close(dn);
    posix_spawn_file_actions_destroy(&fa);
    if (ret == 0) {
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
    }
    unlink(sock);

    /* 启动 master */
    char cport[32], cper[64];
    snprintf(cport, sizeof(cport), "Port=%d", p->port);
    snprintf(cper, sizeof(cper), "ControlPersist=300");

    int use_pass = p->password[0] != 0;
    char *start_argv[32];
    int argc = 0;

    if (use_pass) {
        start_argv[argc++] = "sshpass";
        start_argv[argc++] = "-p";
        start_argv[argc++] = p->password;
    }
    start_argv[argc++] = "ssh";
    start_argv[argc++] = "-M"; start_argv[argc++] = "-f"; start_argv[argc++] = "-N";
    start_argv[argc++] = "-o"; start_argv[argc++] = ctl;
    start_argv[argc++] = "-o"; start_argv[argc++] = cper;
    start_argv[argc++] = "-o"; start_argv[argc++] = cport;
    start_argv[argc++] = "-o"; start_argv[argc++] = "StrictHostKeyChecking=accept-new";
    start_argv[argc++] = "-o"; start_argv[argc++] = "ConnectTimeout=10";
    if (!use_pass) {
        start_argv[argc++] = "-i"; start_argv[argc++] = p->key;
    }
    start_argv[argc++] = target;
    start_argv[argc] = NULL;

    dn = open("/dev/null", O_WRONLY);
    posix_spawn_file_actions_t fa2;
    posix_spawn_file_actions_init(&fa2);
    if (dn >= 0) {
        posix_spawn_file_actions_adddup2(&fa2, dn, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&fa2, dn, STDERR_FILENO);
    }
    ret = posix_spawnp(&pid, use_pass ? "sshpass" : "ssh", &fa2, NULL, start_argv, environ);
    if (dn >= 0) close(dn);
    posix_spawn_file_actions_destroy(&fa2);
    if (ret == 0) {
        int status;
        waitpid(pid, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    return -1;
}

static int ssh_exec(const char *sock, const char *target,
                     const char *command, int timeout,
                     char *out, size_t out_sz, int use_pass, const char *pass,
                     const char *key) {
    int pipefd[2];
    if (pipe(pipefd) < 0) return -1;

    char ctl[256], cto[64];
    snprintf(ctl, sizeof(ctl), "ControlPath=%s", sock);
    snprintf(cto, sizeof(cto), "ConnectTimeout=%d", timeout > 0 ? timeout : 10);

    char *argv[35];
    int argc = 0;
    if (use_pass) {
        argv[argc++] = "sshpass";
        argv[argc++] = "-p";
        argv[argc++] = (char*)pass;
    }
    argv[argc++] = "ssh";
    argv[argc++] = "-o"; argv[argc++] = ctl;
    argv[argc++] = "-o"; argv[argc++] = cto;
    argv[argc++] = "-o"; argv[argc++] = "StrictHostKeyChecking=accept-new";
    if (!use_pass && key && key[0]) {
        argv[argc++] = "-i"; argv[argc++] = (char*)key;
    }
    argv[argc++] = (char*)target;
    argv[argc++] = (char*)command;
    argv[argc] = NULL;

    pid_t pid = vfork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }

    if (pid == 0) {
        /* 子进程: vfork 共享父进程地址空间，必须立即 exec */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execvp(use_pass ? "sshpass" : "ssh", argv);
        _exit(127);
    }

    /* 父进程 */
    close(pipefd[1]);

    size_t total = 0;
    char buf[8192];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0 && total < out_sz - 1) {
        size_t cp = (size_t)n;
        if (total + cp >= out_sz - 1) cp = out_sz - 1 - total;
        memcpy(out + total, buf, cp);
        total += cp;
    }
    out[total] = 0;
    close(pipefd[0]);

    int status;
    waitpid(pid, &status, 0);
    return status;
}

static int ssh_check(const char *sock, const char *target) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "ssh -o ControlPath=%s -O check %s 2>/dev/null", sock, target);
    return system(cmd);
}

/* ─── 简单 JSON ─── */
static int json_get_str(const char *json, const char *key, char *out, size_t sz) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return -1;
    pos = strchr(pos + strlen(search), ':');
    if (!pos) return -1;
    while (*pos == ':' || *pos == ' ') pos++;
    if (*pos != '"') return -1;
    pos++;
    const char *end = strchr(pos, '"');
    if (!end) return -1;
    size_t len = end - pos;
    if (len >= sz) len = sz - 1;
    memcpy(out, pos, len); out[len] = 0;
    return 0;
}

static int json_get_int(const char *json, const char *key, int *val) {
    char buf[32];
    if (json_get_str(json, key, buf, sizeof(buf)) == 0) { *val = atoi(buf); return 0; }
    return -1;
}

static void json_esc(const char *s, char *d, size_t sz) {
    size_t j = 0;
    for (size_t i = 0; s[i] && j < sz - 2; i++) {
        switch (s[i]) {
            case '"':  if (j<sz-3){d[j++]='\\';d[j++]='"'; } break;
            case '\\': if (j<sz-3){d[j++]='\\';d[j++]='\\';} break;
            case '\n': if (j<sz-3){d[j++]='\\';d[j++]='n'; } break;
            case '\r': if (j<sz-3){d[j++]='\\';d[j++]='r'; } break;
            case '\t': if (j<sz-3){d[j++]='\\';d[j++]='t'; } break;
            default:   d[j++] = s[i];
        }
    }
    d[j] = 0;
}

/* ─── 处理连接 (fork 子进程) ─── */
static void handle_client(int fd) {
    char buf[BUF_SIZE] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) { close(fd); _exit(0); }
    buf[n] = 0;

    char action[32]={0}, profile[64]={0}, cmd_buf[BUF_SIZE]={0};
    int timeout = 30;
    json_get_str(buf, "action", action, sizeof(action));
    json_get_str(buf, "profile", profile, sizeof(profile));
    json_get_str(buf, "cmd", cmd_buf, sizeof(cmd_buf));
    json_get_int(buf, "timeout", &timeout);
    if (timeout <= 0) timeout = 30;
    if (!profile[0] && default_profile[0] && !strcmp(action, "exec"))
        strcpy(profile, default_profile);

    char resp[BUF_SIZE * 3];

    if (!strcmp(action, "shutdown")) {
        snprintf(resp, sizeof(resp), "{\"ok\":true}");
        write(fd, resp, strlen(resp)); write(fd, "\n", 1);
        close(fd);
        for (int i = 0; i < profile_count; i++) {
            char sock[512], tgt[256], sc[1024];
            get_sock(profiles[i].name, sock, sizeof(sock));
            snprintf(tgt, sizeof(tgt), "%s@%s", profiles[i].user, profiles[i].host);
            snprintf(sc, sizeof(sc), "ssh -o ControlPath=%s -O exit %s 2>/dev/null", sock, tgt);
            system(sc); unlink(sock);
        }
        unlink(daemon_sock_path);
        _exit(0);
    }

    if (!strcmp(action, "health")) {
        if (!profile[0] || !strcmp(profile, "*")) {
            char *p = resp;
            p += snprintf(p, sizeof(resp) - (p - resp), "{");
            int first = 1;
            for (int i = 0; i < profile_count; i++) {
                if (!first) p += snprintf(p, sizeof(resp) - (p - resp), ","); first = 0;
                ensure_master(i);
                char sock[512], tgt[256];
                get_sock(profiles[i].name, sock, sizeof(sock));
                snprintf(tgt, sizeof(tgt), "%s@%s", profiles[i].user, profiles[i].host);
                char out[256];
                int st = ssh_exec(sock, tgt, "echo ok", 5, out, sizeof(out),
                    profiles[i].password[0] != 0, profiles[i].password,
                    profiles[i].key);
                int alive = WIFEXITED(st) && WEXITSTATUS(st) == 0;
                p += snprintf(p, sizeof(resp) - (p - resp),
                    "\"%s\":{\"alive\":%s}", profiles[i].name, alive ? "true" : "false");
            }
            p += snprintf(p, sizeof(resp) - (p - resp), "}");
        } else {
            int idx = -1;
            for (int i = 0; i < profile_count; i++)
                if (!strcmp(profiles[i].name, profile)) { idx = i; break; }
            if (idx < 0) {
                snprintf(resp, sizeof(resp), "{\"error\":\"unknown profile: %s\"}", profile);
            } else {
                ensure_master(idx);
                char sock[512], tgt[256];
                get_sock(profiles[idx].name, sock, sizeof(sock));
                snprintf(tgt, sizeof(tgt), "%s@%s", profiles[idx].user, profiles[idx].host);
                char out[256];
                int st = ssh_exec(sock, tgt, "echo ok", 5, out, sizeof(out),
                    profiles[idx].password[0] != 0, profiles[idx].password,
                    profiles[idx].key);
                int alive = WIFEXITED(st) && WEXITSTATUS(st) == 0;
                snprintf(resp, sizeof(resp),
                    "{\"profile\":\"%s\",\"alive\":%s}", profile, alive ? "true" : "false");
            }
        }
        goto respond;
    }

    if (!strcmp(action, "exec")) {
        int idx = -1;
        for (int i = 0; i < profile_count; i++)
            if (!strcmp(profiles[i].name, profile)) { idx = i; break; }
        if (idx < 0) {
            snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"unknown profile: %s\"}", profile);
            goto respond;
        }

        /* 热路径不检查 master，直接执行；失败时才重连 */
        char sock[512], tgt[256];
        get_sock(profiles[idx].name, sock, sizeof(sock));
        snprintf(tgt, sizeof(tgt), "%s@%s", profiles[idx].user, profiles[idx].host);

        char stdout_buf[BUF_SIZE] = {0};
        int use_pass = profiles[idx].password[0] != 0;
        int st = ssh_exec(sock, tgt, cmd_buf, timeout, stdout_buf, sizeof(stdout_buf),
            use_pass, profiles[idx].password, profiles[idx].key);

        /* SSH 失败则尝试重连一次 */
        if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
            unlink(sock);
            ensure_master(idx);
            memset(stdout_buf, 0, sizeof(stdout_buf));
            st = ssh_exec(sock, tgt, cmd_buf, timeout, stdout_buf, sizeof(stdout_buf),
                use_pass, profiles[idx].password, profiles[idx].key);
        }
        int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
        int exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;

        char esc_out[BUF_SIZE * 2];
        json_esc(stdout_buf, esc_out, sizeof(esc_out));
        snprintf(resp, sizeof(resp),
            "{\"ok\":%s,\"exit\":%d,\"stdout\":\"%s\",\"stderr\":\"\"}",
            ok ? "true" : "false", exit_code, esc_out);
        goto respond;
    }
    else if (!strcmp(action, "profiles")) {
        char *p = resp;
        p += snprintf(p, sizeof(resp) - (p - resp), "{");
        for (int i = 0; i < profile_count; i++) {
            if (i > 0) p += snprintf(p, sizeof(resp) - (p - resp), ",");
            char sock[512], tgt[256];
            get_sock(profiles[i].name, sock, sizeof(sock));
            snprintf(tgt, sizeof(tgt), "%s@%s", profiles[i].user, profiles[i].host);
            int alive = (ssh_check(sock, tgt) == 0);
            int is_def = !strcmp(profiles[i].name, default_profile);
            p += snprintf(p, sizeof(resp) - (p - resp),
                "\"%s\":{\"host\":\"%s\",\"user\":\"%s\",\"port\":%d,\"alive\":%s,\"default\":%s}",
                profiles[i].name, profiles[i].host, profiles[i].user, profiles[i].port,
                alive ? "true" : "false", is_def ? "true" : "false");
        }
        p += snprintf(p, sizeof(resp) - (p - resp), "}");
    }
    else if (!strcmp(action, "reconnect")) {
        int idx = -1;
        for (int i = 0; i < profile_count; i++)
            if (!strcmp(profiles[i].name, profile)) { idx = i; break; }
        if (idx < 0) {
            snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"unknown profile: %s\"}", profile);
        } else {
            char sock[512];
            get_sock(profiles[idx].name, sock, sizeof(sock));
            unlink(sock);
            int ret = ensure_master(idx);
            snprintf(resp, sizeof(resp),
                "{\"ok\":%s,\"profile\":\"%s\",\"msg\":\"%s\"}",
                ret == 0 ? "true" : "false", profile,
                ret == 0 ? "reconnected" : "reconnect failed");
        }
    }
    else {
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"unknown action: %s\"}", action);
    }

respond:
    write(fd, resp, strlen(resp));
    write(fd, "\n", 1);
    close(fd);
    _exit(0);
}

/* ─── 配置加载 ─── */
static int load_profiles(void) {
    FILE *f = fopen(profiles_path, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *json = malloc(sz + 1);
    fread(json, 1, sz, f); json[sz] = 0; fclose(f);

    json_get_str(json, "default", default_profile, sizeof(default_profile));

    const char *s = strstr(json, "\"servers\"");
    if (!s) { free(json); return -1; }
    s = strchr(s, '{'); if (!s) { free(json); return -1; }
    s++;
    profile_count = 0;

    while (*s && profile_count < MAX_PROFILES) {
        while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t' || *s == ',') s++;
        if (*s == '}') break;
        if (*s != '"') { s++; continue; }
        s++;
        char name[64]; int ni = 0;
        while (*s && *s != '"' && ni < 63) name[ni++] = *s++;
        name[ni] = 0; if (*s == '"') s++;
        while (*s == ' ' || *s == ':') s++;
        if (*s != '{') break; s++;

        Profile *p = &profiles[profile_count];
        memset(p, 0, sizeof(Profile));
        strncpy(p->name, name, sizeof(p->name)-1);
        p->port = 22; strcpy(p->user, "root");

        while (*s && *s != '}') {
            while (*s == ' ' || *s == ',' || *s == '\n' || *s == '\r') s++;
            if (*s == '}') break;
            if (*s != '"') { s++; continue; }
            s++;
            char fn[32]; int fi = 0;
            while (*s && *s != '"' && fi < 31) fn[fi++] = *s++;
            fn[fi] = 0; if (*s == '"') s++;
            while (*s == ' ' || *s == ':') s++;

            if (*s == '"') {
                s++;
                char val[512]; int vi = 0;
                while (*s && *s != '"' && vi < 511) val[vi++] = *s++;
                val[vi] = 0; if (*s == '"') s++;
                if (!strcmp(fn, "host")) strncpy(p->host, val, sizeof(p->host)-1);
                else if (!strcmp(fn, "user")) strncpy(p->user, val, sizeof(p->user)-1);
                else if (!strcmp(fn, "key")) {
                    if (val[0] == '~') {
                        const char *h = getenv("HOME");
                        snprintf(p->key, sizeof(p->key), "%s%s", h ? h : "", val + 1);
                    } else strncpy(p->key, val, sizeof(p->key)-1);
                }
                else if (!strcmp(fn, "password")) strncpy(p->password, val, sizeof(p->password)-1);
            } else if (*s >= '0' && *s <= '9') {
                int v = 0;
                while (*s >= '0' && *s <= '9') v = v*10 + (*s++ - '0');
                if (!strcmp(fn, "port")) p->port = v;
            }
        }
        if (*s == '}') s++;
        if (p->host[0]) profile_count++;
    }
    free(json);
    return profile_count > 0 ? 0 : -1;
}

static void sig_handler(int sig) {
    (void)sig;
    for (int i = 0; i < profile_count; i++) {
        char sock[512], tgt[256], sc[1024];
        get_sock(profiles[i].name, sock, sizeof(sock));
        snprintf(tgt, sizeof(tgt), "%s@%s", profiles[i].user, profiles[i].host);
        snprintf(sc, sizeof(sc), "ssh -o ControlPath=%s -O exit %s 2>/dev/null", sock, tgt);
        system(sc); unlink(sock);
    }
    unlink(daemon_sock_path);
    _exit(0);
}

static void reap_children(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    const char *home = getenv("HOME");
    if (!home) { fprintf(stderr, "HOME not set\n"); return 1; }

    snprintf(app_dir, sizeof(app_dir), "%s/.sshc", home);
    snprintf(socket_dir, sizeof(socket_dir), "%s/mux", app_dir);
    snprintf(daemon_sock_path, sizeof(daemon_sock_path), "%s/daemon.sock", app_dir);
    snprintf(profiles_path, sizeof(profiles_path), "%s/profiles.json", app_dir);

    if (load_profiles() != 0) { fprintf(stderr, "no profiles\n"); return 1; }

    mkdir(app_dir, 0700);
    mkdir(socket_dir, 0700);
    unlink(daemon_sock_path);

    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, daemon_sock_path, sizeof(addr.sun_path)-1);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    chmod(daemon_sock_path, 0600);
    if (listen(srv, 128) < 0) { perror("listen"); return 1; }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, reap_children);

    printf("sshc daemon ready (%s)\n", daemon_sock_path);

    if (default_profile[0]) {
        for (int i = 0; i < profile_count; i++) {
            if (!strcmp(profiles[i].name, default_profile)) {
                printf("  [%s] %s\n", default_profile,
                    ensure_master(i) == 0 ? "connected" : "failed");
                break;
            }
        }
    }

    while (1) {
        int cli = accept(srv, NULL, NULL);
        if (cli < 0) { if (errno == EINTR) continue; break; }

        pid_t pid = fork();
        if (pid == 0) {
            close(srv);
            handle_client(cli);
            _exit(0);
        }
        close(cli);
        if (pid < 0) { close(cli); }
    }

    close(srv);
    unlink(daemon_sock_path);
    return 0;
}
