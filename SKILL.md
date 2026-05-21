---
name: sshc
description: >-
  Universal SSH multiplexing CLI for AI agents. Zero-overhead C daemon with
  ControlMaster connection reuse eliminates repeated authentication latency.
  Trigger: remote server management, batch ops, SSH connection acceleration,
  executing commands on remote servers, VPS administration, server health
  checks, multi-server orchestration. Use whenever the user asks to run
  commands on a remote server, check server status, manage VPS instances,
  or perform DevOps tasks.
---

# sshc — Agent-Native SSH Multiplexing

Pure C daemon architecture (cross-platform: Unix + Windows). System deps only (python3 + nc + ssh). Single-file distributable. SSH ControlMaster persistent multiplexing eliminates repeated auth delay.

## Execution Rules

1. Ensure daemon is alive before any exec: `./sshc health > /dev/null 2>&1 || ./sshc daemon &`
2. All remote commands go through: `./sshc exec <profile> "<command>"`
3. Independent commands can run in parallel
4. Config contains secrets — ensure `chmod 600 ~/.sshc/profiles.json`
5. Config changes are picked up automatically (hot reload via mtime detection — no daemon restart needed)

## Quick Start

```bash
# 1. Compile daemon
cc -O2 -o sshc-daemon sshc-daemon.c       # Unix
# x86_64-w64-mingw32-gcc -O2 -o sshc-daemon.exe sshc-daemon.c -lws2_32  # Windows cross-compile

# 2. Ensure executable
chmod +x sshc sshc-daemon sshc-web

# 3. Create config
mkdir -p ~/.sshc
cat > ~/.sshc/profiles.json << 'EOF'
{
  "default": "my-server",
  "servers": {
    "my-server": {
      "host": "1.2.3.4",
      "user": "root",
      "key": "~/.ssh/id_ed25519",
      "port": 22
    }
  }
}
EOF

# 4. Start daemon
./sshc daemon

# 5. Health check
./sshc health

# 6. Execute remote command
./sshc exec my-server "uptime"

# 7. (Optional) Launch web UI for visual management
./sshc web
# Open http://127.0.0.1:17375
```

## Commands

```bash
sshc daemon                              # Start daemon
sshc health [profile]                    # Health check (omit for all, "*" = all)
sshc exec <profile> <cmd> [timeout]      # Execute command (default timeout: 30s)
sshc profiles                            # List all servers and status
sshc add <name> <user@host> -i <key> [-p port] [--proxy <cmd>] [--allow <re>] [--deny <re>]  # Add server
sshc remove <name>                       # Remove server
sshc default <name>                      # Set default server
sshc reconnect <profile>                 # Force reconnect (kill + re-establish ControlMaster)
sshc upload <profile> <local> <remote>   # Upload file via SFTP (dirs auto tar-pipe)
sshc download <profile> <remote> <local> # Download file via SFTP (dirs auto tar-pipe)
sshc web [port]                          # Start web UI (default :17375)
```

### Add server examples

```bash
# Key auth
./sshc add prod root@10.0.0.1 -i ~/.ssh/id_rsa

# Key auth with custom port + proxy + command safety
./sshc add prod root@10.0.0.1 -i ~/.ssh/id_rsa -p 2222 \
  --proxy "nc -X 5 -x 127.0.0.1:1080 %h %p" \
  --allow "^(ls|df|uptime|systemctl|apt|docker).*" \
  --deny ".*rm -rf.*|.*mkfs.*"

# Password auth (server must be added via Web UI or direct JSON edit for password)
```

## Web UI & REST API

`./sshc web` starts a management dashboard at `http://127.0.0.1:17375`. Features:

- **Drag-and-drop key upload** — drag private key file onto drop zone, auto base64-encodes and stores to `~/.sshc/keys/<profile>/<filename>`
- **Proxy modal** — per-server proxy config with type dropdown (HTTP / SOCKS5 / SOCKS4 / custom ProxyCommand), accessible via "代理" button in server list
- **Async health checks** — table renders instantly, health status streams in via parallel background checks (pulsing gray dot = checking)
- **Live status** — click "测试" to re-check individual server, status dot updates in real-time

