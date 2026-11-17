#!/usr/bin/env bash
# ============================================================================
# 构建 + 烧录「四图切换」验证固件
#
#   bash hw/build_flash.sh                    # 编译 + 烧录到 COM5
#   bash hw/build_flash.sh COM7               # 指定串口
#   bash hw/build_flash.sh only               # 只编译
#   bash hw/build_flash.sh clean              # 先全清再编译+烧录
#
# 烧录前会整片擦除 flash。别慌 —— 小智的完整备份在
# hw/backup/xiaozhi_full_8MB_20260914.bin，用 hw/restore_xiaozhi.sh 一键还原。
# ============================================================================
set -euo pipefail

# ★R144 废弃护栏：本脚本 erase_flash 后只跑 idf.py flash，不回写 assets 分区，
# 开机 assets_boot_check() 会停在兜底页；且整片擦除丢 NVS。
echo "!! build_flash.sh 已废弃（R144）。请改用："
echo "   bash build2.sh app COM5        # 只烧 app，保 NVS（构建目录自动按仓库位置推导）"
echo "   bash build2.sh migrate COM5    # 改分区表才用"
echo "   bash build2_touch.sh app COM7  # 触屏"
exit 1

HERE="$(cd "$(dirname "$0")" && pwd -W)"
PROJ="$HERE/fw_epaper_demo"

# ★ 构建产物的落点 —— **故意放到工作区外面**。
#   ninja 的 build/ 会生成 126MB / 1600+ 个文件（.cmake/.o/.a/.elf），
#   留在工作区里会让 WorkBuddy 的文件索引/监听一直在扫，界面就卡。
#   想换位置：export PET_BUILD_DIR=D:/xxx 再跑本脚本。
EPAPER_ROOT="$(cd "$(dirname "$0")/../.." && pwd -W)"
BUILD_DIR="${PET_BUILD_DIR:-${PET_BUILD_BASE:-$EPAPER_ROOT/_build}/fw_epaper_demo}"
mkdir -p "$BUILD_DIR"

# esptool 用哪个 python：优先 Espressif 的 venv（一定有 esptool）；
# 退回到工作区同级的工程 venv（<epaper 根>/_heavy/venv-pet-epaper，见 find_idf_py.sh）。
# ★ 2026-10-01：工具链 Python 不写死（ESP-IDF 两代安装器布局不同，见 hw/find_idf_py.sh）
. "$HERE/find_idf_py.sh"
PY_ESP="$(find_idf_py)" || { echo "!! 找不到 ESP-IDF 的 Python 环境（见 hw/find_idf_py.sh）"; exit 1; }
ESPTOOL_PY="$PY_ESP"

PORT="COM5"
MODE="all"
CLEAN=0
for a in "$@"; do
  case "$a" in
    COM*)  PORT="$a" ;;
    only)  MODE="only" ;;
    clean) CLEAN=1 ;;
    *)     echo "未知参数：$a" >&2; exit 2 ;;
  esac
done

# shellcheck source=idf_env.sh
source "$HERE/idf_env.sh"

cd "$PROJ"

if [ "$CLEAN" = "1" ]; then
  echo ">>> 全清：删 sdkconfig，旧 build/ 改名留档"
  rm -f sdkconfig
  if [ -d "$BUILD_DIR" ]; then
    mv "$BUILD_DIR" "$BUILD_DIR.old.$(date +%Y%m%d%H%M%S)"
    mkdir -p "$BUILD_DIR"
  fi
fi

if [ ! -f sdkconfig ]; then
  echo ">>> 首次配置：目标芯片 esp32s3"
  idf -B "$BUILD_DIR" set-target esp32s3
fi

echo ">>> 编译（产物落在 $BUILD_DIR，不占工作区）"
idf -B "$BUILD_DIR" build

if [ "$MODE" = "only" ]; then
  echo ">>> 只编译，未烧录"
  exit 0
fi

echo ">>> 整片擦除 $PORT（约 20 秒）"
"$ESPTOOL_PY" -m esptool -p "$PORT" erase_flash

echo ">>> 烧录 bootloader + 分区表 + app"
idf -B "$BUILD_DIR" -p "$PORT" -b 921600 flash

echo
echo ">>> 完成。看串口输出：  bash hw/build_flash.sh 里的 idf -B $BUILD_DIR -p $PORT monitor"
