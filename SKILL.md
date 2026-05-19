---
name: sshc
description: 通用 SSH 多路复用 CLI 工具。触发：远程服务器管理、批量运维、SSH 连接加速。
---

# sshc — SSH 多路复用 CLI

纯 C fork 架构 + UNIX socket，零依赖（仅需 python3 + nc），单文件分发。SSH ControlMaster 常驻复用，消除重复认证延迟。

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

# 7. (可选) 启动 Web 管理界面
./sshc web
# 打开 http://127.0.0.1:17375
```

## 依赖

| 依赖 | 说明 |
|------|------|
| `cc` (clang/gcc) | 编译 C 守护进程 |
| `python3` | JSON 解析 + Web UI（系统自带） |
| `nc` | UNIX socket 通信（系统自带） |
| `ssh` | OpenSSH 客户端 |
| `sshpass` | 密码认证支持（可选，仅密码登录需要） |

## 命令

```bash
sshc daemon                  # 启动守护进程
sshc health [profile]        # 健康检查（不指定=全部，* =全部）
sshc exec <profile> <cmd> [timeout]  # 执行命令
sshc profiles                # 列出所有服务器和状态
sshc add <name> <user@host> -i <key> [-p port]  # 添加服务器
sshc remove <name>           # 移除服务器
sshc default <name>          # 设置默认服务器
sshc reconnect <profile>     # 强制重连
sshc web [port]              # 启动 Web 管理界面 (默认 :17375)
```

## Web 管理界面

`./sshc web` 启动后访问 `http://127.0.0.1:17375`，提供：

- **可视化添加/删除**：支持密钥路径和密码两种认证
- **状态面板**：实时显示所有服务器存活状态
- **快速测试**：一键检测连接
- **REST API**：Agent/LLM 可自行调用 API 管理配置

### API 端点

```bash
# 获取所有服务器
curl http://127.0.0.1:17375/api/profiles

# 添加服务器（密钥）
curl -X POST http://127.0.0.1:17375/api/profiles \
  -H 'Content-Type: application/json' \
  -d '{"name":"prod","host":"1.2.3.4","user":"root","key":"~/.ssh/id_rsa"}'

# 添加服务器（密码）
curl -X POST http://127.0.0.1:17375/api/profiles \
  -H 'Content-Type: application/json' \
  -d '{"name":"staging","host":"5.6.7.8","user":"admin","password":"mypass","port":2222}'

# 设置默认
curl -X PUT http://127.0.0.1:17375/api/profiles/prod -d '{"default":true}'

# 测试连接
curl -X POST http://127.0.0.1:17375/api/test/prod

# 删除服务器
curl -X DELETE http://127.0.0.1:17375/api/profiles/staging
```

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

- `default`：默认服务器名
- `servers.<name>.host`：必填，IP 或域名
- `servers.<name>.user`：选填，默认 `root`
- `servers.<name>.key`：密钥路径（与 `password` 二选一）
- `servers.<name>.password`：明文密码（与 `key` 二选一）
- `servers.<name>.port`：选填，默认 `22`

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

- Daemon 纯开销 ~15ms，端到端延迟 ≈ 裸 SSH
- ControlMaster 自动预热，首次连接后复用
- 零临时文件、零 shell 中转、零轮询

## 分享给团队

只需 3 个文件，对方放到任意目录即可：

```
sshc              # CLI 脚本
sshc-daemon.c     # 守护进程源码
sshc-daemon       # 编译产物 (可选，同平台可直接用)
```

各自配自己的 `~/.sshc/profiles.json`。

## 执行规则

1. 先确保 daemon 存活：`./sshc health > /dev/null 2>&1 || ./sshc daemon &`
2. 所有远程命令走 `./sshc exec <profile> "<命令>"`
3. 多条独立命令可并行执行
4. 配置文件包含敏感信息，确保权限 `chmod 600 ~/.sshc/profiles.json`