### API Endpoints

```bash
# ── List all servers (instant, no health check — alive: null) ──────────
curl http://127.0.0.1:17375/api/profiles
# Response: {"default":"prod","profiles":{"prod":{"host":"1.2.3.4","user":"root","port":22,"alive":null,"has_key":true,"has_password":false,"proxy":"nc -X 5 -x ..."}}}

# ── Add server (key path) ──────────────────────────────────────────────
curl -X POST http://127.0.0.1:17375/api/profiles \
  -H 'Content-Type: application/json' \
  -d '{"name":"prod","host":"1.2.3.4","user":"root","key":"~/.ssh/id_rsa"}'

# ── Add server (key content — base64, from file upload) ─────────────────
curl -X POST http://127.0.0.1:17375/api/profiles \
  -H 'Content-Type: application/json' \
  -d '{"name":"prod","host":"1.2.3.4","user":"root","key_content":"<base64>","key_filename":"id_rsa"}'

# ── Add server (password auth) ─────────────────────────────────────────
curl -X POST http://127.0.0.1:17375/api/profiles \
  -H 'Content-Type: application/json' \
  -d '{"name":"staging","host":"5.6.7.8","user":"admin","password":"mypass","port":2222}'

# ── Add server (with proxy) ────────────────────────────────────────────
curl -X POST http://127.0.0.1:17375/api/profiles \
  -H 'Content-Type: application/json' \
  -d '{"name":"hidden","host":"10.0.0.5","user":"root","key":"~/.ssh/id_rsa","proxy":"nc -X 5 -x 127.0.0.1:1080 %h %p"}'

# ── Update proxy ───────────────────────────────────────────────────────
curl -X PUT http://127.0.0.1:17375/api/profiles/hidden \
  -H 'Content-Type: application/json' \
  -d '{"proxy":"nc -X connect -x proxy.internal:3128 %h %p"}'

# ── Clear proxy ────────────────────────────────────────────────────────
curl -X PUT http://127.0.0.1:17375/api/profiles/hidden \
  -H 'Content-Type: application/json' \
  -d '{"proxy":null}'

# ── Set default server ─────────────────────────────────────────────────
curl -X PUT http://127.0.0.1:17375/api/profiles/prod -d '{"default":true}'

# ── Test connection (blocking — calls daemon health check) ─────────────
curl -X POST http://127.0.0.1:17375/api/test/prod
# Response: {"ok":true,"name":"prod","alive":true}

# ── Delete server ──────────────────────────────────────────────────────
curl -X DELETE http://127.0.0.1:17375/api/profiles/staging
```

### Proxy Types (Web UI modal generates these automatically)

| Type | Generated ProxyCommand |
|------|----------------------|
| HTTP | `nc -X connect -x {host}:{port} %h %p` |
| SOCKS5 | `nc -X 5 -x {host}:{port} %h %p` |
| SOCKS4 | `nc -X 4 -x {host}:{port} %h %p` |
| Custom | user-provided raw ProxyCommand string |

For agents adding servers programmatically, use the `proxy` field directly with any valid OpenSSH ProxyCommand string.

## Configuration

`~/.sshc/profiles.json`:

```json
{
  "default": "prod",
  "servers": {
    "prod": {
      "host": "10.0.0.1",
      "user": "root",
      "key": "~/.ssh/prod.key",
      "port": 22
    },
    "staging": {
      "host": "10.0.0.2",
      "user": "admin",
      "password": "secret",
      "port": 2222
    },
    "hidden": {
      "host": "10.0.0.5",
      "user": "root",
      "key": "~/.ssh/id_rsa",
      "port": 22,
      "proxy": "nc -X 5 -x 127.0.0.1:1080 %h %p"
    },
    "safe-prod": {
      "host": "10.0.0.1",
      "user": "root",
      "key": "~/.ssh/id_rsa",
      "port": 22,
      "allow": "^(ls|df|uptime|systemctl|apt|docker).*",
      "deny": ".*rm -rf.*|.*mkfs.*|.*dd if=.*"
    }
  }
}
```

