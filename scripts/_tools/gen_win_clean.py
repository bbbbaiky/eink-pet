# -*- coding: utf-8 -*-
"""把旧房间的窗景贴片清洗成「纯窗外风景」，另存 win_tech.bin 供科技房间专用

问题（用户报）：旧 win.bin 的裁片里烤进了**旧房间自己的窗框边线**和**窗下猫抓板**：
  · 左右各约 3 列是旧窗框的黑边 → 新房间窗孔形状不同，这圈边会露在窗外，像多了一层框；
  · 右下角 x54..86 / y63..83 是旧房间的猫抓板（白面板 + 黑横线 + 支架）→ 新房间没有，
    却从透明窗孔里透出来。
处理：左右各 3 列用邻列覆盖（保留白天/夜晚各自的明暗，不硬填白）；
      猫抓板区域填黑（该区域在旧图里本来就是「黑色地面 + 白家具」，填黑即恢复地面）。
★ 输出到**独立文件** win_tech.bin（资产区 WIN_TECH）—— 原 win.bin **一个字节都不动**，
  默认房间的窗外（含它自己的窗框/猫抓板）保持原样。
"""
import os
import shutil
import sys

import numpy as np

BASE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(BASE, "..", ".."))
WIN = os.path.join(ROOT, "fw_epaper_demo", "main", "assets", "win.bin")
OUT = os.path.join(ROOT, "fw_epaper_demo", "main", "assets", "win_tech.bin")
import pet_paths                      # ★R217：统一路径解析
ORIG = str(pet_paths.inbox() / "win_orig_backup.bin")   # ★R217：按仓库位置推导（有 else 兜底，不崩）

W, H, ROW = 90, 84, 12
BYTES = ROW * H
N = 56
BOARD = (45, 62, 90, 84)     # x0, y0, x1(不含), y1(不含) —— 只盖猫抓板那块（含左侧黑柱，接边无缝）
EDGE = 3                     # 左右各洗 3 列


def unpack(buf):
    return np.unpackbits(np.frombuffer(buf, dtype=np.uint8)).reshape(H, ROW * 8)[:, :W]


def pack(a):
    b = np.packbits(a.astype(np.uint8), axis=1, bitorder="big").tobytes()
    if len(b) != BYTES:
        raise SystemExit("打包长度 %d != %d" % (len(b), BYTES))
    return b


def main():
    src = ORIG if os.path.exists(ORIG) else WIN
    raw = open(src, "rb").read()
    if len(raw) != N * BYTES:
        raise SystemExit("%s 长度 %d != %d×%d" % (src, len(raw), N, BYTES))
    out = bytearray()
    changed = 0
    for i in range(N):
        a = unpack(raw[i * BYTES:(i + 1) * BYTES]).copy()
        before = a.copy()
        x0, y0, x1, y1 = BOARD
        # ★R194 用户五次订正：**整体构图一点不动**（拉伸/镜像都不行），只把猫抓板那块
        #   抹成「与周围一致的黑色地面」—— 区域含左侧黑立柱，右/下贴到贴片边缘，
        #   四周本来也是黑，因此看不出接缝、也不会像「挖了个坑」。
        a[y0:y1, x0:x1] = True
        for k in range(EDGE):                       # 左：用 x=EDGE 列覆盖
            a[:, k] = a[:, EDGE]
        for k in range(W - EDGE, W):                # 右：用 x=W-EDGE-1 列覆盖
            a[:, k] = a[:, W - EDGE - 1]
        if not np.array_equal(a, before):
            changed += 1
        out += pack(a)
    open(OUT, "wb").write(out)
    print(">>> %s 已生成：%d 片（原 win.bin 未改动）" % (OUT, N))
    return 0


if __name__ == "__main__":
    sys.exit(main())
