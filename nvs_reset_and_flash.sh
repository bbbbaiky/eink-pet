#!/usr/bin/env bash
# ============================================================================
# 把设备恢复成「没有网络记忆」的状态 —— 先诊断、再重置、再烧录、再抓日志
#
#   bash hw/nvs_reset_and_flash.sh            # 默认 COM5
#   bash hw/nvs_reset_and_flash.sh COM7
#   PET_FLASH_WAIT=60 bash hw/nvs_reset_and_flash.sh    # 只等 60 秒
#
# 为什么要有这个脚本（2026-09-19 用户要求）：
#   用户反馈「刷固件后不进配网了」+「我要大概测试一下配网的功能，帮我回到没有
#   网络记忆的状态」。刷完固件要能重新走一遍配网，就得让 NVS 里**没有** petns/ssid。
#
#   而 erase_flash 会连 app 一起擦掉，所以顺序是：
#     ① 先把 NVS 读出来看（诊断：到底有没有网络记忆）→ 不猜，直接看字节
#     ② 再整片擦除 + 重烧（build2.sh 自带 erase_flash）→ NVS 一定干净
#     ③ 冷启动（esptool 复位）→ 没有配置 + 不是深睡唤醒 → **必然进配网页**
#
# ⚠ 前提：板子要能被看见。深睡时 USB-Serial-JTAG 会断电、COM 口整个消失，
#   所以**拔插一次 USB**（或按一下 POWER 唤醒）再来。
# ============================================================================
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd -W)"
PORT="${1:-COM5}"
WAIT_MAX="${PET_FLASH_WAIT:-1800}"
# ★ 2026-10-01：工具链 Python 不写死（ESP-IDF 两代安装器布局不同，见 hw/find_idf_py.sh）
. "$HERE/find_idf_py.sh"
PY_ESP="$(find_idf_py)" || { echo "!! 找不到 ESP-IDF 的 Python 环境（见 hw/find_idf_py.sh）"; exit 1; }
PY="$PY_ESP"

NVS_OFF=0x9000
NVS_LEN=0x6000          # 24K，见分区表：nvs,data,nvs,0x9000,24K
NVS_BIN="$HERE/_nvs_before.bin"

echo ">>> 等待 $PORT 出现（最多 $((WAIT_MAX / 60)) 分钟；深睡时请拔插一次 USB 或按 POWER）"

WAIT=0
FOUND=""
while [ "$WAIT" -lt "$WAIT_MAX" ]; do
  FOUND=$("$PY" -c "
import serial.tools.list_ports as lp
print('$PORT' if '$PORT' in [p.device for p in lp.comports()] else '')" 2>/dev/null)
  if [ -n "$FOUND" ]; then
    echo ">>> 检测到 $PORT ✓（等了 ${WAIT}s）"
    sleep 2
    break
  fi
  if [ $((WAIT % 20)) -eq 0 ]; then
    echo "    ... 还没有（${WAIT}s）"
  fi
  sleep 2
  WAIT=$((WAIT + 2))
done

if [ -z "$FOUND" ]; then
  echo "!! $((WAIT_MAX / 60)) 分钟内未检测到 $PORT，放弃。"
  exit 1
fi

echo
echo "=========== ① 读 NVS 分区（$NVS_OFF / $NVS_LEN）做诊断 ==========="
# ★ 2026-09-19 踩坑：这里原来写成 `>/dev/null 2>&1`，把 esptool 的报错**全吞了**，
#   结果只看到"读 NVS 失败"却不知道为什么（实测真的失败过一次）。
#   读 flash 是唯一一步"必须在设备正常跑着时做"的操作，容易和别的串口占用打架
#   （比如同时挂着 watch_serial.py）→ 所以：**打印全部错误 + 重试 2 次**。
NVS_OK=0
for attempt in 1 2 3; do
  if "$PY" -m esptool -p "$PORT" --before default_reset read_flash \
       $NVS_OFF $NVS_LEN "$NVS_BIN" 2>&1 | tail -6; then
    if [ -s "$NVS_BIN" ]; then NVS_OK=1; break; fi
  fi
  echo "    (第 $attempt 次读 NVS 失败，2 秒后重试；若提示 port busy，先确认没有别的脚本占着串口)"
  sleep 2
done

if [ "$NVS_OK" = "1" ]; then
  echo
  "$PY" "$HERE/nvs_report.py" "$NVS_BIN"
else
  echo "!! 读 NVS 三次都没成功（不阻断，继续烧录）—— 反正下一步 erase_flash 也会把它清空"
fi

echo
echo "=========== ② 整片擦除 + 烧录（build2.sh）==========="
bash "$HERE/build2.sh" all "$PORT" || { echo "!! 烧录失败"; exit 1; }   # ★R171：默认已改 app，整片擦除必须显式 all

echo
echo "=========== ③ 抓 40 秒启动日志（看是否进配网）==========="
"$PY" "$HERE/watch_serial.py" 40 "$HERE/_boot_after_reset.log"

echo
echo "=========== ④ 日志要点 ==========="
grep -aE "启动原因|没有 WiFi 配置|配网热点已开|配网引导|页：" "$HERE/_boot_after_reset.log" 2>/dev/null | head -20
echo
echo "期望看到（三行都要有）："
echo "  启动原因：上电 / 复位 (cause=0)        ← 必须是冷启动，不是深睡唤醒"
echo "  没有 WiFi 配置 → 进入配网模式（仅冷启动）"
echo "  配网热点已开：SSID=PetCat-XXXX（无密码）"