- `default` — default server name (used when profile omitted in exec)
- `servers.<name>.host` — IP or domain **(required)**
- `servers.<name>.user` — SSH user (default: `root`)
- `servers.<name>.key` — SSH private key path on disk (mutually exclusive with `password`)
- `servers.<name>.password` — plaintext password (mutually exclusive with `key`)
- `servers.<name>.port` — SSH port (default: `22`)
- `servers.<name>.proxy` — OpenSSH ProxyCommand string (optional, default: none / direct connect)
- `servers.<name>.allow` — POSIX regex for allowed commands (optional, default: allow all)
- `servers.<name>.deny` — POSIX regex for denied commands (optional, default: deny none)

### Command Safety

Per-server command validation via POSIX extended regex (Unix) / glob matching (Windows). Rules are evaluated as: **deny takes priority**. If `deny` matches → rejected. If `allow` is set but doesn't match → rejected.

```bash
# Add with command restrictions
./sshc add safe-prod root@10.0.0.1 -i ~/.ssh/id_rsa \
  --allow "^(ls|df|uptime|systemctl).*" \
  --deny ".*rm -rf.*"

# Update via API
curl -X PUT http://127.0.0.1:17375/api/profiles/safe-prod \
  -H 'Content-Type: application/json' \
  -d '{"allow":"^(ls|df|uptime|systemctl|apt).*"}'
```

Validation happens in the C daemon before any command reaches SSH — no way to bypass at the CLI or Web UI level.

### SFTP File Transfer

File upload/download via OpenSSH `sftp` reusing the existing ControlMaster connection (no extra auth). Directories are auto-detected and transferred via tar pipe for efficiency.

```bash
# CLI upload (single file)
./sshc upload prod ./app.tar.gz /opt/app.tar.gz

# CLI upload (directory → auto tar pipe)
./sshc upload prod ./my-project/ /opt/my-project/

# CLI download
./sshc download prod /var/log/syslog ./syslog
./sshc download prod /opt/data/ ./data-backup/

# Web UI: click "上传" / "下载" buttons in server list
```

**Web UI upload API:**
```bash
# Upload file (base64 content in JSON)
curl -X POST http://127.0.0.1:17375/api/upload/prod \
  -H 'Content-Type: application/json' \
  -d '{"content":"<base64>","remote_path":"/opt/app.tar.gz","filename":"app.tar.gz"}'
# Response: {"ok":true,"name":"prod","size":12345,"remote":"/opt/app.tar.gz"}
```

**Web UI download API:**
```bash
# Download file (streaming binary response)
curl -o app.tar.gz 'http://127.0.0.1:17375/api/download/prod?path=/opt/app.tar.gz'
```

### Config Hot Reload

The C daemon automatically detects config file changes via mtime (last-modified timestamp). No daemon restart needed — any changes to `~/.sshc/profiles.json` take effect on the next request. This includes adding/removing servers, changing proxies, and updating allow/deny rules.

## Architecture

```
sshc (bash) ──nc──> daemon.sock (Unix) / 127.0.0.1:{port} (Windows TCP)
                           │
                      sshc-daemon (C, fork/thread-per-request)
                           │
                      posix_spawnp/vfork+execvp("ssh") / CreateProcess + pipes
                           │
                      ControlMaster ──> Remote Server
                      (~/.sshc/mux/ssh-<name>, persist 300s)

Web UI (python3) ──socket──> daemon IPC ──> health/exec requests
         │
         └── profiles.json (read/write directly, atomic replace + chmod 600)
```

- Daemon overhead ~15ms, end-to-end latency ≈ raw SSH
- ControlMaster auto-warmup on first health check or exec
- Master socket persists 300s idle, auto-reconnect on failure
- Health checks: `ensure_master` + `ssh exec "echo ok"` — also serves as connection warmup
- **Unix:** AF_UNIX socket at `~/.sshc/daemon.sock`, `fork()` per request, `posix_spawnp` for master, `vfork` for exec
- **Windows:** TCP localhost (port written to `~/.sshc/daemon.port`), `CreateThread` per request, `CreateProcess` for all SSH calls
- Command validation: POSIX regex (Unix) / glob matching (Windows) evaluated in daemon before SSH execution
- SFTP: `sftp -b -` with ControlPath reuse for file transfer; tar pipe for directories
- Config hot reload: `stat()`-based mtime detection at start of each request handler, no polling loop
- Key upload: browser `FileReader.readAsDataURL()` → base64 → `~/.sshc/keys/<profile>/<filename>` (chmod 600, basename-sanitized)
- Proxy: stored as raw ProxyCommand string, injected via `-o ProxyCommand="..."` into all SSH invocations

