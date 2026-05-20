<sub>🌐 <a href="README.en.md">English</a></sub>

# sshc — 让本地 AI Agent 在线管理你的服务器

装上这个 skill，你的 AI Agent 就能直接操作远程服务器。不用记命令，不用配环境，说人话就行。

```bash
npx skills add fivif/sshc -g --agent claude-code -y
```

## 说人话就能管服务器

```
你：/sshc
Agent：已打开 Web 管理面板 http://127.0.0.1:17375

你：/sshc 配置一台生产服务器，1.2.3.4，root 用户，用我的 ~/.ssh/id_rsa 密钥
Agent：已添加服务器 "prod"：root@1.2.3.4:22

你：/sshc 给 prod 加个本地 SOCKS5 代理，走 127.0.0.1:1080
Agent：已为 prod 配置代理：nc -X 5 -x 127.0.0.1:1080 %h %p

你：看看所有服务器状态
Agent：● prod 正常  ● staging 正常  ○ dev 失联

你：在 prod 上看看磁盘
Agent：磁盘用了 79%，还有 14G，建议清理 /var/log

你：三台服务器都 apt update 一下
Agent：[并行] prod ✓  staging ✓  dev ✗（失联需手动检查）
```

## Agent 能帮你做什么

| 你说 | Agent 做 |
|------|----------|
| `/sshc` | 打开 Web 管理面板 |
| `/sshc 配置 xxx VPS` | 添加服务器到配置 |
| `/sshc 给 xxx 加代理` | 配置 HTTP/SOCKS5 代理 |
| `/sshc 查看所有服务器` | 健康检查 + 状态面板 |
| `/sshc 在 xxx 上执行 <命令>` | 远程执行任意命令 |
| `/sshc 批量更新所有服务器` | 并行执行，逐个汇报 |

Agent 自动处理：编译 daemon、启动守护进程、配置权限、并行执行、故障重试。

## 配置文件

Agent 帮你维护 `~/.sshc/profiles.json`，支持密钥/密码/代理：

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

## 系统支持

| 系统 | 状态 |
|------|------|
| macOS | 原生支持 |
| Linux (Debian/Ubuntu/CentOS/Arch 等) | 原生支持 |
| BSD (FreeBSD/OpenBSD) | 原生支持 |
| Windows (原生) | 守护进程 + Web UI 原生支持，CLI 需 Git Bash |
| Windows (WSL) | 完整支持 |

## 手动命令参考

如果你想自己敲命令而不是让 Agent 代劳：

```bash
./sshc daemon                              # 启动守护进程
./sshc health                              # 检查所有服务器状态
./sshc exec <name> "<cmd>" [timeout]       # 执行命令
./sshc add <name> <user@host> -i <key> [-p port] [--proxy <cmd>]  # 添加
./sshc remove <name>                       # 移除
./sshc default <name>                      # 设为默认
./sshc reconnect <name>                    # 强制重连
./sshc web [port]                          # 打开 Web 管理面板
```

## 依赖

系统自带：`python3` `nc` `ssh` `cc`。`sshpass` 用密码时才需要。

Windows 额外需要 `ws2_32`（系统自带，MinGW/MSVC 自动链接）。

## License

MIT

## 致谢

感谢 [DeepSeek](https://deepseek.com) 和 [LINUX DO](https://linux.do) 社区。本项目由 DeepSeek 模型辅助开发。
