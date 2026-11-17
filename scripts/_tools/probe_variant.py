# -*- coding: utf-8 -*-
"""抓启动日志判断串口板子变体（按键版 / 触屏版）。
先 DTR/RTS 组合复位（ESP32-S3 经典：RTS=EN, DTR=GPIO0），再读 6s 串口。"""
import sys, time, serial

def grab(port, secs=6):
    s = serial.Serial(port, 115200, timeout=0.2)
    # 进下载模式再退出 → 等价于按复位
    s.dtr = False; s.rts = True;  time.sleep(0.1)
    s.dtr = True;  s.rts = False; time.sleep(0.1)
    s.dtr = False; s.rts = False
    buf = b''
    t0 = time.time()
    while time.time() - t0 < secs:
        buf += s.read(4096)
    s.close()
    return buf.decode('utf-8', 'replace')

for port in sys.argv[1:]:
    txt = grab(port)
    print('==== %s (%d bytes) ====' % (port, len(txt)))
    # 只打可能含变体标识的行
    keys = ['按键', '触屏', 'TOUCH', 'DEMO', '版本', 'version', ' pet', '墨水屏', '启动', 'boot']
    hits = [l for l in txt.splitlines() if any(k in l for k in keys)]
    for l in hits[:25]:
        print(' ', l.strip()[:150])
    if not hits:
        print('  (无关键词行，头 5 行:)')
        for l in txt.splitlines()[:5]:
            print(' ', l.strip()[:150])