## Platform Notes

| Aspect | Unix (macOS/Linux) | Windows |
|--------|-------------------|---------|
| IPC | AF_UNIX socket | TCP 127.0.0.1 (auto-assigned port) |
| Concurrency | `fork()` per request | `CreateThread` per request |
| Process spawn | `posix_spawnp` / `vfork+execvp` | `CreateProcess` |
| Socket path | `~/.sshc/daemon.sock` | `~/.sshc/daemon.port` (contains port number) |
| Compile | `cc -O2 -o sshc-daemon sshc-daemon.c` | Add `-lws2_32` |
| Shell | bash (native) | Git Bash / MSYS2 / WSL |

## Dependencies

| Dependency | Purpose | Required |
|-----------|---------|----------|
| `cc` (clang/gcc) | Compile C daemon | Yes (one-time) |
| `python3` | JSON parsing + Web UI server | Yes |
| `nc` | Socket communication (IPC) | Yes |
| `ssh` | OpenSSH client with ControlMaster | Yes |
| `sshpass` | Password authentication support | No (only if using password auth) |

## Agent Workflow Guide

### Adding a new server behind a proxy

```bash
# Via CLI
./sshc add hidden root@10.0.0.5 -i ~/.ssh/id_rsa --proxy "nc -X 5 -x 127.0.0.1:1080 %h %p"

# Via REST API (when daemon+web are running)
curl -X POST http://127.0.0.1:17375/api/profiles \
  -H 'Content-Type: application/json' \
  -d '{"name":"hidden","host":"10.0.0.5","user":"root","key":"~/.ssh/id_rsa","proxy":"nc -X 5 -x 127.0.0.1:1080 %h %p"}'
```

### Running commands on multiple servers in parallel

```bash
./sshc exec prod "uptime" &
./sshc exec staging "df -h" &
./sshc exec db "systemctl status postgresql" &
wait
```

### Checking all server status

```bash
./sshc health          # all profiles
./sshc health prod     # single profile
./sshc profiles        # table format with status
```

### Uploading and downloading files

```bash
# Upload
./sshc upload prod ./app.tar.gz /opt/app.tar.gz
./sshc upload prod ./my-project/ /opt/my-project/   # directory → tar pipe

# Download
./sshc download prod /var/log/syslog ./syslog
./sshc download prod /opt/data/ ./data-backup/

# Via Web UI: click "上传" / "下载" buttons in server list
```

### Restricting commands with allow/deny patterns

```bash
# Add server with command safety
./sshc add safe-prod root@10.0.0.1 -i ~/.ssh/id_rsa \
  --allow "^(ls|df|uptime|systemctl|apt|docker).*" \
  --deny ".*rm -rf.*|.*mkfs.*|.*dd if=.*"

# Update via REST API
curl -X PUT http://127.0.0.1:17375/api/profiles/safe-prod \
  -H 'Content-Type: application/json' \
  -d '{"allow":"^(ls|df|uptime|systemctl|apt|docker).*"}'

# Validation happens in daemon — bypassing impossible
```

### Setting up via Web UI (when human needs visual management)

```bash
./sshc web
# Human opens http://127.0.0.1:17375
# - Drag key file onto drop zone
# - Fill host/user/port
# - Click "代理" in server list to configure proxy via modal
# - Click "测试" to verify connectivity
```

### Troubleshooting

```bash
# Daemon not responding
./sshc daemon          # restart

# Master connection stale
./sshc reconnect prod  # force re-establish

# Check if daemon is alive
./sshc health > /dev/null 2>&1 && echo "alive" || echo "dead"
```
