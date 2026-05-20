<sub>🌐 <a href="README.md">中文</a></sub>

# sshc — Let Your Local AI Agent Manage Your Servers

Install this skill and your AI Agent can operate remote servers directly. No commands to memorize, no environment to configure — just talk naturally.

```bash
npx skills add fivif/sshc -g --agent claude-code -y
```

## Natural Language Server Management

```
You: /sshc
Agent: Web management panel opened at http://127.0.0.1:17375

You: /sshc configure a production server at 1.2.3.4, root user, use my ~/.ssh/id_rsa key
Agent: Added server "prod": root@1.2.3.4:22

You: /sshc add a local SOCKS5 proxy to prod, using 127.0.0.1:1080
Agent: Proxy configured for prod: nc -X 5 -x 127.0.0.1:1080 %h %p

You: Check all servers
Agent: ● prod alive  ● staging alive  ○ dev dead

You: Show disk usage on prod
Agent: Disk 79% used, 14G remaining. Should I clean /var/log?

You: apt update all three servers
Agent: [parallel] prod ✓  staging ✓  dev ✗ (unreachable — needs manual check)
```

## What Your Agent Can Do

| You say | Agent does |
|---------|------------|
| `/sshc` | Open the web management dashboard |
| `/sshc configure xxx VPS` | Add a server to config |
| `/sshc add proxy to xxx` | Configure HTTP/SOCKS5 proxy |
| `/sshc check all servers` | Health check + status overview |
| `/sshc run <command> on xxx` | Execute any command remotely |
| `/sshc batch update all servers` | Parallel execution, per-server report |

Your agent handles: compiling the daemon, starting the daemon, setting file permissions, parallel execution, and automatic retry on failure.

## Configuration

Your agent maintains `~/.sshc/profiles.json`, supporting keys, passwords, and proxies:

```json
{
  "default": "prod",
  "servers": {
    "prod": {
      "host": "10.0.0.1", "user": "root",
      "key": "~/.ssh/id_rsa", "port": 22,
      "proxy": "nc -X 5 -x 127.0.0.1:1080 %h %p"
    },
    "staging": {
      "host": "10.0.0.2", "user": "admin",
      "password": "xxx", "port": 2222
    }
  }
}
```

## Platform Support

| OS | Status |
|-----|--------|
| macOS | Native |
| Linux (Debian/Ubuntu/CentOS/Arch etc.) | Native |
| BSD (FreeBSD/OpenBSD) | Native |
| Windows (native) | Daemon + Web UI native, CLI needs Git Bash |
| Windows (WSL) | Full support |

## Manual Command Reference

If you prefer typing commands yourself instead of letting your agent handle it:

```bash
./sshc daemon                              # Start daemon
./sshc health                              # Check all servers
./sshc exec <name> "<cmd>" [timeout]       # Execute command
./sshc add <name> <user@host> -i <key> [-p port] [--proxy <cmd>]  # Add server
./sshc remove <name>                       # Remove server
./sshc default <name>                      # Set as default
./sshc reconnect <name>                    # Force reconnect
./sshc web [port]                          # Open web dashboard
```

## Dependencies

System builtins: `python3` `nc` `ssh` `cc`. `sshpass` only needed for password auth.

Windows additionally requires `ws2_32` (system library, auto-linked by MinGW/MSVC).

## License

MIT

## Acknowledgments

Thanks to [DeepSeek](https://deepseek.com) and the [LINUX DO](https://linux.do) community. This project was developed with assistance from DeepSeek models.
