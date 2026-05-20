<sub>🌐 <a href="README.en.md">English</a></sub>

# sshc — 让本地 AI Agent 在线管理你的服务器

装上这个 skill，Claude Code / Codex / Cursor 就能直接操作你的远程服务器——查状态、看日志、批量更新、重启服务，像本地命令一样顺手。

```bash
npx skills add fivif/sshc -g --agent claude-code -y
```

## 能做什么

```
你：帮我看看所有服务器状态
Agent：● prod 正常  ● staging 正常  ○ dev 失联

你：在 prod 上看看磁盘
Agent：磁盘用了 79%，还有 14G，建议清理

你：把三台服务器都 apt update 一下
Agent：[并行] prod ✓  staging ✓  dev ✗（失联需检查）
```

## 安装后怎么用
```bash
Agent 交互： /sshc 打开UI 
```

### 1. 添加服务器

```bash
# 命令行
./sshc add my-server root@1.2.3.4 -i ~/.ssh/id_ed25519

# 或开 Web UI 可视化操作
./sshc web
# 打开 http://127.0.0.1:17375
```

### 2. 启动守护进程

```bash
./sshc daemon
```

### 3. 开始管理

```bash
./sshc health              # 检查所有服务器状态
./sshc exec my-server "uptime"   # 执行任意命令
```

## 配置文件

`~/.sshc/profiles.json`，支持密钥和密码两种认证：

```json
{
  "default": "prod",
  "servers": {
    "prod": { "host": "10.0.0.1", "user": "root", "key": "~/.ssh/id_rsa", "port": 22 },
    "staging": { "host": "10.0.0.2", "user": "admin", "password": "xxx", "port": 2222 }
  }
}
```

## 支持认证方式

SSH 密钥 + 密码，配置里 `key` 和 `password` 二选一。

## 系统支持

| 系统 | 状态 |
|------|------|
| macOS | 原生支持 |
| Linux (Debian/Ubuntu/CentOS/Arch 等) | 原生支持 |
| BSD (FreeBSD/OpenBSD) | 原生支持 |
| Windows (WSL) | WSL 内完整支持 |
| Windows (原生) | 不支持，请用 WSL |

## 依赖

系统自带：`python3` `nc` `ssh` `cc`。`sshpass` 用密码时才需要。

## License

MIT

## 致谢

感谢 [DeepSeek](https://deepseek.com) 和 [LINUX DO](https://linux.do) 社区。本项目由 DeepSeek 模型辅助开发。
