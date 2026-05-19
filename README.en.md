<sub>🌐 <a href="README.md">中文</a></sub>

# sshc — Agent-Native SSH Multiplexing

Blazing-fast SSH multiplexing CLI built for AI agents. Pure C daemon with ControlMaster connection reuse — let your agent manage remote servers as if they were local.

```bash
npx skills add fivif/sshc -g --agent claude-code -y
```

## Why sshc?

When you manage multiple VPS instances, run batch ops across servers, or let an AI agent handle your infrastructure, repeated SSH handshakes add up. SSH's built-in ControlMaster works but requires manual socket management. sshc automates all of that:

- **Agent-first design** — REST API + Web UI, agents self-configure server profiles
- **Native concurrency** — C fork-per-request, commands run truly in parallel
- **Zero-dependency distribution** — python3 + nc + cc ship with every OS, just drop 3 files
- **~15ms daemon overhead** — end-to-end latency matches raw SSH

## Agent Interaction Demo

Once installed, your agent picks up sshc automatically. Example session:

```
User: Check all my servers
Agent: ● prod: alive  ● staging: alive  ○ dev: dead

User: Show disk usage on prod
Agent: /dev/vda1  64G  51G  14G  79% /
      ⚠️ Disk almost full, should I clean up?

User: Update all three servers
Agent: [running in parallel]
      prod: apt update done ✓
      staging: apt update done ✓
      dev: connection failed ✗
      dev is unreachable — needs manual inspection
```

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

# 5. Health check
./sshc health

# 6. Run remote commands
./sshc exec my-server "uptime"
```

## Commands

```bash
sshc daemon                   # Start daemon
sshc health [profile]         # Health check (all or specific)
sshc exec <profile> <cmd> [timeout]  # Execute remote command
sshc profiles                 # List all servers
sshc add <name> <user@host> -i <key> [-p port]  # Add server
sshc remove <name>            # Remove server
sshc default <name>           # Set default server
sshc reconnect <profile>      # Force reconnect
sshc web [port]               # Launch web UI
```

## Agent Self-Configuration

Agents manage server profiles via the web UI REST API — users only provide credentials once:

```bash
# Start web UI
./sshc web

# Agent adds server (key auth)
curl -X POST http://127.0.0.1:17375/api/profiles \
  -H 'Content-Type: application/json' \
  -d '{"name":"prod","host":"1.2.3.4","user":"root","key":"~/.ssh/id_rsa"}'

# Agent adds server (password auth)
curl -X POST http://127.0.0.1:17375/api/profiles \
  -H 'Content-Type: application/json' \
  -d '{"name":"staging","host":"5.6.7.8","user":"admin","password":"mypass","port":2222}'

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
- Blocking pipe I/O — no busy-wait
- ControlMaster auto-warmup, reuse after first connection
- Key fallback: `-i` flag always passed, degraded auth when master socket expires

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
