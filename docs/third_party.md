# 第三方组件

项目代码使用根目录的 [MIT License](../LICENSE)。下列组件有各自许可证，分发源码或程序时应保留对应声明。

| 组件 | 用途 | 许可证位置 |
| --- | --- | --- |
| libjuice | ICE、UDP 网络与中继处理 | [third_party/libjuice/LICENSE](../third_party/libjuice/LICENSE)，源文件内另有声明 |
| Sensitive-lexicon 历史集成 | 昵称和聊天过滤的历史来源，当前词表为空 | [third_party/sensitive_lexicon/LICENSE](../third_party/sensitive_lexicon/LICENSE) |
| Boost.Asio | 异步网络 | vcpkg 安装目录的 `share/boost-asio` 及相关 Boost 包 |
| nlohmann/json | JSON 解析 | vcpkg 安装目录的 `share/nlohmann-json` |
| OpenSSL | 密码与 TLS | vcpkg 安装目录的 `share/openssl` |
| spdlog、fmt | 日志与格式化 | vcpkg 安装目录的 `share/spdlog`、`share/fmt` |
| MySQL C API | 数据库客户端 | Oracle MySQL SDK 的许可证和分发条款 |

vcpkg 组件的许可证随依赖恢复到 `Artifacts/Dependencies/vcpkg/x64-windows-static-md/share`。源码仓库不包含已编译依赖、MySQL 软件或其运行 DLL。Release 包包含 MySQL 客户端运行 DLL，及 `bin/licenses` 下的 MySQL、libjuice、词库历史来源和 vcpkg 依赖许可证；不包含 MySQL 数据库服务。分发二进制时需按使用组件的条款保留许可证。
