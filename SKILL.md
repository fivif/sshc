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

Pure C fork architecture + UNIX domain socket. System dependencies only (python3 + nc). Single-file distributable. SSH ControlMaster persistent multiplexing eliminates repeated auth delay.

## Execution Rules

1. Ensure daemon is alive before any exec: `./sshc health > /dev/null 2>&1 || ./sshc daemon &`
2. All remote commands go through: `./sshc exec <profile> "<command>"`
3. Independent commands can run in parallel
4. Config contains secrets — ensure `chmod 600 ~/.sshc/profiles.json`

## Quick Start

```bash
# 1. Compile daemon
cc -O2 -o sshc-daemon sshc-daemon.c

# 2. Ensure executable
chmod +x sshc sshc-daemon

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
sshc daemon                  # Start daemon
sshc health [profile]        # Health check (omit for all)
sshc exec <profile> <cmd> [timeout]  # Execute command
sshc profiles                # List all servers and status
sshc add <name> <user@host> -i <key> [-p port]  # Add server
sshc remove <name>           # Remove server
sshc default <name>          # Set default server
sshc reconnect <profile>     # Force reconnect
sshc web [port]              # Start web UI (default :17375)
```

## Web UI & REST API

`./sshc web` starts a management dashboard at `http://127.0.0.1:17375`. Agents can self-configure servers via REST API without human intervention:

```bash
# List all servers
curl http://127.0.0.1:17375/api/profiles

# Add server (key auth)
curl -X POST http://127.0.0.1:17375/api/profiles \
  -H 'Content-Type: application/json' \
  -d '{"name":"prod","host":"1.2.3.4","user":"root","key":"~/.ssh/id_rsa"}'

# Add server (password auth)
curl -X POST http://127.0.0.1:17375/api/profiles \
  -H 'Content-Type: application/json' \
  -d '{"name":"staging","host":"5.6.7.8","user":"admin","password":"mypass","port":2222}'

# Set default
curl -X PUT http://127.0.0.1:17375/api/profiles/prod -d '{"default":true}'

# Test connection
curl -X POST http://127.0.0.1:17375/api/test/prod

# Delete server
curl -X DELETE http://127.0.0.1:17375/api/profiles/staging
```

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
    }
  }
}
```

- `default` — default server name
- `servers.<name>.host` — IP or domain (required)
- `servers.<name>.user` — SSH user (default: `root`)
- `servers.<name>.key` — SSH key path (mutually exclusive with `password`)
- `servers.<name>.password` — plaintext password (mutually exclusive with `key`)
- `servers.<name>.port` — SSH port (default: `22`)

## Architecture

```
sshc (bash) ──nc──> daemon.sock (UNIX socket)
                         │
                    sshc-daemon (C, fork-per-request)
                         │
                    vfork()+execvp("ssh") + pipe
                         │
                    ControlMaster ──> Remote Server
                    (~/.sshc/mux/ssh-<name>.sock, persist 300s)
```

- Daemon overhead ~15ms, end-to-end latency ≈ raw SSH
- ControlMaster auto-warmup, reuse after first connection
- Zero temp files, zero shell passthrough, zero polling
- Key fallback: if ControlMaster socket expires, SSH falls back to `-i` key automatically

## Dependencies

| Dependency | Purpose |
|-----------|---------|
| `cc` (clang/gcc) | Compile C daemon |
| `python3` | JSON parsing + web UI (system builtin) |
| `nc` | UNIX socket communication (system builtin) |
| `ssh` | OpenSSH client |
| `sshpass` | Password auth support (optional) |
