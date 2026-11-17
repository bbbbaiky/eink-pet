# -*- coding: utf-8 -*-
"""
读回来的 NVS 分区（0x9000 / 24K）做诊断：到底有没有「网络记忆」。

不猜、不靠日志推断 —— 直接把 NVS 里的键值抠出来看。

用法：
    python nvs_report.py _nvs_before.bin

看什么：
    · 找得到 `petns` 命名空间 + `ssid`/`pass`/`city` 键  → **有网络记忆**（设备会直接去连它）
    · 一个都找不到                                        → **没有网络记忆**（那"不进配网"就只能是深睡唤醒跳过了）
"""
import sys
import os
import re


def strings(buf, minlen=3):
    """扫出所有可打印串（ASCII + UTF-8 中文），带文件偏移。"""
    out = []
    i = 0
    n = len(buf)
    while i < n:
        if 32 <= buf[i] < 127 or buf[i] >= 0x80:      # 可见 ASCII 或 UTF-8 高位
            j = i
            while j < n and (32 <= buf[j] < 127 or buf[j] >= 0x80):
                j += 1
            raw = buf[i:j]
            if len(raw) >= minlen:
                try:
                    s = raw.decode('utf-8')
                except UnicodeDecodeError:
                    s = raw.decode('latin-1')
                if re.search(r'[0-9A-Za-z\u4e00-\u9fff]', s):
                    out.append((i, s))
            i = j
        else:
            i += 1
    return out


def main():
    if len(sys.argv) < 2:
        print('用法: python nvs_report.py <nvs.bin>')
        return 1
    path = sys.argv[1]
    if not os.path.exists(path):
        print('!! 找不到 %s（可能读 NVS 那一步失败了）' % path)
        return 1

    buf = open(path, 'rb').read()
    nonzero = sum(1 for b in buf if b != 0xFF)
    print('文件 %s  大小 %d 字节' % (os.path.basename(path), len(buf)))
    print('非 0xFF 字节数 = %d  （0 表示整片是空白/已擦除）' % nonzero)
    if nonzero == 0:
        print()
        print('★ 结论：NVS 整片是全 0xFF = **完全空**，没有任何网络记忆。')
        return 0

    print()
    print('=== 关键命名空间 / 键（NVS 的 ns 与 key 都存在定长字段里）===')
    for kw in (b'petns', b'ssid', b'pass', b'city', b'scenery', b'geo_city', b'bowl'):
        idx = buf.find(kw)
        print('  %-10s %s' % (kw.decode(), ('命中 @ 0x%04X' % idx) if idx >= 0 else '未找到'))

    print()
    print('=== 全部可打印串（前 60 条）===')
    ss = strings(buf)
    for off, s in ss[:60]:
        print('  0x%04X  %s' % (off, s))
    if len(ss) > 60:
        print('  ... 还有 %d 条' % (len(ss) - 60))

    print()
    has_ns = buf.find(b'petns') >= 0
    has_ssid = buf.find(b'ssid') >= 0
    if has_ns and has_ssid:
        print('★ 结论：**有网络记忆** —— petns 命名空间和 ssid 键都在，'
              '设备会直接拿它去连 WiFi，不会进配网。')
    elif has_ns:
        print('★ 结论：有 petns 命名空间但没看到 ssid 键 —— 可能只存了食盆/状态，没存 WiFi。')
    else:
        print('★ 结论：**没有网络记忆** —— 看不到 petns 命名空间。'
              '那么"不进配网"的原因只能是：本次开机被判成了**深睡唤醒**（按设计跳过配网）。')
    return 0


if __name__ == '__main__':
    sys.exit(main())
