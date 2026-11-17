#!/usr/bin/env bash
# ============================================================================
# find_idf_py.sh — 定位 ESP-IDF 自带的 Python 环境（**不要把版本号写死**）
#
# 为什么单独一个文件（2026-10-01 踩坑）：
#   ESP-IDF 两代安装器的目录布局不同 ——
#     · 旧（tools 安装器 v1）：$IDF_TOOLS_PATH/tools/python/<ver>/venv/Scripts/python.exe
#     · 新（EIM 安装器，**本机就是这种**）：$IDF_TOOLS_PATH/python_env/idfX.Y_pyZ_env/Scripts/python.exe
#   hw/build2.sh 原本把 PY_ESP 写死成旧布局路径 → `run_env` 连 python 都起不来 →
#   ninja 2 秒就返回非 0，而且**不打印任何 error 行**（错误信息被那条 grep 过滤掉了），
#   表面现象是「编译失败但没原因」，极难查。所以改成运行时探测。
#
# 用法：
#     . "$HERE/find_idf_py.sh"
#     PY_ESP="$(find_idf_py)" || { echo "!! 找不到 IDF python"; exit 1; }
#
#     # 注意：IDF_PYTHON_ENV_PATH 要从找到的结果**反推**，别再单独写死：
#     export IDF_PYTHON_ENV_PATH="$(dirname "$(dirname "$PY_ESP")")"
# ============================================================================
find_idf_py() {
  # ★R217：允许显式指定（换电脑 / 自定义 IDF 环境时不必改脚本）
  if [ -n "${PET_IDF_PY:-}" ] && [ -f "${PET_IDF_PY}" ]; then
    printf '%s' "${PET_IDF_PY}"; return 0
  fi
  local base="${IDF_TOOLS_PATH:-C:/Espressif}" c
  local epaper_root
  epaper_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -W)"
  # 顺序即优先级：新布局 → 旧布局 → 本机备用 venv（路径按仓库相对位置推导）
  for c in \
    "$base"/python_env/idf*_env/Scripts/python.exe \
    "$base"/tools/python/*/venv/Scripts/python.exe \
    "C:/Espressif"/python_env/idf*_env/Scripts/python.exe \
    "C:/Espressif"/tools/python/*/venv/Scripts/python.exe \
    "$epaper_root/_heavy/venv-pet-epaper/Scripts/python.exe" ; do
    [ -f "$c" ] && { printf '%s' "$c"; return 0; }
  done
  return 1
}
