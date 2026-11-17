# -*- coding: utf-8 -*-
"""复位一次并旁观开机日志（只用于「这版固件真跑起来了」的证据抓取）。

为什么要单写一个：`watch_serial.py` 刻意**不复位**（它是给「长按睡着没」用的旁观工具）。
本脚本相反 —— 主动拉一次 RTS 复位，把 boot → 首帧 → 进主循环那一段全抓下来。

用法： python hw/boot_log_probe.py [COM口] [秒数]
"""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else 'COM5'
SECS = int(sys.argv[2]) if len(sys.argv) > 2 else 14

# ★ 与 watch_serial.py 同款铁律：必须在 open() 之前把 DTR/RTS 置 False，
#   否则 pyserial 初始化时会把两根线拉高 → ESP32-S3 被 USB_UART_CHIP_RESET 复位。
p = serial.Serial()
p.port = PORT
p.baudrate = 115200
p.timeout = 0.2
p.dtr = False
p.rts = False
p.open()

# 拉低 RTS 一下 = 拉 EN 脚 = 硬复位
# ★R145：USB-Serial/JTAG（本板 USB mode: USB-Serial/JTAG）下只 toggle RTS **不复位** ——
#   CDC 控制线要 DTR 同拍才认。口径照抄 esptool 的 HardReset(uses_usb=True)：
#       setRTS(True); setDTR(dtr); 0.2s; setRTS(False); setDTR(dtr); 0.2s
#   （DTR 全程不变，只是每步显式重写一次，逼 CDC 把控制线送下去。）
#   实测：0.12s 的单拉 RTS 在这块板子上抓不到 boot 段，只能听到"已经在跑"的那几行。
p.dtr = False
p.rts = True
p.dtr = False
time.sleep(0.2)
p.rts = False
p.dtr = False
time.sleep(0.2)

t0 = time.time()
buf = b''
while time.time() - t0 < SECS:
    try:
        n = p.in_waiting
        chunk = p.read(n if n else 1)
        if chunk:
            buf += chunk
            sys.stdout.write(chunk.decode('utf-8', 'replace'))
            sys.stdout.flush()
    except Exception as e:
        print('\n[读串口异常] %r' % e)
        break

p.close()
print('\n=== 共 %d 字节 / %.1f 秒 ===' % (len(buf), SECS))
