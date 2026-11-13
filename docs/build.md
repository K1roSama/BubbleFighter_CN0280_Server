# 构建指南

## 环境要求

安装 Visual Studio 2022 的“使用 C++ 的桌面开发”组件，包含 MSVC v143 和 Windows SDK `10.0.26100.0`。项目使用 C++20，只提供 `Debug / Release`、`x64` 配置。

构建还需要 Python 3.12 或更新版本、PowerShell、Git，以及可用的 vcpkg。应用和依赖使用动态 MSVC C 运行库 `/MD`，Debug 使用 `/MDd`；部署机器需要 x64 Visual C++ v14 运行库。

运行 `build/fetch_mysql_sdk.ps1` 可从 Oracle 官方地址下载 MySQL **8.4.11** Windows x64 ZIP，验证 [固定 SHA-256](../build/dependencies.json)，并只提取构建和运行需要的 SDK 文件与许可证。该脚本不安装数据库服务。也可以自行准备 SDK，目录必须包含：

```text
include/mysql.h
include/mysql_version.h
lib/libmysql.lib
lib/libmysql.dll
bin/libssl-3-x64.dll
bin/libcrypto-3-x64.dll
```

构建会检查 MySQL 头文件和 DLL 版本。`MySqlRoot` 可使用环境变量或 MSBuild 属性指定，默认路径为 `Artifacts/Dependencies/mysql-8.4.11-winx64`。

## 恢复依赖

在仓库根目录打开 Visual Studio Developer PowerShell：

```powershell
.\build\fetch_mysql_sdk.ps1
.\build\restore_dependencies.ps1
```

脚本优先使用 `VCPKG_ROOT`，未设置时查找 VS 2022 自带的 vcpkg。也可显式传入 `-VcpkgRoot C:\src\vcpkg`。独立 vcpkg 仓库需要先完成其自身的 bootstrap。

依赖按 [vcpkg.json](../vcpkg.json) 的固定基线恢复到 `Artifacts/Dependencies/vcpkg`，使用 [x64-windows-static-md](../triplets/x64-windows-static-md.cmake) triplet。该 triplet 将 C++ 库静态链接，C 运行库仍使用 DLL，并固定 v143。

已有官方 ZIP 时可用 `build/fetch_mysql_sdk.ps1 -Archive C:\Downloads\mysql-8.4.11-winx64.zip`，仍会校验 SHA-256。使用其他解压位置时，先设置 `$env:MySqlRoot = 'C:\MySQL\mysql-8.4.11-winx64'`，再运行依赖恢复和构建。`BfDependencies` 可覆盖包含 `include`、`lib` 和 `debug/lib` 的 vcpkg 安装目录。

## 生成程序

```powershell
msbuild .\BubbleFighter.Server.sln /m /p:Configuration=Release /p:Platform=x64
msbuild .\BubbleFighter.Server.sln /m /p:Configuration=Debug /p:Platform=x64
```

使用自定义 SDK 路径时，也可以传入：

```powershell
msbuild .\BubbleFighter.Server.sln /m /p:Configuration=Release /p:Platform=x64 "/p:MySqlRoot=C:\MySQL\mysql-8.4.11-winx64"
```

解决方案只有一个应用工程，所有服务端模块与 libjuice 源码直接编译到 `BubbleFighter.Server.exe`，不存在内部静态库子工程或测试构建入口。项目自身 C++ 代码采用 `/W4 /WX`，第三方依赖头文件使用外部头文件警告设置。

输出位于 `Artifacts/Bin/x64/Release` 或对应的 `Debug` 目录。构建后会自动复制 MySQL 客户端 DLL，并通过 `build/prepare_runtime.py` 准备配置、发布资源、迁移和日志目录。已有的 `config/server.conf` 和 `config/mysql.password` 保留原内容。

新增源码时，直接在 VS 的唯一工程中添加文件；同名源文件需设置独立的对象文件名。生成数据已随源码提交，普通构建不依赖额外代码生成器。

## CI 与发布

