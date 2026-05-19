<sub>🌐 <a href="README.md">中文</a></sub>

# sshc — Let Your Local AI Agent Manage Your Servers

Install this skill and your AI agent (Claude Code, Codex, Cursor) can manage remote servers directly — check status, view logs, batch update, restart services, just like local commands.

```bash
npx skills add fivif/sshc -g --agent claude-code -y
```

## What it does

```
You: Check all my servers
Agent: ● prod alive  ● staging alive  ○ dev dead

You: Show disk usage on prod
Agent: Disk 79% used, 14G remaining. Should I clean up?

You: apt update all three servers
Agent: [parallel] prod ✓  staging ✓  dev ✗ (unreachable)
```

## Getting started

### 1. Add servers

```bash
# CLI
./sshc add my-server root@1.2.3.4 -i ~/.ssh/id_ed25519

# Or use web UI
./sshc web
# Open http://127.0.0.1:17375
```

### 2. Start daemon

```bash
./sshc daemon
```

### 3. Manage

```bash
./sshc health                    # Check all servers
./sshc exec my-server "uptime"   # Run any command
```

## Config

`~/.sshc/profiles.json`, supports key and password auth:

```json
{
  "default": "prod",
  "servers": {
    "prod": { "host": "10.0.0.1", "user": "root", "key": "~/.ssh/id_rsa", "port": 22 },
    "staging": { "host": "10.0.0.2", "user": "admin", "password": "xxx", "port": 2222 }
  }
}
```

## Auth

SSH key or password — mutually exclusive in config.

## Dependencies

System builtins: `python3` `nc` `ssh` `cc`. `sshpass` only needed for password auth.

## License

MIT

## Acknowledgments

Thanks to [DeepSeek](https://deepseek.com) and the [LINUX DO](https://linux.do) community. This project was developed with assistance from DeepSeek models.
