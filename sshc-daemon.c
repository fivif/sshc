/*
 * sshc-daemon.c — SSH multiplexing daemon (cross-platform)
 * Build: cc -O2 -o sshc-daemon sshc-daemon.c          (Unix)
 *        cl /O2 /Fe:sshc-daemon.exe sshc-daemon.c ws2_32.lib  (Windows MSVC)
 *        x86_64-w64-mingw32-gcc -O2 -o sshc-daemon.exe sshc-daemon.c -lws2_32  (cross)
 *
 * Architecture:
 *   Unix:   fork-per-request + AF_UNIX + vfork/posix_spawnp
 *   Windows: thread-per-request + TCP localhost + CreateProcess
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/stat.h>
#endif

#ifdef _WIN32
  typedef int ssize_t;
  #ifndef _WIN32_WINNT
  #define _WIN32_WINNT 0x0600
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <io.h>
  #include <process.h>
  #include <direct.h>
  #include <sys/stat.h>

  #define unlink _unlink
  #define mkdir(p,m) _mkdir(p)
  #define chmod(p,m) do {} while(0)
  #define PATH_SEP "\\"

  typedef SOCKET socket_t;

  #define sock_read(fd,buf,sz)  recv(fd,buf,sz,0)
  #define sock_write(fd,buf,sz) send(fd,buf,sz,0)
  #define sock_close(fd)        closesocket(fd)

  #define WIFEXITED(s)   1
  #define WEXITSTATUS(s) ((s) >> 8)

  static volatile int g_shutdown = 0;

  #define stat _stat

  static const char *get_home(void) {
      static char home[512];
      const char *h = getenv("USERPROFILE");
      if (h) return h;
      const char *hd = getenv("HOMEDRIVE");
      const char *hp = getenv("HOMEPATH");
      if (hd && hp) { snprintf(home, sizeof(home), "%s%s", hd, hp); return home; }
      return "C:\\Users\\Default";
  }

  static void expand_tilde(const char *path, char *out, size_t sz) {
      if (path[0] == '~') {
          snprintf(out, sz, "%s%s", get_home(), path + 1);
      } else {
          strncpy(out, path, sz - 1); out[sz - 1] = 0;
      }
  }

#else
  #define _POSIX_C_SOURCE 200809L
  #include <spawn.h>
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <sys/stat.h>
  #include <sys/wait.h>
  #include <signal.h>
  #include <fcntl.h>

  #define PATH_SEP "/"

  typedef int socket_t;

  #define sock_read(fd,buf,sz)  read(fd,buf,sz)
  #define sock_write(fd,buf,sz) write(fd,buf,sz)
  #define sock_close(fd)        close(fd)

  static const char *get_home(void) { return getenv("HOME"); }

  static void expand_tilde(const char *path, char *out, size_t sz) {
      if (path[0] == '~') {
          const char *h = get_home();
          snprintf(out, sz, "%s%s", h ? h : "", path + 1);
      } else {
          strncpy(out, path, sz - 1); out[sz - 1] = 0;
      }
  }

  extern char **environ;
#endif

#define MAX_PROFILES   32
#define BUF_SIZE       65536
#define SMALL_BUF      4096

static char app_dir[512];
static char socket_dir[512];
#ifdef _WIN32
static char daemon_port_path[512];
static int  daemon_port = 17376;
#else
static char daemon_sock_path[512];
#endif
static char profiles_path[512];

typedef struct {
    char name[64], host[128], user[32], key[512], password[256], proxy[512];
    char allow[512], deny[512];
    int  port;
} Profile;

static Profile profiles[MAX_PROFILES];
static int    profile_count = 0;
static char   default_profile[64] = {0};
static time_t config_mtime = 0;

static void get_sock(const char *name, char *out, size_t sz) {
    snprintf(out, sz, "%s" PATH_SEP "ssh-%s", socket_dir, name);
}

/* ─── SSH check (system call) ──────────────────────────────────────── */
static int ssh_check(const char *sock, const char *target, const char *proxy) {
    char cmd[1024];
    char proxy_part[640] = "";
    if (proxy && proxy[0]) snprintf(proxy_part, sizeof(proxy_part), "-o ProxyCommand=\"%s\" ", proxy);
    snprintf(cmd, sizeof(cmd),
        "ssh %s-o ControlPath=%s -O check %s 2>"
#ifdef _WIN32
        "nul"
#else
        "/dev/null"
#endif
        , proxy_part, sock, target);
    return system(cmd);
}

/* ─── Simple JSON parser ───────────────────────────────────────────── */
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

