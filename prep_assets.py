# -*- coding: utf-8 -*-
"""把分层资产打成 ESP32 固件能直接吃的位图（2026-09-16 新方案，48 图 / 无状态条）。

数据格式照搬 xiaozhi-esp32 在这块板子上的驱动
(main/boards/waveshare/esp32-s3-epaper-1.54/custom_lcd_display.cc)：
    buffer[y * 25 + (x >> 3)]，bit = 7-(x&7)
    bit = 1 → 白，bit = 0 → 黑
行主序 · 每行 25 字节 · MSB 在前。整屏 = 200*200/8 = 5000 字节。

★ 五层（**没有 strip.bin 了** —— 用户要求顶部规格条去掉）：
    room.bin   房间底图 × 4（按时段序 清晨/正午/傍晚/深夜）   4 × 5000 = 20000 字节
    win.bin    窗贴片 × 48（按 idx 序）                       48 × ((90+7)/8*84) = 48384 字节
    moon.bin   ★ 月相贴片 × 8（新增 2026-09-16）               8 × ((27+7)/8*27) = 864 字节
    cat.bin    猫精灵（原样拷贝；u32 数量 + u32 偏移表 + 每精灵 u16 x0,y0,w,h + S/W/SH）
    off.bin    关机提示页 5000 字节
  位约定：room/win 与屏缓冲一致（1=白）；
          ★ moon.bin 是 **1 = 亮面（贴白）**，与 win.bin 的「1=黑墨」**故意相反**；
          cat.bin 三层是「语义位」（S:1=贴黑，W:1=贴白，SH:1=贴黑），
          都由 build_统一出图.py 打好，这里原样拷。

★★ moon.bin 为什么单列一层（而不是打进 win 贴片）：
   打进 win 贴片 → win.bin 从 48 张变 48×8 = 384 张（193KB），
   且每换一次月相就要重刷整屏 1.8s —— 墨水屏上体验极差。
   单列一层 → 只 864 字节，换月相理论上只刷月亮那一小块（27×27）。
   ★ 前提：`room.bin` 的**深夜**那张必须是「已抠掉月亮」的净底图，
     否则月相层叠上去会与原底图的固定月牙重影。
     `room_from_src()` 与 `build_统一出图.base_room()` 必须用同一张图。

房间底图从哪来：build_统一出图.py 只出整屏成品，**不出** room_*.png。
  这里按「哪个时段」从整屏成品里反推会引入猫，所以房间底图必须**单独生成**：
  见 `room_from_src()` —— 直接用 src/rooms/room_<时段>_200.png + 窗内美术拼。

图源 = out/全量/（build_统一出图.py 的产物 + 清单 + win.bin）
"""
import hashlib
import json
import os
import shutil
import sys

import numpy as np
from PIL import Image

BASE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(BASE)
sys.path.insert(0, PROJ)
ASSETS = os.path.join(BASE, 'fw_epaper_demo', 'main', 'assets')
# ★R216：旧默认值指向已删的 C:/epaper/_preview/...；manifest 属一次性出图产物，须显式提供。
# ★R217：不再写死 C:/epaper —— 按仓库相对位置推导（可用 PET_INBOX / MANIFEST 覆盖）
_REPO = os.path.dirname(os.path.abspath(__file__))
_EPAPER = os.path.abspath(os.path.join(_REPO, '..', '..'))
MANIFEST = os.environ.get('MANIFEST', os.path.join(_EPAPER, 'inbox', 'manifest.json'))
SRCDIR = os.path.dirname(MANIFEST)
OFF_PNG = os.path.join(PROJ, 'out', '关机页.png')
BIN_BYTES = 5000
PERIODS = ['清晨', '正午', '傍晚', '深夜']

