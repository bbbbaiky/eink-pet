# -*- coding: utf-8 -*-
"""盯一段时间串口，用来验证「长按 PWR 到底睡没睡」。

刻意不做 RTS 复位 —— 这里是旁观，不能把设备重置掉。
★ 2026-09-19 踩坑：光「在 open() 之后 setDTR/setRTS(False)」是不够的，open() 那一刻
   pyserial 已经默认把 DTR/RTS 拉高过，ESP32-S3 会被 USB_UART_CHIP_RESET 复位一次。
   必须在 open() **之前**设 port.dtr/port.rts = False（见下面 _open 处注释）。
写文件的同时也打到 stdout。

用法：  python hw/watch_serial.py [秒数] [输出文件]
"""
import sys
import time

import serial

secs = int(sys.argv[1]) if len(sys.argv) > 1 else 180
out = sys.argv[2] if len(sys.argv) > 2 else '_pwr_test.log'

t0 = time.time()
buf = b''
shown = 0
port = None
while time.time() - t0 < secs:
    try:
        if port is None:
            # ★ 必须在 open() 之前就把 DTR/RTS 置 False。
            #   pyserial 的 SerialBase 初始化时 _dtr_state/_rts_state 都是 True，
            #   open() 那一刻会真的把两根线拉高 → ESP32-S3 的 USB-Serial-JTAG 会把板子
            #   复位（实测日志首行 rst:0x15 (USB_UART_CHIP_RESET)）。
            #   那样「挂个监听旁观」就变成了「顺手把设备重启一次」，会打断正在进行的
            #   配网/按键测试。用「先建空对象、设好 dtr/rts、再 open」的顺序可以避免。
            port = serial.Serial()
            port.port = 'COM5'
            port.baudrate = 115200
            port.timeout = 0.3
            port.dtr = False
            port.rts = False
            port.open()
            sys.stdout.write('=== 开始监听 COM5（%d 秒，未复位板子）===\n' % secs)
            sys.stdout.flush()
        chunk = port.read(4096)
        if chunk:
            buf += chunk
            txt = buf.decode('utf-8', 'replace')
            if len(txt) > shown:
                sys.stdout.write(txt[shown:])
                sys.stdout.flush()
                shown = len(txt)
    except Exception as e:                      # 入睡时 USB 可能掉线
        sys.stdout.write('\n=== 串口中断：%s ===\n' % e)
        sys.stdout.flush()
        try:
            if port:
                port.close()
        except Exception:
            pass
        port = None
        time.sleep(1.0)

try:
    if port:
        port.close()
except Exception:
    pass

with open(out, 'w', encoding='utf-8', errors='replace') as f:
    f.write(buf.decode('utf-8', 'replace'))
sys.stdout.write('\n=== 监听结束，已存 %s ===\n' % out)