/* ─── Platform: ensure Master connection ───────────────────────────── */

#ifdef _WIN32

static int ensure_master(int idx) {
    Profile *p = &profiles[idx];
    char sock[512], target[256];
    get_sock(p->name, sock, sizeof(sock));
    snprintf(target, sizeof(target), "%s@%s", p->user, p->host);

    /* check if master is alive */
    if (ssh_check(sock, target, p->proxy) == 0) return 0;
    unlink(sock);

    /* start master */
    int use_pass = p->password[0] != 0;
    int has_proxy = p->proxy[0] != 0;
    char cmdline[2048];
    int off = 0;

    if (use_pass) {
        off += snprintf(cmdline + off, sizeof(cmdline) - off,
            "sshpass -p \"%s\" ", p->password);
    }
    off += snprintf(cmdline + off, sizeof(cmdline) - off,
        "ssh -M -f -N "
        "-o ControlPath=%s "
        "-o ControlPersist=300 "
        "-o Port=%d "
        "-o StrictHostKeyChecking=accept-new "
        "-o ConnectTimeout=10 ",
        sock, p->port);
    if (has_proxy) {
        off += snprintf(cmdline + off, sizeof(cmdline) - off,
            "-o ProxyCommand=\"%s\" ", p->proxy);
    }
    if (!use_pass) {
        off += snprintf(cmdline + off, sizeof(cmdline) - off, "-i \"%s\" ", p->key);
    }
    off += snprintf(cmdline + off, sizeof(cmdline) - off, "%s", target);

    STARTUPINFO si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi;
    if (!CreateProcess(NULL, cmdline, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, 10000);
    DWORD ec;
    GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return ec == 0 ? 0 : -1;
}

/* ─── Platform: SSH exec (CreateProcess + pipes) ──────────────────── */

static int ssh_exec(const char *sock, const char *target,
                     const char *command, int timeout,
                     char *out, size_t out_sz, int use_pass, const char *pass,
                     const char *key, const char *proxy) {
    HANDLE hRead, hWrite;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return -1;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    int has_proxy = proxy && proxy[0];
    char cmdline[4096];
    int off = 0;
    if (use_pass) {
        off += snprintf(cmdline + off, sizeof(cmdline) - off,
            "sshpass -p \"%s\" ", pass);
    }
    off += snprintf(cmdline + off, sizeof(cmdline) - off,
        "ssh -o ControlPath=%s "
        "-o ConnectTimeout=%d "
        "-o StrictHostKeyChecking=accept-new ",
        sock, timeout > 0 ? timeout : 10);
    if (has_proxy) {
        off += snprintf(cmdline + off, sizeof(cmdline) - off,
            "-o ProxyCommand=\"%s\" ", proxy);
    }
    if (!use_pass && key && key[0]) {
        off += snprintf(cmdline + off, sizeof(cmdline) - off, "-i \"%s\" ", key);
    }
    off += snprintf(cmdline + off, sizeof(cmdline) - off, "%s %s", target, command);

    STARTUPINFO si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWrite;
    si.hStdError  = hWrite;

    PROCESS_INFORMATION pi;
    if (!CreateProcess(NULL, cmdline, NULL, NULL, TRUE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hRead); CloseHandle(hWrite);
        return -1;
    }
    CloseHandle(hWrite);

    size_t total = 0;
    char buf[8192];
    DWORD n;
    while (ReadFile(hRead, buf, sizeof(buf), &n, NULL) && n > 0
           && total < out_sz - 1) {
        size_t cp = n;
        if (total + cp >= out_sz - 1) cp = out_sz - 1 - total;
        memcpy(out + total, buf, cp);
        total += cp;
    }
    out[total] = 0;
    CloseHandle(hRead);

    DWORD to = (DWORD)(timeout > 0 ? timeout : 30) * 1000;
    if (WaitForSingleObject(pi.hProcess, to) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
    }
    DWORD ec;
    GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)(ec << 8);  /* encode like WIFEXITED/WEXITSTATUS */
}

#else  /* ─── POSIX implementations ─────────────────────────────────── */