WIN_X0, WIN_Y0, WIN_W, WIN_H = 40, 32, 90, 84
WIN_BYTES = ((WIN_W + 7) // 8) * WIN_H

# ★★★ 月相层几何（必须与 build_统一出图.py / main.c 一致）
MOON_X0, MOON_Y0, MOON_SZ = 64, 40, 27
MOON_BYTES = ((MOON_SZ + 7) // 8) * MOON_SZ        # = 108
N_PHASE = 8


def pack_full(arr):
    """bool(True=墨) → 5000B 位图（1=白）。"""
    a = (~arr).astype(np.uint8)                   # 1 = 白
    b = np.packbits(a, axis=1, bitorder='big').tobytes()
    if len(b) != BIN_BYTES:
        raise SystemExit('打包后长度 %d，应为 %d' % (len(b), BIN_BYTES))
    return b


def pack_full_png(path):
    im = Image.open(path).convert('L')
    if im.size != (200, 200):
        raise SystemExit('%s 尺寸不是 200x200，实际 %s' % (path, im.size))
    return pack_full(np.asarray(im) < 128)


def room_from_src(per):
    """按时段拼「房间底图」（无猫、无天气、**无地标**、深夜**无月亮**）。

    ★ 必须与 build_统一出图.py 的 base_room() 逐像素一致。
      窗外那块（山 / 月亮 / 夜山白点 / 天气）全部由 win 贴片负责，
      所以这里只取「用户画的房间原图」，一个像素都不动 ——
      否则「深夜」要存 fuji 版 + pearl 版两个房间，room.bin 装不下。

    ★★★ 深夜用 **净底图**（月亮已抠掉）—— 月相改由 moon.bin 单列一层叠加。
    这一条必须与 `build_统一出图.base_room()` 完全一致，否则固件重组会出现
    「月相层底下压着一颗旧月牙」的重影。两张图同源：`_preview/build_moon.py` 生成。
    """
    from trial_窗外 import load
    if per == '深夜':
        p = os.path.join(PROJ, 'src', 'rooms', 'room_深夜_200_clean.png')
        if not os.path.exists(p):
            raise SystemExit('缺净夜底图 %s\n先跑： python _preview/build_moon.py' % p)
        return load(p).copy()
    p = os.path.join(PROJ, 'src', 'rooms', 'room_%s_200.png' % per)
    if not os.path.exists(p):
        raise SystemExit('缺房间底图 %s' % p)
    return load(p).copy()


def main():
    if not os.path.exists(MANIFEST):
        raise SystemExit('找不到清单 %s\n先跑： python build_统一出图.py' % MANIFEST)
    with open(MANIFEST, encoding='utf-8') as f:
        items = json.load(f)
    items.sort(key=lambda d: d['idx'])
    for i, d in enumerate(items):
        if d['idx'] != i:
            raise SystemExit('清单图号不连续：第 %d 项的 idx=%s' % (i, d['idx']))
    n_img = len(items)
    os.makedirs(ASSETS, exist_ok=True)

    # ---- room.bin：4 张房间底图 ----
    blob = bytearray()
    for per in PERIODS:
        b = pack_full(room_from_src(per))
        blob += b
        print('room  %-4s 黑像素 %6.2f%%  MD5 %s'
              % (per, 100 * (1 - (np.unpackbits(
                  np.frombuffer(b, dtype=np.uint8)).reshape(200, 200)).mean()),
                 hashlib.md5(b).hexdigest()[:8]))
    with open(os.path.join(ASSETS, 'room.bin'), 'wb') as f:
        f.write(blob)
    print('room.bin  4 × %d = %d 字节' % (BIN_BYTES, len(blob)))

    # ---- win.bin：由 build_统一出图.py 产出，原样拷 ----
    src = os.path.join(SRCDIR, 'win.bin')
    if not os.path.exists(src):
        raise SystemExit('缺 %s（build_统一出图.py 主进程产物）' % src)
    if os.path.getsize(src) != n_img * WIN_BYTES:
        raise SystemExit('win.bin 长度 %d != %d × %d'
                         % (os.path.getsize(src), n_img, WIN_BYTES))
    shutil.copyfile(src, os.path.join(ASSETS, 'win.bin'))
    print('win.bin   %d 片 × %d = %d 字节  MD5 %s'
          % (n_img, WIN_BYTES, os.path.getsize(src),
             hashlib.md5(open(src, 'rb').read()).hexdigest()[:8]))

    # ---- ★★★ moon.bin：月相贴片 × 8（2026-09-16 新增）----
    src = os.path.join(SRCDIR, 'moon.bin')
    if not os.path.exists(src):
        raise SystemExit('缺 %s（build_统一出图.py 主进程产物）' % src)
    if os.path.getsize(src) != N_PHASE * MOON_BYTES:
        raise SystemExit('moon.bin 长度 %d != %d × %d'
                         % (os.path.getsize(src), N_PHASE, MOON_BYTES))
    shutil.copyfile(src, os.path.join(ASSETS, 'moon.bin'))
    print('moon.bin  %d 档 × %d = %d 字节（%dx%d @%d,%d）MD5 %s'
          % (N_PHASE, MOON_BYTES, os.path.getsize(src), MOON_SZ, MOON_SZ,
             MOON_X0, MOON_Y0,
             hashlib.md5(open(src, 'rb').read()).hexdigest()[:8]))

    # ---- cat.bin：原样拷 ----
    src = os.path.join(SRCDIR, 'cat.bin')
    if not os.path.exists(src):
        raise SystemExit('缺 %s' % src)
    shutil.copyfile(src, os.path.join(ASSETS, 'cat.bin'))
    print('cat.bin   %d 字节  MD5 %s' % (os.path.getsize(src),
                                         hashlib.md5(open(src, 'rb').read()).hexdigest()[:8]))

    # ---- off.bin：关机页 ----
    if not os.path.exists(OFF_PNG):
        raise SystemExit('缺 %s（先跑 OFF_FONT=fp8 python make_off_page.py）' % OFF_PNG)
    ob = pack_full_png(OFF_PNG)
    with open(os.path.join(ASSETS, 'off.bin'), 'wb') as f:
        f.write(ob)
    black = int((np.asarray(Image.open(OFF_PNG).convert('L')) < 128).sum())
    print('off.bin   1 张 · 黑像素 %d（%.2f%%）' % (black, 100.0 * black / 40000))

    # ---- 清掉上一版残留（已废弃：strip 状态条 / 整屏 imgs）----
    for q in [os.path.join(ASSETS, 'strip.bin'),
              os.path.join(ASSETS, 'imgs.bin')] + \
             [os.path.join(ASSETS, 'img%d.bin' % i) for i in range(8)]:
        if os.path.exists(q):
            os.remove(q)
            print('  清理旧素材 %s' % os.path.basename(q))

    print('\n共 %d 张。下一步： python hw/verify_assets.py   （往返比对，必须全部 0 像素）'
          % n_img)


if __name__ == '__main__':
    main()
