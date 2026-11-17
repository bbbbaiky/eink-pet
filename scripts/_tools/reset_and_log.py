#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""复位一次并抓启动日志（只为确认烧完能正常起来、没有 panic）。

用法： python reset_and_log.py COM5 8
注意：必须用 IDF 的 python（系统 python 没有 pyserial）。

★ 坑（本机实测）：ESP32-S3 走 USB-Serial-JTAG 口时，复位会触发 USB 重新枚举，
  复位前打开的串口句柄直接失效 → 只能读到 0 行。所以复位后必须关掉、
  等 USB 重新出现，再重新 open 去读。
"""
import sys
import time

import serial
from serial.tools import list_ports

port_name = sys.argv[1] if len(sys.argv) > 1 else 'COM5'
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0


def open_port(name):
    s = serial.Serial()
    s.port = name
    s.baudrate = 115200
    s.dtr = False
    s.rts = False
    s.timeout = 0.2
    s.open()
    return s


# 1) 复位：RTS 拉低 = EN 拉低（复位），保持 DTR=False 让 IO0 保持高（不进下载模式）
s = open_port(port_name)
s.rts = True
time.sleep(0.12)
s.rts = False
time.sleep(0.3)
s.close()

# 2) 等 USB 重新枚举（最多 6 秒）
t = time.time()
while time.time() - t < 6:
    names = {p.device.upper() for p in list_ports.comports()}
    if port_name.upper() in names:
        break
    time.sleep(0.15)
time.sleep(0.35)

# 3) 重新打开读日志
buf = b''
try:
    s = open_port(port_name)
    t0 = time.time()
    while time.time() - t0 < secs:
        buf += s.read(4096)
    s.close()
except serial.SerialException as e:
    print('!! 重开串口失败：%s' % e)

txt = buf.decode('utf-8', 'replace')
lines = [l for l in txt.splitlines() if l.strip()]
print('=== %s 抓到 %d 行 ===' % (port_name, len(lines)))
for l in lines[:45]:
    print(l[:150])
bad = [l for l in lines if 'panic' in l.lower() or 'Guru Meditation' in l or 'abort()' in l]
print('\n=== panic 检查 ===')
print('发现 panic/abort：%d 行' % len(bad))
for l in bad[:6]:
    print('   ', l[:160])
