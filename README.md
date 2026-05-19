# sshc — SSH Multiplexing CLI

Blazing-fast SSH multiplexing with zero startup overhead. Pure C daemon + Bash client + Python web UI.

## Why sshc?

SSH ControlMaster eliminates repeated authentication, but managing persistent connections is tedious. sshc provides a clean CLI, a zero-overhead C daemon, and a visual web UI — all in 3 distributable files.

- **~15ms daemon overhead** — matches raw SSH performance
- **Zero dependencies** beyond system tools (python3, nc, cc)
- **Single-file distribution** — no package manager needed

## Quick Start

```bash
# 1. Compile the daemon
cc -O2 -o sshc-daemon sshc-daemon.c

# 2. Make executable
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

# 6. Run remote commands
./sshc exec my-server "uptime"

# 7. (Optional) Launch web UI
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

## Web UI

`./sshc web` starts a management dashboard at `http://127.0.0.1:17375`:

- Visual add/remove servers with key or password auth
- Real-time health status
- One-click connection testing
- REST API for LLM/Agent auto-configuration

### REST API

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

## Dependencies

| Dependency | Purpose |
|-----------|---------|
| `cc` (clang/gcc) | Compile C daemon |
| `python3` | JSON parsing + web UI (system builtin) |
| `nc` | UNIX socket communication (system builtin) |
| `ssh` | OpenSSH client |
| `sshpass` | Password auth support (optional) |

## Sharing

Only 3 files needed — drop them anywhere:

```
sshc              # CLI script
sshc-daemon.c     # Daemon source
sshc-daemon       # Compiled binary (optional, same platform)
```

Each user maintains their own `~/.sshc/profiles.json`.

## Security

- Config file permissions: `chmod 600 ~/.sshc/profiles.json`
- Daemon socket: UNIX domain socket, restricted to `0600`
- Web UI binds to `127.0.0.1` only
- Passwords stored in plaintext — use key auth for production

## License

MIT
