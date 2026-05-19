<sub>🌐 <a href="README.en.md">English</a></sub>

# sshc — AI Agent 原生 SSH 多路复用

为 AI Agent 打造的极速 SSH 多路复用 CLI。纯 C 守护进程 + ControlMaster 连接复用，让 Agent 能像操作本地一样操作远程服务器。

```bash
npx skills add fivif/sshc -g
```

## 为什么用 sshc？

当你需要管理多台 VPS、批量执行运维任务、或者让 AI Agent 帮你管服务器时，每次 SSH 都要重新握手认证。ssh 自带的 ControlMaster 能用但需要手动管理 socket、配置繁琐。sshc 把这些自动化了：

- **Agent 优先设计** — REST API + Web UI，Agent 可自行添加/删除服务器配置
- **并发原生支持** — C fork-per-request 架构，多条命令真正并行执行
- **零依赖分发** — python3 + nc + cc 系统自带，丢 3 个文件就能用
- **~15ms 守护进程开销** — 端到端延迟追平裸 SSH

## Agent 使用演示

安装后，Agent 会自动识别 sshc 技能。典型交互：

```
用户：帮我看看所有服务器状态
Agent：● prod: alive  ● staging: alive  ○ dev: dead

用户：在 prod 上查一下磁盘
Agent：/dev/vda1  64G  51G  14G  79% /
      ⚠️ 磁盘快满了，要清理吗？

用户：把三台服务器都更新一下
Agent：[并行执行]
      prod: apt update done ✓
      staging: apt update done ✓
      dev: connection failed ✗
      dev 服务器失联，需要你手动检查一下
```

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

# 5. 健康检查
./sshc health

# 6. 执行远程命令
./sshc exec my-server "uptime"
```

## 命令

```bash
sshc daemon                   # 启动守护进程
sshc health [profile]         # 健康检查（不指定=全部）
sshc exec <profile> <cmd> [timeout]  # 执行远程命令
sshc profiles                 # 列出所有服务器
sshc add <name> <user@host> -i <key> [-p port]  # 添加服务器
sshc remove <name>            # 移除服务器
sshc default <name>           # 设置默认服务器
sshc reconnect <profile>      # 强制重连
sshc web [port]               # 启动 Web 管理界面
```

## Agent 自配置

Agent 可以通过 Web UI 的 REST API 自行管理服务器——用户只需提供一次连接信息：

```bash
# 启动 Web 界面
./sshc web

# Agent 添加服务器（密钥认证）
curl -X POST http://127.0.0.1:17375/api/profiles \
  -H 'Content-Type: application/json' \
  -d '{"name":"prod","host":"1.2.3.4","user":"root","key":"~/.ssh/id_rsa"}'

# Agent 添加服务器（密码认证）
curl -X POST http://127.0.0.1:17375/api/profiles \
  -H 'Content-Type: application/json' \
  -d '{"name":"staging","host":"5.6.7.8","user":"admin","password":"mypass","port":2222}'

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
- 阻塞管道 I/O — 无忙等待
- ControlMaster 自动预热，首次连接后复用
- 密钥 fallback：master socket 过期时自动退化密钥认证

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