static int ensure_master(int idx) {
    Profile *p = &profiles[idx];
    char sock[512], target[256];
    get_sock(p->name, sock, sizeof(sock));
    snprintf(target, sizeof(target), "%s@%s", p->user, p->host);

    char proxy_opt[640];
    int has_proxy = p->proxy[0] != 0;
    if (has_proxy) snprintf(proxy_opt, sizeof(proxy_opt), "ProxyCommand=%s", p->proxy);

    /* check if master is alive */
    char ctl[256];
    snprintf(ctl, sizeof(ctl), "ControlPath=%s", sock);
    char *check_argv[16] = { "ssh" };
    int ac = 1;
    if (has_proxy) { check_argv[ac++] = "-o"; check_argv[ac++] = proxy_opt; }
    check_argv[ac++] = "-o"; check_argv[ac++] = ctl;
    check_argv[ac++] = "-O"; check_argv[ac++] = "check";
    check_argv[ac++] = target;
    check_argv[ac] = NULL;
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

    /* start master */
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
    if (has_proxy) { start_argv[argc++] = "-o"; start_argv[argc++] = proxy_opt; }
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
                     const char *key, const char *proxy) {
    int pipefd[2];
    if (pipe(pipefd) < 0) return -1;

    char ctl[256], cto[64];
    snprintf(ctl, sizeof(ctl), "ControlPath=%s", sock);
    snprintf(cto, sizeof(cto), "ConnectTimeout=%d", timeout > 0 ? timeout : 10);

    char proxy_opt[640];
    int has_proxy = proxy && proxy[0];

    char *argv[40];
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
    if (has_proxy) {
        snprintf(proxy_opt, sizeof(proxy_opt), "ProxyCommand=%s", proxy);
        argv[argc++] = "-o"; argv[argc++] = proxy_opt;
    }
    if (!use_pass && key && key[0]) {
        argv[argc++] = "-i"; argv[argc++] = (char*)key;
    }
    argv[argc++] = (char*)target;
    argv[argc++] = (char*)command;
    argv[argc] = NULL;

    pid_t pid = vfork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execvp(use_pass ? "sshpass" : "ssh", argv);
        _exit(127);
    }

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

#endif /* _WIN32 / POSIX */