[Windows CI](https://github.com/K1roSama/BubbleFighter_CN0280_Server/actions/workflows/windows.yml) 在 `main` 推送、Pull Request 和手动运行时执行，使用 Windows Server 2022、VS 2022、Python 3.12，以及固定 vcpkg 基线和 MySQL SDK。依赖按运行器镜像版本和依赖文件摘要缓存。Debug 与 Release 分别执行完整构建，保留 `/W4 /WX`，并运行 `--help` 和 `--check-config`。这些检查不启动数据库，也不替代客户端联调。

成功的 Release 构建上传 `Windows-x64-release` 产物，包含：

- `BubbleFighter-CN0280-Server-win-x64.zip`：一个服务端 EXE、MySQL 客户端 DLL、公开配置模板、发布资源、迁移和许可证。解压后位于 `BubbleFighter-CN0280-Server-win-x64` 目录，EXE 与 `config`、`data`、`migrations`、`licenses` 同级。
- `SHA256SUMS`：ZIP 的 SHA-256 校验值。包内 `build-info.json` 记录源码提交和逐文件校验值。

本地打包须先完成 Release 构建，再执行 `py -3 build/package_release.py`。打包器从仓库的公开模板读取配置，不复制输出目录里的私人配置或密码；第三方许可证缺失时会拒绝打包。

发布新版本时，先等待目标提交的 Windows CI 全部成功，再创建版本标签，例如：

```powershell
git tag v0.2.80.1 <通过CI的提交SHA>
git push origin v0.2.80.1
```

[Publish Release](https://github.com/K1roSama/BubbleFighter_CN0280_Server/actions/workflows/release.yml) 会下载该提交的成功 CI 产物，验证 ZIP 校验值、源码提交和包内文件后上传并发布 Release。该流程也可从 Actions 页面选择已有标签手动重试；已发布的版本不会被覆盖。CI 产物保留 14 天，超期后需对标签对应的提交重新运行 Windows CI。

下载发布包后，可在解压前核对校验值：

```powershell
Get-FileHash .\BubbleFighter-CN0280-Server-win-x64.zip -Algorithm SHA256
Get-Content .\SHA256SUMS
```

## 数据库准备

### MySQL 实例

数据库使用 MySQL 8.4.11、TCP、TLS、`utf8mb4` 和 `utf8mb4_0900_bin`。Windows 实例必须在首次初始化时使用 `lower_case_table_names=1`。已初始化实例不能通过修改该选项直接切换表名大小写策略。

如果已有符合要求的 MySQL，直接创建独立数据库即可。新实例可参考以下 `my.ini`，将目录改为实际路径，并预先创建数据目录的父目录：

```ini
[mysqld]
basedir=C:/MySQL/mysql-8.4.11-winx64
datadir=C:/MySQL/bubble-fighter-data
bind-address=127.0.0.1
port=3306
mysqlx=OFF
lower_case_table_names=1
require_secure_transport=ON
skip-log-bin
character-set-server=utf8mb4
collation-server=utf8mb4_0900_bin
```

此配置用于本机独立实例，`skip-log-bin` 关闭二进制日志。需要复制或时间点恢复的部署应按自身策略启用二进制日志，并处理下述函数创建权限。

仅对新建空数据目录执行 `mysqld --defaults-file=C:\MySQL\my.ini --initialize --console`，记录初始化输出中的临时 root 密码。再用同一配置启动 MySQL，并通过 `mysql --host=127.0.0.1 --port=3306 --ssl-mode=REQUIRED --user=root --password` 登录，按 MySQL 提示修改初始密码。也可以按 MySQL 官方流程安装 Windows 服务。

### 数据库与账号

以下 SQL 由数据库管理员执行，密码均需替换为独立的随机值：

```sql
CREATE DATABASE bubble_fighter
  CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;

CREATE USER 'bf_migrate'@'127.0.0.1'
  IDENTIFIED BY 'REPLACE_WITH_MIGRATION_PASSWORD' REQUIRE SSL;
GRANT ALL PRIVILEGES ON bubble_fighter.* TO 'bf_migrate'@'127.0.0.1';

CREATE USER 'bf_runtime'@'127.0.0.1'
  IDENTIFIED BY 'REPLACE_WITH_RUNTIME_PASSWORD' REQUIRE SSL;
GRANT SELECT, INSERT, UPDATE, DELETE, EXECUTE
  ON bubble_fighter.* TO 'bf_runtime'@'127.0.0.1';
```

迁移账号应在后续升级和校验期间继续存在，因为函数和触发器使用创建者作为 `DEFINER`。使用其他账号校验既有函数时，可能还需要 `SHOW_ROUTINE` 权限；应由数据库管理员按部署方式授予。

启用二进制日志的实例对创建函数和触发器有额外权限要求，仅授予目标库的 `ALL PRIVILEGES` 可能不足。由数据库管理员执行迁移，或根据 MySQL 部署策略配置 `log_bin_trust_function_creators` 及所需权限。

### 执行迁移

将输出目录中的 `config/examples/migrate.example.conf` 复制为 `config/migrate.conf`，确认数据库地址、库名和 `bf_migrate` 账号。模板从 `BF_MYSQL_MIGRATE_PASSWORD` 环境变量获取密码。

以下以源码构建后的目录为例；使用发布包时将 `$bin` 改为解压后的 `BubbleFighter-CN0280-Server-win-x64` 绝对路径：

```powershell
$bin = Resolve-Path .\Artifacts\Bin\x64\Release
Copy-Item "$bin\config\examples\migrate.example.conf" "$bin\config\migrate.conf"
$secret = Read-Host 'Migration database password' -AsSecureString
$env:BF_MYSQL_MIGRATE_PASSWORD = [Net.NetworkCredential]::new('', $secret).Password
try {
    & "$bin\BubbleFighter.Server.exe" --mode migrate `
      --config "$bin\config\migrate.conf" --migrations "$bin\migrations"
    if ($LASTEXITCODE) { throw 'Database migration failed.' }
} finally {
    Remove-Item Env:BF_MYSQL_MIGRATE_PASSWORD -ErrorAction SilentlyContinue
}
```

首次迁移依次执行版本 1 的结构与种子数据，以及版本 2 的历史服务清理，完成后 schema 版本为 `2`，报告 `applied=2`。已有版本 1 的数据库使用相同命令升级，报告 `previous=1 current=2 applied=1`。重复执行会校验并跳过已完成步骤，报告 `applied=0`。中断后使用相同迁移文件续跑；不要改动已应用的 SQL 或校验合同，也不要绕过迁移器手动拆分执行。

版本 2 会删除 `AccountEmails`、`EmailBindingSessions`、`EmailVerificationChallenges`、`ForensicsAttachments`、`ForensicsReports`、`SoftwareDetections`、`SoftwareBlocklist`，以及 `SoftwareBlocklistAction`、`SoftwareBlocklistBanSeconds` 函数。执行前停止所有连接该库的游戏与认证服务，并完整备份数据库；这些旧表中的数据会被删除。

设备码封禁结构保留，包括 `MachineDevices`、`AccountDevices`、`AccountDeviceLogins`、`ModerationSanctions`、`ModerationAudit`，以及设备码字段、外键和 `AccountBanActive` 函数。该迁移不删除或撤销设备、账号处罚及审计记录。旧版 `0001_*` 文件作为已应用迁移的历史输入保持不变，新库完成两次迁移后的最终结构不包含已移除的表。

### 配置运行账号

编辑 `$bin/config/server.conf`，至少确认：

```ini
database.host=127.0.0.1
database.port=3306
database.name=bubble_fighter
database.user=bf_runtime
database.password_file=mysql.password
database.tls=required
database.expected_schema_version=2
```

将运行账号密码写入同目录的 `mysql.password`，替换模板的 `CHANGE_ME`。密码文件是 UTF-8 单行文本，支持 BOM 和 CRLF，路径相对配置文件解析。限制该文件的 Windows 访问权限。

升级现有部署时，将原有主服务和独立登录配置的 `database.expected_schema_version` 改为 `2`。构建会保留私人主配置，因此不会自动替换旧配置中的版本号；数据库和程序必须一起升级，版本 1 的旧程序不适用于升级后的数据库。

也可用 `database.password_env` 指向密码环境变量，但不能与 `database.password_file` 同时使用。远程 MySQL 使用 `database.tls=verify_identity`，并设置 `database.ca_file`。

验证后即可双击 EXE：

```powershell
& "$bin\BubbleFighter.Server.exe" --check-config
& "$bin\BubbleFighter.Server.exe" --check-database
```

## 部署目录

部署时复制完整 Release 输出目录，或解压发布包；EXE、三个 MySQL 客户端 DLL、`config`、`data` 和 `migrations` 应保持相对位置。部署机无需 VS、Python 或 vcpkg，但需要匹配的 Visual C++ 运行库、已启动的 MySQL 和已配置的数据库。

备份包含账号数据的 MySQL 数据库，并单独保管私有配置。升级前停止服务端；备份、迁移和恢复应使用相同版本的 MySQL 工具与应用迁移文件。不要把密码、数据库数据或日志提交到源码仓库。

更多运行参数、客户端接入和排查方法见[运行与接入](usage.md)。
