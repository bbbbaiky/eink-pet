#!/usr/bin/env bash
# ============================================================================
# ESP-IDF v5.5.3 构建环境（Git Bash 版）
#
#   source hw/idf_env.sh
#   idf build
#   idf -p COM5 flash
#
# ── 为什么所有路径都得是纯 ASCII ──────────────────────────────────────────
# 工具链里的 ld.exe 是 mingw 构建的。工具链一旦放在中文路径下
# （默认的 C:\Users\澄澄\.espressif），链接时就会报：
#     cannot find -lgcc / -lc / -lnosys
# gcc 自己能找到库（-print-file-name 打得出来），但 ld 打不开——
# 编码在 mingw 的 ANSI/UTF-8 之间对不上。
#
# 所以这边用的是 Espressif IDF Manager (EIM) 原来那套 ASCII 布局：
#     IDF_TOOLS_PATH       = C:\Espressif        （其下有 tools\ 和 dist\）
#     IDF_PATH             = C:\esp\v5.5.3\esp-idf
#     IDF_PYTHON_ENV_PATH  = C:\Espressif\python_env\idf5.5_py3.13_env
# ★ 2026-10-01 更正：原注释与 export 写的是 tools\python\v5.5.3\venv（**旧安装器布局**），
#   本机 ESP-IDF 是 EIM 装的，python 环境在 python_env\idf5.5_py3.13_env。
#   写死旧路径的后果：build2.sh 的 run_env 连 python 都起不来 → ninja 秒退、不吐 error 行。
#   现在各脚本统一走 hw/find_idf_py.sh 探测；这个文件只作手动 source 时的兜底。
# 工程自己的路径带中文没关系，编译输出到哪都没问题；
# 但 ccache 会崩，所以 IDF_CCACHE_ENABLE 关掉（见下）。
#
# 另外 idf.py 在 MSYS 下会直接不执行 main()，所以统一走 hw/idf_run.py。
# ============================================================================

export IDF_TOOLS_PATH="C:/Espressif"
export IDF_PATH="C:/esp/v5.5.3/esp-idf"
# ★ 不写死：能探测到就用探测结果（新布局 python_env / 旧布局 tools\python）
_IDF_PY_HIT="$(ls -1d C:/Espressif/python_env/idf*_env C:/Espressif/tools/python/*/venv 2>/dev/null | tail -1)"
export IDF_PYTHON_ENV_PATH="${_IDF_PY_HIT:-C:/Espressif/python_env/idf5.5_py3.13_env}"
export ESP_ROM_ELF_DIR="C:/Espressif/tools/esp-rom-elfs/20241011/"
export OPENOCD_SCRIPTS="C:/Espressif/tools/openocd-esp32/v0.12.0-esp32-20251215/openocd-esp32/share/openocd/scripts"

# 关掉 ccache：它的 C++ 版 std::filesystem 在中文路径上会抛
#   filesystem error: Cannot convert character sequence: Illegal byte sequence
# 而工程目录正好带中文。编译慢一点，但稳。
export IDF_CCACHE_ENABLE=0
export PYTHON="$IDF_PYTHON_ENV_PATH/Scripts/python.exe"

# Git Bash 不导出这个变量，Python 的 platform.machine() 会返回空串，
# idf_tools 就认不出平台（报 "Support for platform 'Windows-'"）。
: "${PROCESSOR_ARCHITECTURE:=AMD64}"
export PROCESSOR_ARCHITECTURE

# 故意不导出 PATH：在 Git Bash 里 export 带 C:/ 前缀的 PATH 会被 MSYS
# 二次加工成 C:\...\PortableGit\versions\1.2.0\Users\... 这种废路径。
# 改由 hw/idf_run.py 在 Python 内部用 Windows 原生路径重建 PATH。

_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -W)"
idf() { "$PYTHON" "$_HERE/idf_run.py" "$@"; }