/* ─── Config loading ───────────────────────────────────────────────── */
static int load_profiles(void) {
    FILE *f = fopen(profiles_path, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *json = malloc(sz + 1);
    if (!json) { fclose(f); return -1; }
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
                else if (!strcmp(fn, "key")) expand_tilde(val, p->key, sizeof(p->key));
                else if (!strcmp(fn, "password")) strncpy(p->password, val, sizeof(p->password)-1);
                else if (!strcmp(fn, "proxy")) strncpy(p->proxy, val, sizeof(p->proxy)-1);
                else if (!strcmp(fn, "allow")) strncpy(p->allow, val, sizeof(p->allow)-1);
                else if (!strcmp(fn, "deny")) strncpy(p->deny, val, sizeof(p->deny)-1);
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

/* ─── Request handler ──────────────────────────────────────────────── */

static void maybe_reload_config(void) {
    struct stat st;
    if (stat(profiles_path, &st) == 0 && st.st_mtime > config_mtime) {
        if (load_profiles() == 0) config_mtime = st.st_mtime;
    }
}

#ifdef _WIN32
static int match_glob(const char *pattern, const char *str) {
    char buf[512];
    const char *p = pattern;
    while (*p) {
        const char *end = strchr(p, '|');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        memcpy(buf, p, len); buf[len] = 0;
        /* simple substring match — covers common allow/deny patterns */
        if (strstr(str, buf)) return 1;
        if (!end) break;
        p = end + 1;
    }
    return 0;
}
#else
#include <regex.h>
static int match_regex(const char *pattern, const char *str) {
    regex_t re;
    if (regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB) != 0) return 0;
    int ret = regexec(&re, str, 0, NULL, 0);
    regfree(&re);
    return ret == 0;
}
#define match_glob match_regex
#endif

static int validate_command(const char *cmd, const char *allow, const char *deny,
                             char *err, size_t err_sz) {
    if (!cmd || !cmd[0]) {
        snprintf(err, err_sz, "empty command rejected"); return -1;
    }
    if (deny && deny[0] && match_glob(deny, cmd)) {
        snprintf(err, err_sz, "command matches deny pattern"); return -1;
    }
    if (allow && allow[0] && !match_glob(allow, cmd)) {
        snprintf(err, err_sz, "command does not match allow pattern"); return -1;
    }
    return 0;
}

static void cleanup_masters(void) {
    for (int i = 0; i < profile_count; i++) {
        char sock[512], tgt[256], sc[1024];
        get_sock(profiles[i].name, sock, sizeof(sock));
        snprintf(tgt, sizeof(tgt), "%s@%s", profiles[i].user, profiles[i].host);
        snprintf(sc, sizeof(sc), "ssh -o ControlPath=%s -O exit %s 2>"
#ifdef _WIN32
                 "nul"
#else
                 "/dev/null"
#endif
                 , sock, tgt);
        system(sc);
        unlink(sock);
    }
}

static void handle_request(socket_t fd) {
    maybe_reload_config();
    char buf[BUF_SIZE] = {0};
    ssize_t n = sock_read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) { sock_close(fd); return; }
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
        sock_write(fd, resp, (int)strlen(resp));
        sock_write(fd, "\n", 1);
        sock_close(fd);
        cleanup_masters();
#ifdef _WIN32
        unlink(daemon_port_path);
        g_shutdown = 1;
#else
        unlink(daemon_sock_path);
        _exit(0);
#endif
        return;
    }

    if (!strcmp(action, "health")) {
        if (!profile[0] || !strcmp(profile, "*")) {
            char *p = resp;
            p += snprintf(p, sizeof(resp) - (p - resp), "{");
            int first = 1;
            for (int i = 0; i < profile_count; i++) {
                if (!first) p += snprintf(p, sizeof(resp) - (p - resp), ",");
                first = 0;
                ensure_master(i);
                char sock[512], tgt[256];
                get_sock(profiles[i].name, sock, sizeof(sock));
                snprintf(tgt, sizeof(tgt), "%s@%s", profiles[i].user, profiles[i].host);
                char out[256];
                int st = ssh_exec(sock, tgt, "echo ok", 5, out, sizeof(out),
                    profiles[i].password[0] != 0, profiles[i].password,
                    profiles[i].key, profiles[i].proxy);
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
                    profiles[idx].key, profiles[idx].proxy);
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
            snprintf(resp, sizeof(resp),
                "{\"ok\":false,\"error\":\"unknown profile: %s\"}", profile);
            goto respond;
        }

        char sock[512], tgt[256];
        get_sock(profiles[idx].name, sock, sizeof(sock));
        snprintf(tgt, sizeof(tgt), "%s@%s", profiles[idx].user, profiles[idx].host);

        ensure_master(idx);

        char valid_err[256];
        if (validate_command(cmd_buf, profiles[idx].allow, profiles[idx].deny,
                             valid_err, sizeof(valid_err)) != 0) {
            snprintf(resp, sizeof(resp),
                "{\"ok\":false,\"error\":\"%s\"}", valid_err);
            goto respond;
        }

        char stdout_buf[BUF_SIZE] = {0};
        int use_pass = profiles[idx].password[0] != 0;
        int st = ssh_exec(sock, tgt, cmd_buf, timeout, stdout_buf, sizeof(stdout_buf),
            use_pass, profiles[idx].password, profiles[idx].key, profiles[idx].proxy);

        if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
            unlink(sock);
            ensure_master(idx);
            memset(stdout_buf, 0, sizeof(stdout_buf));
            st = ssh_exec(sock, tgt, cmd_buf, timeout, stdout_buf, sizeof(stdout_buf),
                use_pass, profiles[idx].password, profiles[idx].key, profiles[idx].proxy);
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
            int alive = (ssh_check(sock, tgt, profiles[i].proxy) == 0);
            int is_def = !strcmp(profiles[i].name, default_profile);
            p += snprintf(p, sizeof(resp) - (p - resp),
                "\"%s\":{\"host\":\"%s\",\"user\":\"%s\",\"port\":%d,\"alive\":%s,\"default\":%s,\"proxy\":\"%s\"}",
                profiles[i].name, profiles[i].host, profiles[i].user, profiles[i].port,
                alive ? "true" : "false", is_def ? "true" : "false", profiles[i].proxy);
        }
        p += snprintf(p, sizeof(resp) - (p - resp), "}");
    }
    else if (!strcmp(action, "reconnect")) {
        int idx = -1;
        for (int i = 0; i < profile_count; i++)
            if (!strcmp(profiles[i].name, profile)) { idx = i; break; }
        if (idx < 0) {
            snprintf(resp, sizeof(resp),
                "{\"ok\":false,\"error\":\"unknown profile: %s\"}", profile);
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
        snprintf(resp, sizeof(resp),
            "{\"ok\":false,\"error\":\"unknown action: %s\"}", action);
    }

respond:
    sock_write(fd, resp, (int)strlen(resp));
    sock_write(fd, "\n", 1);
    sock_close(fd);
}

/* ─── Platform: request dispatch (thread vs fork) ──────────────────── */

#ifdef _WIN32
static DWORD WINAPI request_thread(LPVOID arg) {
    socket_t fd = (socket_t)(intptr_t)arg;
    handle_request(fd);
    return 0;
}
#else
static void sig_handler(int sig) {
    (void)sig;
    cleanup_masters();
    unlink(daemon_sock_path);
    _exit(0);
}

