# -*- coding: utf-8 -*-
"""
app_off.py —— 读回设备上的分区表，打印**第一个 app 分区的偏移**
=================================================================================
为什么需要它（2026-10-16 OTA 改分区表踩到的坑）
-----------------------------------------------
工程分区表从「factory 6M @0x10000」改成「ota_0/ota_1 各 2.8M @0x20000」后：
  · 旧表：app 在 **0x10000**
  · 新表：app 在 **0x20000**（0x10000 变成 otadata 8K！）
烧录脚本原来写死 0x10000 ⇒ 拿新表的设备用旧脚本烧，app 会写进 otadata、
**屏上毫无变化**（静默失败，最难查的那种）。

⇒ 本脚本在每次烧 app 前读回设备分区表，按表算出真正的 app 偏移：
   表里有 factory  → 0x10000（旧设备，未迁移）
   表里只有 ota_0  → ota_0 的偏移（新设备，已迁移）
读不到/解析失败 → 返回 0x10000（= 旧行为，绝不猜一个新偏移乱写）。

用法：python app_off.py <PORT> <esptool.py 路径>
输出：一行十六进制偏移，如 0x10000 或 0x20000
"""
import os
import struct
import subprocess
import sys
import tempfile

MAGIC = 0x50AA
TYPE_APP = 0
SUB_FACTORY, SUB_OTA_0 = 0x00, 0x10


def main():
    if len(sys.argv) < 3:
        print('0x10000')
        return
    port, esptool = sys.argv[1], sys.argv[2]
    tmp = os.path.join(tempfile.gettempdir(), 'dev_partitions.bin')
    try:
        subprocess.run([sys.executable, '-m', 'esptool', '-p', port,
                        'read_flash', '0x8000', '0xC00', tmp],
                       capture_output=True, timeout=15)
        d = open(tmp, 'rb').read()
    except Exception:
        print('0x10000')
        return

    best = None
    for i in range(len(d) // 32):
        e = d[i * 32:(i + 1) * 32]
        magic, typ, sub = struct.unpack('<HBB', e[0:4])
        if magic != MAGIC:
            continue
        off, _size = struct.unpack('<II', e[4:12])
        if typ != TYPE_APP:
            continue
        # factory 优先（旧表），否则取第一个 ota_*
        if sub == SUB_FACTORY:
            best = off
            break
        if best is None:
            best = off
    print(hex(best) if best else '0x10000')


if __name__ == '__main__':
    main()
