# -*- coding: utf-8 -*-
"""抓一段串口日志（复位后 ~N 秒），用来验证开机路径 —— 2026-10-16 OTA 拆分验收用。

用法: python fw/_cap_serial.py COM5 [秒数]
复位方式 = DTR/RTS 组合（CH34x / CP210x 的老套路；本板「USB 串行设备」实测有效）：
    RTS=1(EN=0 复位) → 松手 → 芯片重新跑 bootloader + app。
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else 'COM5'
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0

s = serial.Serial(port, 115200, timeout=0.2)
# 复位：RTS 拉低 EN 一小会儿（DTR 保持高 = BOOT 不按下）
s.setDTR(False)
s.setRTS(True)
time.sleep(0.12)
s.setRTS(False)
time.sleep(0.05)

t0 = time.time()
buf = bytearray()
while time.time() - t0 < secs:
    chunk = s.read(4096)
    if chunk:
        buf += chunk
s.close()

sys.stdout.write(buf.decode('utf-8', 'replace'))
sys.stdout.write('\n')
print('---- 共 %d 字节 ----' % len(buf))
