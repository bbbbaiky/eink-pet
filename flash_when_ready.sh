#!/usr/bin/env bash
# ============================================================================
# 等板子插好 → 自动烧录 → 自动抓 30 秒串口日志
#
#   bash hw/flash_when_ready.sh              # 默认 COM5
#   bash hw/flash_when_ready.sh COM7         # 指定串口
#
# 用途：板子掉线时（COM 口消失），插上线后不用手动重跑，脚本自己等。
# ============================================================================
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd -W)"
PORT="${1:-COM5}"
WAIT_MAX="${PET_FLASH_WAIT:-1800}"   # 默认等 30 分钟；PET_FLASH_WAIT=秒 可覆盖
# ★ 2026-10-01：工具链 Python 不写死（ESP-IDF 两代安装器布局不同，见 hw/find_idf_py.sh）
. "$HERE/find_idf_py.sh"
PY_ESP="$(find_idf_py)" || { echo "!! 找不到 ESP-IDF 的 Python 环境（见 hw/find_idf_py.sh）"; exit 1; }
PY="$PY_ESP"

echo ">>> 等待 $PORT 出现（最多 $((WAIT_MAX / 60)) 分钟，请把板子插上 USB）"

WAIT=0
while [ "$WAIT" -lt "$WAIT_MAX" ]; do
  FOUND=$("$PY" -c "
import serial.tools.list_ports as lp
ports = [p.device for p in lp.comports()]
print('$PORT' if '$PORT' in ports else '')
" 2>/dev/null)
  if [ -n "$FOUND" ]; then
    echo ">>> 检测到 $PORT ✓（等了 ${WAIT}s）"
    sleep 2          # 等驱动枚举稳定
    break
  fi
  if [ $((WAIT % 10)) -eq 0 ]; then
    echo "    ... 还没有（${WAIT}s），已发现的串口：$("$PY" -c "
import serial.tools.list_ports as lp
print(','.join(p.device for p in lp.comports()) or '无')
" 2>/dev/null)"
  fi
  sleep 2
  WAIT=$((WAIT + 2))
done

if [ -z "${FOUND:-}" ]; then
  echo "!! $((WAIT_MAX / 60)) 分钟内未检测到 $PORT，放弃。请检查线材/驱动。"
  exit 1
fi

echo
echo ">>> 开始烧录"
bash "$HERE/build2.sh" app "$PORT"   # ★R171 起默认就是 app（保 NVS）；显式写出来只为口径清楚
RC=$?
[ "$RC" != "0" ] && { echo "!! 烧录失败"; exit $RC; }

echo
echo ">>> 抓 35 秒串口日志（看启动 + 联网同步结果）"
"$PY" "$HERE/watch_serial.py" 35 "$HERE/_boot_v6.log"

echo
echo ">>> 日志已存 $HERE/_boot_v6.log"
echo "    重点看这几行："
echo "      LUT[153..158]      → 应为 22 17 41 00 32 20"
echo "      没有 WiFi 配置      → 应进入配网模式"
echo "      同步结果：时段=..   → 真实时段/天气/月相"
echo "      发现热点 PetCat-    → 用手机连它配网"
