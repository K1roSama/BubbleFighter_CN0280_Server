## Windows x64 服务端

面向 BubbleFighter CN0280（0.2.80.1）客户端的 C++20 / MySQL 服务端。

- 单一 VS 工程和 `BubbleFighter.Server.exe`，包含网关、认证、房间、迁移及管理入口。
- EXE、配置、发布资源、迁移和许可证位于同一个发布目录；配置好数据库后可直接双击启动。
- 英文日志；已移除邮箱验证和反作弊接入，昵称与聊天违规词表为空。
- 下载包由同一提交通过 Windows CI 后生成，并附带第三方许可证和 SHA-256 校验文件。

## 下载与运行

下载 `BubbleFighter-CN0280-Server-win-x64.zip` 并解压。安装 x64 Visual C++ v14
运行库，准备 MySQL 8.4.11，按仓库的 [构建指南](https://github.com/K1roSama/BubbleFighter_CN0280_Server/blob/main/docs/build.md#数据库准备) 创建数据库、授权并执行迁移。
进入解压后的 `BubbleFighter-CN0280-Server-win-x64` 目录，编辑 `config/server.conf`
和 `config/mysql.password`，确认 MySQL 已运行，然后双击 `BubbleFighter.Server.exe`。

当前数据库 schema 为 `2`。旧部署升级前停止服务并完整备份数据库，再执行
`--mode migrate`，将原有配置中的 `database.expected_schema_version` 改为 `2`。
升级会删除邮箱验证及反作弊专用表；设备码封禁相关结构、处罚及审计记录保留。

默认游戏与启动器端口为 `127.0.0.1:27100`，运维就绪地址为
`http://127.0.0.1:27102/ready`。客户端和启动器需另行准备并接入账号票据流程。

发布包不包含 MySQL 数据库服务、客户端、启动器、本机账号数据或真实密码。
`SHA256SUMS` 可用于核对 ZIP；包内 `build-info.json` 记录源码提交及文件校验值。
