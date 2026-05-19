# sshc — Agent-Native SSH Multiplexing

Blazing-fast SSH multiplexing CLI built for AI agents. Zero-overhead C daemon with ControlMaster connection reuse eliminates repeated authentication latency.

```bash
npx skills add fivif/sshc -g
```

## Why sshc?

AI agents (Claude Code, Codex, Cursor) frequently need to run commands on remote servers. Raw SSH re-authenticates every time — adding 300ms-2s latency per command. sshc solves this:

- **~15ms daemon overhead** — matches raw SSH performance ceiling
- **Fork-per-request C daemon** — handles concurrent agent calls natively
- **REST API for self-configuration** — agents add/remove servers without human intervention
- **Zero npm/pip dependencies** — python3 + nc + cc, everything ships with macOS/Linux
- **Single-file distributable** — drop 3 files anywhere, no install step

## Quick Start

```bash
# 1. Compile daemon
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

# 5. Run remote commands
./sshc exec my-server "uptime"
```

## Commands

```bash
sshc daemon                   # Start daemon
sshc health [profile]         # Health check (all or specific)
sshc exec <profile> <cmd>     # Execute remote command
sshc profiles                 # List all servers
sshc add <name> <user@host> -i <key>    # Add server
sshc remove <name>            # Remove server
sshc default <name>           # Set default server
sshc reconnect <profile>      # Force reconnect
sshc web [port]               # Launch web UI
```

## Agent Self-Configuration

Agents manage server profiles via the web UI REST API — no human needed:

```bash
# Start web UI
./sshc web

# Agent adds a server
curl -X POST http://127.0.0.1:17375/api/profiles \
  -H 'Content-Type: application/json' \
  -d '{"name":"prod","host":"1.2.3.4","user":"root","key":"~/.ssh/id_rsa"}'

# Agent tests connection
curl -X POST http://127.0.0.1:17375/api/test/prod
```

Full REST API: `GET/POST /api/profiles`, `DELETE/PUT /api/profiles/:name`, `POST /api/test/:name`

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

Supports both SSH key and password authentication.

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

- Pure C daemon with `vfork()` + `execvp()` — zero shell, zero temp files, zero polling
- `posix_spawnp()` for macOS-optimized master connection setup
- Blocking pipe I/O — no busy-wait, no `usleep()`
- ControlMaster socket with `ControlPersist=300` for connection reuse
- Key fallback: `ssh_exec` always passes `-i` so expired master sockets don't cause auth failures

## Performance

Daemon overhead measured at ~15ms above raw SSH. Remaining latency is SSH/network jitter (inherent to TCP + SSH multiplexing). When benchmarking, test raw SSH simultaneously to establish the true baseline.

## Dependencies

| Dependency | Purpose |
|-----------|---------|
| `cc` (clang/gcc) | Compile C daemon |
| `python3` | JSON + web UI (system builtin) |
| `nc` | UNIX socket IPC (system builtin) |
| `ssh` | OpenSSH client |
| `sshpass` | Password auth (optional) |

## Sharing

Drop 3 files anywhere — no install required:

```
sshc              # CLI script
sshc-daemon.c     # C daemon source
sshc-daemon       # Pre-compiled binary (optional, same platform)
```

Each user maintains their own `~/.sshc/profiles.json`.

## Security

- Config file: `chmod 600 ~/.sshc/profiles.json`
- Daemon socket: UNIX domain, restricted `0600`
- Web UI: binds `127.0.0.1` only, no external exposure
- Passwords stored as plaintext — prefer key auth for production

## License

MIT
