# 运行与接入

## 启动与停止

按[构建指南](build.md)完成数据库准备后，双击 `BubbleFighter.Server.exe`。无参数运行会以 EXE 所在目录作为工作目录，读取 `config/server.conf`；从快捷方式或其他工作目录打开也会读取同一套文件。

数据库必须提前启动。服务端不负责安装 MySQL、创建空库或自动执行迁移。双击启动失败时，独立控制台会保留错误提示并等待按键。

当前程序要求 MySQL schema 版本 `2`。旧部署先按[数据库准备](build.md#数据库准备)备份并执行迁移，再将已有配置的 `database.expected_schema_version` 改为 `2`。版本 2 移除邮箱验证和反作弊专用存储，设备码封禁相关结构与已有处罚记录保留。

按 **Ctrl+C** 有序停止服务。在线会话、广场和普通房间不在重启后恢复，客户端需要重新连接；角色、库存、奖励和结算等持久化数据保存在 MySQL。

## 地址与监听

默认模板仅监听本机：

| 配置项 | 默认值 | 用途 |
| --- | --- | --- |
| `network.bind_address` | `127.0.0.1` | 游戏和认证监听地址 |
| `network.port` | `27100` | 游戏网关、启动器注册与登录 |
| `network.clan_port` | `27101` | 战队服务 |
| `network.equipment_relay_port` | `27104` | 装备中继 |
| `operations.bind_address` | `127.0.0.1` | 运维 HTTP，仅本机 |
| `operations.port` | `27102` | `/ready` 等运维端点 |

局域网部署将 `network.bind_address` 改为服务器网卡的实际 IPv4，并设置客户端可以访问的 `network.advertised_address`。NAT 部署仍监听本机拥有的地址，将 advertised address 配置为玩家可达地址，并配置相应防火墙和端口转发。

运维 HTTP 保持回环监听，不向外部直接开放。监听端口应彼此不同；默认 MySQL 也只监听本机。

## 客户端接入

仓库面向国服 `0.2.80.1` 客户端，不包含客户端补丁、启动器或分发链接。官方 Passport 不能直接作为本服务端的登录凭据。

主网关提供启动器注册和密码登录入口。启动器需要使用服务端对应的包格式和字段，成功登录后取得一次性游戏票据，再让游戏客户端连接对应游戏端点。票据消费后不能重放。仅修改客户端 IP 不等于完成认证接入。

认证实现见 [launcher_authentication_handler.cpp](../src/network/launcher_authentication_handler.cpp)、[launcher_authentication.cpp](../src/features/authentication/launcher_authentication.cpp) 和 [authentication.cpp](../src/features/authentication/authentication.cpp)。

## 命令行选项

在 EXE 所在目录打开 PowerShell，可以查看帮助：

```powershell
.\BubbleFighter.Server.exe --help
```

| 模式 | 参数 | 用途 |
| --- | --- | --- |
| `combined` | `--config <server.conf>`，可选 `--login-config <login.conf>` | 默认模式，统一网关及认证 |
| `game` | `--config <game.conf>`，可选 `--room-ds-config <gateway.json>` | 游戏服务与可选房间网关 |
| `login` | `--config <login.conf>` | 独立登录入口 |
| `migrate` | `--config <database.conf> --migrations <目录>` | 数据库结构和种子数据迁移 |
| `admin` | 管理命令与 `--config <database.conf>` | 查询账号、封禁、禁言及维护 |
| `room` | `--config <node.json>` | 专用房间进程 |
| `agent` | `--config <node.json>` | 房间节点代理 |

使用 `--mode` 选择模式。普通部署无需单独启动全部模式；同一个 EXE 可用于需要拆分的不同进程。

```powershell
.\BubbleFighter.Server.exe --config .\config\server.conf
.\BubbleFighter.Server.exe --mode login --config .\config\login.conf
.\BubbleFighter.Server.exe --mode migrate --config .\config\migrate.conf --migrations .\migrations
```

主服务和独立登录还支持：

| 参数 | 行为 |
| --- | --- |
| `--check-config` | 校验配置后退出，不启动监听 |
| `--check-database` | 校验数据库连接、schema 及所需发布数据后退出 |
| `--run-for-ms <0..3600000>` | 按给定时长运行后有序退出 |

显式 `--config` 路径按调用方工作目录解析。无参数运行、或主模式省略 `--config` 时，程序使用 EXE 目录的默认配置。

EXE 旁存在 `config/login.conf` 时，默认启动会在同一进程内增加登录监听。登录配置必须与主服务使用相同数据库、大区和游戏端点，端口应单独设置；合并运行时共用主运维 HTTP。模板位于 `config/examples/login.production.example.conf`。

房间代理与专用房间需要部署者自行配置 mTLS 证书、可信对端、容量和地址，相关 JSON 模板位于 [config/examples](../config/examples)。本机普通运行无需配置这些节点。

## 账号管理

复制 `config/examples/admin.example.conf` 为私有 `config/admin.conf`，配置具有对应业务写权限的管理账号。模板从 `BF_MYSQL_ADMIN_PASSWORD` 环境变量读取数据库密码，也可改用 `database.password_file`；两者不能同时设置。

```powershell
.\BubbleFighter.Server.exe --mode admin --help
.\BubbleFighter.Server.exe --mode admin inspect-account --account PlayerName --config .\config\admin.conf
.\BubbleFighter.Server.exe --mode admin mute --account PlayerName --minutes 60 --actor Operator --reason "Chat abuse" --config .\config\admin.conf
.\BubbleFighter.Server.exe --mode admin unmute --account PlayerName --actor Operator --reason "Reviewed" --config .\config\admin.conf
.\BubbleFighter.Server.exe --mode admin ban-account --account PlayerName --minutes 60 --actor Operator --reason "Manual review" --config .\config\admin.conf
.\BubbleFighter.Server.exe --mode admin unban-account --account PlayerName --actor Operator --reason "Reviewed" --config .\config\admin.conf
```

可以用 `--account-id <id>` 替代 `--account <login>`，两者只能选择一个。修改类命令要求填写 `--actor` 和 `--reason`；`--minutes 0` 表示永久。管理工具不会打印数据库密码。

## 日志与排查

控制台使用英文日志、本地时间和 `INFO / WARN / ERROR` 级别。文件日志包含 UTC 时间、级别、关联标识，默认保存到 EXE 旁的 `logs/YYYY-MM-DD.log`。

- 修改 `logging.level=debug` 可查看更详细的连接记录。
- `/ready` 返回 HTTP 200 与 `{"status":"ready"}` 表示服务就绪。
- 数据库连接失败时检查 MySQL 是否启动、账号权限、TLS，以及密码文件是否仍为 `CHANGE_ME`。
- 资源校验失败时检查迁移是否完成、schema 是否为 `2`、数据库与发布数据是否匹配。
- 端口被占用时先确认占用程序，修改配置或正常关闭对应服务。
- 启动成功但客户端进不去时核对客户端版本、启动器协议、票据、游戏端点及防火墙。

配置、密码文件和日志都可能包含私有信息；提交问题前删除密码、票据和个人信息。
