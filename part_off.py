# -*- coding: utf-8 -*-
"""
part_off.py —— 从 **构建产物 partition-table.bin** 里读某个分区的偏移/大小（2026-10-16 拆分⑦）
=================================================================================
为什么需要它：烧录脚本要在 migrate / 整片两条路径里把 assets.bin 写进 assets 分区，
    而这个偏移是 gen_esp32part 从 partitions.csv **算出来的**（csv 里 assets 的偏移列是空的）。
    写死一个数迟早对不上（改分区表就漂）——所以直接问**真正要烧的那张表**。

用法：
    python fw/part_off.py <partition-table.bin> <label> [off|size]
默认打印偏移（0x…）；取不到时打印空串并以 1 退出（调用方据此报错）。

分区表条目（32B，全 LE）：magic(2)=0x50AA · type(1) · subtype(1) · offset(4) · size(4) ·
    label(16) · flags(4)
"""
import struct
import sys


def main():
    if len(sys.argv) < 3:
        print('用法：python fw/part_off.py <partition-table.bin> <label> [off|size]',
              file=sys.stderr)
        return 2
    path, label = sys.argv[1], sys.argv[2]
    what = sys.argv[3] if len(sys.argv) > 3 else 'off'
    data = open(path, 'rb').read()
    for i in range(0, len(data) - 31, 32):
        e = data[i:i + 32]
        if e[0] != 0xAA or e[1] != 0x50:      # 0x50AA 小端 = AA 50
            continue
        off, size = struct.unpack('<II', e[4:12])
        name = e[12:28].split(b'\x00')[0].decode('utf-8', 'replace')
        if name == label:
            print('0x%X' % (off if what == 'off' else size))
            return 0
    print('', end='')
    return 1


if __name__ == '__main__':
    sys.exit(main())
