<sub>🌐 <a href="README.en.md">English</a></sub>

# sshc — AI Agent 原生 SSH 多路复用

为 AI Agent 打造的极速 SSH 多路复用 CLI。纯 C 守护进程 + ControlMaster 连接复用，消除重复认证延迟。

```bash
npx skills add fivif/sshc -g
```

## 为什么用 sshc？

AI Agent（Claude Code、Codex、Cursor）经常需要在远程服务器上执行命令。原生 SSH 每次都要重新认证——每条命令增加 200ms-2s 延迟。sshc 解决这个问题：

- **~15ms 守护进程开销** — 性能追平裸 SSH 上限
- **Fork-per-request C 守护进程** — 原生支持并发 Agent 调用
- **REST API 自配置** — Agent 无需人工干预即可自行添加/删除服务器
- **零 npm/pip 依赖** — python3 + nc + cc，macOS/Linux 系统自带
- **单文件分发** — 丢 3 个文件到任意目录即可使用

## 快速开始

```bash
# 1. 编译守护进程
cc -O2 -o sshc-daemon sshc-daemon.c

# 2. 确保可执行
chmod +x sshc sshc-daemon

# 3. 创建配置
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

# 4. 启动守护进程
./sshc daemon

# 5. 执行远程命令
./sshc exec my-server "uptime"
```

## 命令

```bash
sshc daemon                   # 启动守护进程
sshc health [profile]         # 健康检查（不指定=全部）
sshc exec <profile> <cmd>     # 执行远程命令
sshc profiles                 # 列出所有服务器
sshc add <name> <user@host> -i <key>    # 添加服务器
sshc remove <name>            # 移除服务器
sshc default <name>           # 设置默认服务器
sshc reconnect <profile>      # 强制重连
sshc web [port]               # 启动 Web 管理界面
```

## Agent 自配置

Agent 通过 Web UI 的 REST API 自行管理服务器配置——无需人工介入：

```bash
# 启动 Web 界面
./sshc web

# Agent 添加服务器
curl -X POST http://127.0.0.1:17375/api/profiles \
  -H 'Content-Type: application/json' \
  -d '{"name":"prod","host":"1.2.3.4","user":"root","key":"~/.ssh/id_rsa"}'

# Agent 测试连接
curl -X POST http://127.0.0.1:17375/api/test/prod
```

完整 REST API：`GET/POST /api/profiles`、`DELETE/PUT /api/profiles/:name`、`POST /api/test/:name`

## 配置文件

`~/.sshc/profiles.json`：

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

支持 SSH 密钥和密码两种认证方式。

## 架构

```
sshc (bash) ──nc──> daemon.sock (UNIX socket)
                         │
                    sshc-daemon (C, fork-per-request)
                         │
                    vfork()+execvp("ssh") + pipe
                         │
                    ControlMaster ──> 远程服务器
                    (~/.sshc/mux/ssh-<name>.sock, 持久 300s)
```

- 纯 C 守护进程，`vfork()` + `execvp()` — 零 shell、零临时文件、零轮询
- `posix_spawnp()` 优化 macOS 下的 master 连接建立
- 阻塞管道 I/O — 无忙等待、无 `usleep()`
- ControlMaster socket + `ControlPersist=300` 连接复用
- 密钥 fallback：`ssh_exec` 始终携带 `-i`，master socket 过期时自动退化密钥认证

## 性能

守护进程开销约 15ms（高于裸 SSH）。剩余延迟来自 SSH/网络抖动（TCP + SSH 多路复用的固有波动）。基准测试时请同步测试裸 SSH 以建立真实基线。

## 依赖

| 依赖 | 用途 |
|------|------|
| `cc` (clang/gcc) | 编译 C 守护进程 |
| `python3` | JSON 解析 + Web UI（系统自带） |
| `nc` | UNIX socket 通信（系统自带） |
| `ssh` | OpenSSH 客户端 |
| `sshpass` | 密码认证（可选） |

## 分发

只需 3 个文件，丢到任意目录即可：

```
sshc              # CLI 脚本
sshc-daemon.c     # C 守护进程源码
sshc-daemon       # 预编译二进制（可选，同平台直接用）
```

各自维护自己的 `~/.sshc/profiles.json`。

## 安全

- 配置文件：`chmod 600 ~/.sshc/profiles.json`
- 守护进程 socket：UNIX domain，权限 `0600`
- Web UI：仅绑定 `127.0.0.1`，不对外暴露
- 密码明文存储 — 生产环境建议使用密钥认证

## License

MIT
