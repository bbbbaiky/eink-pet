#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""编译后体检「内部 DRAM 余量」（2026-10-22 加的纪律工具）。

背景：本工程内部 DRAM（dram0_0_seg，S3 上 334 KB）已长期贴顶 —— 历史上多次因为
「新加一个几百字节的常驻数组」直接链接失败（ld: section `.dram0.bss' will not fit）。
这个脚本解析 link map，把「已用 / 余量」打到构建输出里，余量 < 4 KB 时红字告警，
避免下次又是"改着改着突然 overflow"。

用法： python dram_headroom.py <build_dir>/pet_epaper_xxx.map [阈值字节，默认4096]
退出码恒为 0（只是告警，不拦构建）。
"""
import io
import re
import sys

REGION = "dram0_0_seg"
RED = "\033[31m"
YEL = "\033[33m"
RST = "\033[0m"


def main():
    if len(sys.argv) < 2:
        print("usage: dram_headroom.py <map> [warn_bytes]")
        return 0
    path = sys.argv[1]
    warn = int(sys.argv[2]) if len(sys.argv) > 2 else 4096

    try:
        s = io.open(path, encoding="latin-1", errors="replace").read()
    except OSError:
        print("   （跳过 DRAM 体检：读不到 map）")
        return 0

    m = re.search(r"Memory Configuration(.*?)Linker script and memory map", s, re.S)
    if not m:
        print("   （跳过 DRAM 体检：map 里没有 Memory Configuration）")
        return 0

    base = size = None
    for ln in m.group(1).splitlines():
        if REGION in ln:
            parts = ln.split()      # 形如： dram0_0_seg  0x3fc88000  0x00053700  rw
            base = int(parts[1], 16)
            size = int(parts[2], 16)
    if base is None or size is None:
        print("   （跳过 DRAM 体检：找不到 %s 区域定义）" % REGION)
        return 0

    # 统计所有落在该区域内的段（行首带点的才是"段头"，对象级明细是缩进的）
    used = 0
    for mm in re.finditer(r"^(\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)", s, re.M):
        addr = int(mm.group(2), 16)
        sz = int(mm.group(3), 16)
        if base <= addr < base + size and sz < size:
            used += sz

    free = size - used
    pct = (free * 100.0 / size) if size else 0.0
    line = (">>> 内部 DRAM：已用 %.1f KB / %.1f KB，余 %.1f KB（%.1f%%）"
            % (used / 1024.0, size / 1024.0, free / 1024.0, pct))
    if free < warn:
        print(RED + line + RST)
        print(RED + "    ⚠ 余量 < %.0f KB：再加常驻数组就会链接失败"
                    "（ld: section `.dram0.bss' will not fit）。" % (warn / 1024.0) + RST)
        print(RED + "      处理办法：新缓冲改用 psram_u8()（PSRAM）或改成栈上临时缓冲。"
                    + RST)
    elif free < warn * 3:
        print(YEL + line + RST)
    else:
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
