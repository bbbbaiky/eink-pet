#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""用 esptool 硬复位（保证退出下载模式、真正跑 app），随后立刻开串口抓启动日志。

用法： python run_app_and_log.py COM5 6
必须用 IDF 的 python 跑（要 pyserial + esptool）。

★ 本机两个坑：
 1) USB-Serial-JTAG 口复位会让 USB 重枚举 → 复位前开的句柄失效（0 行）。
 2) pyserial 打开瞬间默认拉高 DTR/RTS，会把芯片拽回下载模式，
    所以 open 之前必须先把 dtr/rts 置 False。
"""
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

port_name = sys.argv[1] if len(sys.argv) > 1 else 'COM5'
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0

# 1) esptool 硬复位（--after no_reset 之外的默认行为 = 复位后运行）
cmd = [sys.executable, '-m', 'esptool', '-p', port_name, '-b', '921600',
       '--after', 'hard_reset', 'read_mac']
r = subprocess.run(cmd, capture_output=True, text=True)
mac = [l for l in (r.stdout + r.stderr).splitlines() if 'MAC:' in l]
print('复位+读MAC: %s' % (mac[0] if mac else '未取到 MAC（rc=%d）' % r.returncode))

# 2) 等 USB 重新枚举，再尽快开串口（启动日志只打一次）
t = time.time()
while time.time() - t < 5:
    if port_name.upper() in {p.device.upper() for p in list_ports.comports()}:
        break
    time.sleep(0.03)

buf = b''
try:
    s = serial.Serial()
    s.port = port_name
    s.baudrate = 115200
    s.dtr = False
    s.rts = False
    s.timeout = 0.2
    s.open()
    t0 = time.time()
    while time.time() - t0 < secs:
        buf += s.read(4096)
    s.close()
except serial.SerialException as e:
    print('!! 开串口失败：%s' % e)

txt = buf.decode('utf-8', 'replace')
lines = [l for l in txt.splitlines() if l.strip()]
print('=== %s 抓到 %d 行 ===' % (port_name, len(lines)))
for l in lines[:40]:
    print(l[:150])
bad = [l for l in lines if 'panic' in l.lower() or 'Guru Meditation' in l or 'abort()' in l]
print('=== panic 检查：%d 行 ===' % len(bad))
for l in bad[:6]:
    print('   ', l[:160])
