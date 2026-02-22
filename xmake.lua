-- 项目名称和版本
set_project("RedstoneOptimizer")
set_version("1.0.0")

-- 添加自定义仓库，确保后续依赖能正确找到
add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

-- 固定 LeviLamina 的 target_type 为 "server"
add_requires("levilamina 1.9.5", {configs = {target_type = "server"}})
add_requires("levibuildscript")

-- 运行时库设置（如果用户未通过命令行指定）
if not has_config("vs_runtime") then
    set_runtimes("MD")
end

-- 编译器选项：按平台分别设置
if is_plat("windows") then
    -- Windows 专用选项
    add_cxflags(
        "/EHa",        -- 启用 SEH 异常（可保留，不影响）
        "/utf-8",      -- 设置源文件编码为 UTF-8
        "/W4",         -- 警告等级 4
        "/w44265",
        "/w44289",
        "/w44296",
        "/w45263",
        "/w44738",
        "/w45204"
    )
else
    -- 其他平台（如 Linux）选项
    add_cxflags(
        "-O2",
        "-march=native",
        "-flto=auto"
    )
end

-- 通用定义和设置
add_defines("NOMINMAX", "UNICODE")
set_languages("c++20")
set_optimize("fast")
set_symbols("none")
set_exceptions("none")   -- 禁用 C++ 异常（若需 SEH，可改为 set_exceptions("seh")）

add_includedirs("src")

-- 主目标
target("RedstoneOptimizer")
    add_rules("@levibuildscript/linkrule")
    add_rules("@levibuildscript/modpacker")
    set_kind("shared")
    set_languages("c++20")
    add_headerfiles("src/*.h")
    add_files("src/*.cpp")
    add_packages("levilamina")
    -- 添加系统链接库（根据实际需要，可保留或移除）
    add_syslinks("shlwapi", "advapi32")
    set_targetdir("bin")
    set_runtimes("MD")   -- 确保 target 使用 MD 运行时