static void reap_children(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0);
}
#endif

/* ─── Platform: daemon entry setup & main loop ─────────────────────── */

#ifdef _WIN32

static BOOL ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT) {
        cleanup_masters();
        unlink(daemon_port_path);
        WSACleanup();
        ExitProcess(0);
    }
    return TRUE;
}

static int run_daemon(void) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n"); return 1;
    }

    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    /* find available port */
    SOCKET test = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    sa.sin_port = 0; /* auto-assign */
    if (bind(test, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "bind test failed\n"); WSACleanup(); return 1;
    }
    int namelen = sizeof(sa);
    getsockname(test, (struct sockaddr*)&sa, &namelen);
    daemon_port = ntohs(sa.sin_port);
    closesocket(test);

    /* write port file */
    FILE *pf = fopen(daemon_port_path, "w");
    if (pf) { fprintf(pf, "%d\n", daemon_port); fclose(pf); }

    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET) {
        fprintf(stderr, "socket failed\n"); WSACleanup(); return 1;
    }

    sa.sin_port = htons((u_short)daemon_port);
    if (bind(srv, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
        fprintf(stderr, "bind failed\n"); closesocket(srv); WSACleanup(); return 1;
    }
    if (listen(srv, SOMAXCONN) != 0) {
        fprintf(stderr, "listen failed\n"); closesocket(srv); WSACleanup(); return 1;
    }

    printf("sshc daemon ready (127.0.0.1:%d)\n", daemon_port);

    if (default_profile[0]) {
        for (int i = 0; i < profile_count; i++) {
            if (!strcmp(profiles[i].name, default_profile)) {
                printf("  [%s] %s\n", default_profile,
                    ensure_master(i) == 0 ? "connected" : "failed");
                break;
            }
        }
    }

    while (!g_shutdown) {
        SOCKET cli = accept(srv, NULL, NULL);
        if (cli == INVALID_SOCKET) {
            if (g_shutdown) break;
            continue;
        }
        HANDLE th = CreateThread(NULL, 0, request_thread,
                                 (LPVOID)(intptr_t)cli, 0, NULL);
        if (th == NULL) {
            closesocket(cli);
        } else {
            CloseHandle(th); /* detach — thread runs independently */
        }
    }

    closesocket(srv);
    unlink(daemon_port_path);
    WSACleanup();
    return 0;
}

static int setup_daemon_paths(void) {
    const char *home = get_home();
    if (!home) { fprintf(stderr, "USERPROFILE not set\n"); return 1; }

    snprintf(app_dir, sizeof(app_dir), "%s\\.sshc", home);
    snprintf(socket_dir, sizeof(socket_dir), "%s\\mux", app_dir);
    snprintf(daemon_port_path, sizeof(daemon_port_path), "%s\\daemon.port", app_dir);
    snprintf(profiles_path, sizeof(profiles_path), "%s\\profiles.json", app_dir);
    return 0;
}

#else  /* ─── POSIX ─────────────────────────────────────────────────── */

static int run_daemon(void) {
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, daemon_sock_path, sizeof(addr.sun_path)-1);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
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
            handle_request(cli);
            _exit(0);
        }
        close(cli);
    }

    close(srv);
    unlink(daemon_sock_path);
    return 0;
}

static int setup_daemon_paths(void) {
    const char *home = get_home();
    if (!home) { fprintf(stderr, "HOME not set\n"); return 1; }

    snprintf(app_dir, sizeof(app_dir), "%s/.sshc", home);
    snprintf(socket_dir, sizeof(socket_dir), "%s/mux", app_dir);
    snprintf(daemon_sock_path, sizeof(daemon_sock_path), "%s/daemon.sock", app_dir);
    snprintf(profiles_path, sizeof(profiles_path), "%s/profiles.json", app_dir);
    return 0;
}

#endif /* _WIN32 / POSIX */

/* ─── main ─────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    if (setup_daemon_paths() != 0) return 1;

    mkdir(app_dir, 0700);
    mkdir(socket_dir, 0700);
#ifndef _WIN32
    unlink(daemon_sock_path);
#else
    unlink(daemon_port_path);
#endif

    if (load_profiles() != 0) {
        fprintf(stderr, "sshc-daemon: no profiles found in %s\n", profiles_path);
        return 1;
    }
    { struct stat st; if (stat(profiles_path, &st) == 0) config_mtime = st.st_mtime; }

    return run_daemon();
}
