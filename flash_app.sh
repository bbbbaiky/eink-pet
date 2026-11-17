#!/usr/bin/env bash
# ============================================================================
# ★R144 重写：本脚本原来写死 write_flash 0x10000 —— 分区表改版后 0x10000 是
# otadata(8KB)，会把 1.7MB 的 app 烧进去，破坏 OTA 记录且不生效（静默软砖）。
# 现在一律转发给 build2.sh app：由 app_off.py 从设备读回真实 app 偏移，
# 写完自动擦 otadata，NVS 不动。
#
#   bash flash_app.sh            → 烧 COM5
#   bash flash_app.sh COM7       → 指定串口
# ============================================================================
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd -W)"
PORT="${1:-COM5}"
exec bash "$HERE/build2.sh" app "$PORT"
