# Libraries use static linkage. The C runtime uses the matching MSVC DLL.
# C 运行库仍使用 DLL，与 Visual Studio 应用默认的 /MD[d] 保持一致。
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
# 固定与项目 `Visual Studio 17 2022` 生成器一致的 v143；本机并存 VS2026 时禁止
# vcpkg 自动选择其 v145 编译器，否则依赖与服务目标会混用不同 MSVC ABI。
set(VCPKG_PLATFORM_TOOLSET v143)
