# -*- coding: utf-8 -*-
"""分层资产往返校验（2026-09-16 新方案：48 图 · 无状态条）。

★ 按**固件的合成路径**重组整屏：
    room[时段] → 盖窗贴片[win_idx]【整块替换 90×84】→ 影(SH) → 猫黑(S) → 猫白(W)
  图号 → (精灵, 地标, 时段, 天气) 走 names.h 的 IMG_MAP（与固件同一来源）。
  参照 = build_统一出图.py 交付的整屏成品（48 张），重组结果必须**逐像素 0 差异**。

位约定：room/win = 1 白 0 黑（屏缓冲同款）；cat.bin 三层 = 语义位
（S:1 黑 / W:1 白 / SH:1 黑），每精灵头 u16 LE x0,y0,w,h；
cat.bin 总头 = u32 数量 + u32 偏移表[×N]。

⛔ 已废弃：strip.bin（顶部规格条，用户要求去掉）。
"""
import json
import os
import re
import sys

import numpy as np
from PIL import Image

BASE = os.path.dirname(os.path.abspath(__file__))
# ★ 月相专项验证要 import build_统一出图（项目根的脚本），把根加进路径。
#   ⚠️ 忘了这行会报 ModuleNotFoundError —— 这里踩过一次。
sys.path.insert(0, os.path.dirname(BASE))
ASSETS = os.path.join(BASE, 'fw_epaper_demo', 'main', 'assets')
# ★R216：旧默认值指向已删的 C:/epaper/_preview/...；这两项须显式提供。
import tempfile
# ★R217：不再写死 C:/epaper；预览写到系统临时目录，不污染素材投放区
_REPO = os.path.dirname(os.path.abspath(__file__))
_EPAPER = os.path.abspath(os.path.join(_REPO, '..', '..'))
MANIFEST = os.environ.get('MANIFEST', os.path.join(_EPAPER, 'inbox', 'manifest.json'))
SRCDIR = os.path.dirname(MANIFEST)
PREVIEW = os.environ.get('PREVIEW', os.path.join(tempfile.gettempdir(), 'verify_v23'))
BIN_BYTES = 5000


def unpack_full(buf):
    a = np.unpackbits(np.frombuffer(buf, dtype=np.uint8), axis=0) \
        .reshape(200, 25 * 8)[:, :200]
    return a.astype(bool)                      # True = 白


def unpack_bits(data, h, w):
    """行序 MSB 位图 → bool（True = 1）。行宽补齐到字节。"""
    rowb = (w + 7) // 8
    if len(data) != rowb * h:
        raise SystemExit('位图长度 %d != %d' % (len(data), rowb * h))
    a = np.unpackbits(np.frombuffer(data, dtype=np.uint8), axis=0) \
        .reshape(h, rowb * 8)[:, :w]
    return a.astype(bool)


def read_names_h():
    """从 names.h 抠常量 + IMG_MAP（与固件同一来源）。"""
    txt = open(os.path.join(ASSETS, 'names.h'), encoding='utf-8').read()
    vals = {}
    for line in txt.splitlines():
        line = line.strip()
        if line.startswith('#define'):
            parts = line.split()
            if len(parts) >= 3 and parts[1] in (
                    'N_LAND', 'N_PERIOD', 'N_WEATHER', 'N_POSE', 'N_IMG',
                    'WIN_X0', 'WIN_Y0', 'WIN_W', 'WIN_H', 'WIN_ROW', 'WIN_BYTES',
                    'MOON_X0', 'MOON_Y0', 'MOON_SZ', 'MOON_ROW', 'MOON_BYTES',
                    'N_PHASE'):
                try:
                    vals[parts[1]] = eval(' '.join(parts[2:]).replace('/', '//'),
                                          {}, vals)
                except Exception:
                    pass
    mm = re.search(r'IMG_MAP\[N_IMG\]\s*=\s*\{(.*?)\};', txt, re.S)
    assert mm, 'names.h 里没找到 IMG_MAP'
    entries = []
    for m in re.finditer(r'\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}', mm.group(1)):
        entries.append(tuple(int(g) for g in m.groups()))
    vals['IMG_MAP'] = entries
    return vals


def read_cat_sprites(cat_raw):
    """cat.bin → [精灵字典(x0,y0,w,h,S,W,SH), ...]"""
    (n,) = np.frombuffer(cat_raw[:4], dtype='<u4')
    offs = np.frombuffer(cat_raw[4:4 + 4 * n], dtype='<u4')
    sprites = []
    for o in offs:
        x0, y0, w, h = np.frombuffer(cat_raw[o:o + 8], dtype='<u2')
        body = cat_raw[o + 8:]
        lay = (w + 7) // 8 * h
        assert len(body) >= 3 * lay, (len(body), 3 * lay)
        sprites.append(dict(x0=int(x0), y0=int(y0), w=int(w), h=int(h),
                            S=unpack_bits(body[0:lay], h, w),
                            W=unpack_bits(body[lay:2 * lay], h, w),
                            SH=unpack_bits(body[2 * lay:3 * lay], h, w)))
    return sprites


def main():
    nv = read_names_h()
    print('names.h:', {k: nv[k] for k in sorted(nv) if k != 'IMG_MAP'})
    n_img = nv['N_IMG']
    img_map = nv['IMG_MAP']
    assert len(img_map) == n_img, (len(img_map), n_img)
    gx, gy, gw, gh = nv['WIN_X0'], nv['WIN_Y0'], nv['WIN_W'], nv['WIN_H']
    win_bytes = nv['WIN_BYTES']

    with open(MANIFEST, encoding='utf-8') as f:
        items = sorted(json.load(f), key=lambda d: d['idx'])
    assert len(items) == n_img, (len(items), n_img)
    assert not os.path.exists(os.path.join(ASSETS, 'strip.bin')), \
        'strip.bin 还在（顶部状态条已废弃，应删）'

    room_raw = open(os.path.join(ASSETS, 'room.bin'), 'rb').read()
    win_raw = open(os.path.join(ASSETS, 'win.bin'), 'rb').read()
    moon_raw = open(os.path.join(ASSETS, 'moon.bin'), 'rb').read()
    cat_raw = open(os.path.join(ASSETS, 'cat.bin'), 'rb').read()
    off_raw = open(os.path.join(ASSETS, 'off.bin'), 'rb').read()
    assert len(room_raw) == 4 * BIN_BYTES, len(room_raw)
    assert len(win_raw) == n_img * win_bytes, (len(win_raw), n_img * win_bytes)
    assert len(off_raw) in (BIN_BYTES, 2 * BIN_BYTES), len(off_raw)   # ★R205：关机页两页轮替
    # ★ 月相层（2026-09-16 新增）：8 档 × 27×27 小贴片
    mx, my, ms = nv['MOON_X0'], nv['MOON_Y0'], nv['MOON_SZ']
    moon_bytes = nv['MOON_BYTES']
    n_phase = nv['N_PHASE']
    assert len(moon_raw) == n_phase * moon_bytes, (len(moon_raw), n_phase * moon_bytes)
    sprites = read_cat_sprites(cat_raw)
    print('cat.bin %d 精灵；0=默认 %dx%d@(%d,%d)'
          % (len(sprites), sprites[0]['w'], sprites[0]['h'],
             sprites[0]['x0'], sprites[0]['y0']))
    assert len(sprites) == nv['N_POSE'], (len(sprites), nv['N_POSE'])
    print('moon.bin %d 档 × %d 字节（%dx%d @%d,%d）'
          % (n_phase, moon_bytes, ms, ms, mx, my))

    rooms = [unpack_full(room_raw[i * BIN_BYTES:(i + 1) * BIN_BYTES])
             for i in range(4)]
    os.makedirs(PREVIEW, exist_ok=True)
    bad = 0
    worst = 0
    print('%-4s %-26s %8s  %s' % ('图号', '源文件', '差像素', '判定'))
    for i in range(n_img):
        pose, land, period, weather = img_map[i]
        win_idx = (land * nv['N_PERIOD'] + period) * nv['N_WEATHER'] + weather
        fb = rooms[period].copy()
        # 窗贴片：1 = 黑墨；固件是整块替换
        pb = unpack_bits(win_raw[win_idx * win_bytes:(win_idx + 1) * win_bytes],
                         gh, gw)
        sub = fb[gy:gy + gh, gx:gx + gw]
        sub[pb] = False
        sub[~pb] = True
        # 猫精灵：影子黑 → S 黑 → W 白（compose 同序：先影再黑后白）
        sp = sprites[pose]
        region = fb[sp['y0']:sp['y0'] + sp['h'], sp['x0']:sp['x0'] + sp['w']]
        region[sp['SH']] = False
        region[sp['S']] = False
        region[sp['W']] = True

        src = os.path.join(SRCDIR, items[i]['file'])
        ref = np.asarray(Image.open(src).convert('L')) >= 128   # True = 白
        diff = int((fb ^ ref).sum())
        worst = max(worst, diff)
        ok = diff == 0
        bad += 0 if ok else 1
        if i < 3 or not ok or i == n_img - 1:
            print('%-4d %-26s %8d  %s' % (i, os.path.basename(src)[:26], diff,
                                          'OK' if ok else '✗'))
        Image.fromarray(np.where(fb, 255, 0).astype(np.uint8)).save(
            os.path.join(PREVIEW, '%02d.png' % i))

    print('\n关机页 %d 字节 · 图 %d 张 · 最大差异 %d px · 差异非 0：%d 张'
          % (len(off_raw), n_img, worst, bad))

    # ---- ★★★ 月相层专项验证（2026-09-16 新增）------------------------------
    # 三条检查，缺一不可：
    #   ① 8 档形状**单调**：新月 0px → 满月最大 → 残月回小（waxing 段递增）
    #   ② 位约定是 **1 = 亮面**（不是 1 = 黑墨）—— 这条写反月亮会变黑圆盘
    #   ③ **换月相只影响月亮框内**，框外 0 差异 —— 这是「单列一层」的意义所在
    import build_统一出图 as B  # noqa: E402
    moon_masks = []
    for i in range(n_phase):
        m = unpack_bits(moon_raw[i * moon_bytes:(i + 1) * moon_bytes], ms, ms)
        moon_masks.append(m)
    counts = [int(m.sum()) for m in moon_masks]
    print('月相亮面像素：%s' % ' '.join('%d' % c for c in counts))
    assert counts[0] == 0, '新月应为 0px，实际 %d' % counts[0]
    for k in range(1, 5):
        assert counts[k] >= counts[k - 1], \
            'waxing 段必须单调递增：%d → %d' % (counts[k - 1], counts[k])
    for k in range(5, 8):
        assert counts[k] <= counts[k - 1], \
            'waning 段必须单调递减：%d → %d' % (counts[k - 1], counts[k])
    # 与 build 脚本算的期望形状逐像素比对（防止打包/位序出错）
    for i in range(n_phase):
        exp = B.moon_shape(i / float(n_phase))[my:my + ms, mx:mx + ms]
        assert int((exp ^ moon_masks[i]).sum()) == 0, \
            '月相 %d 与 build 期望不一致（打包或位序错了）' % i
    print('月相层：位序/形状与 build 脚本逐像素一致 ✓')
    # ③ 换月相只影响月亮框内
    moon_zone = np.zeros((200, 200), bool)
    moon_zone[my:my + ms, mx:mx + ms] = True
    base0 = rooms[3].copy()
    base4 = rooms[3].copy()
    s0 = base0[my:my + ms, mx:mx + ms]; s0[moon_masks[0]] = True
    base0[my:my + ms, mx:mx + ms] = s0
    s4 = base4[my:my + ms, mx:mx + ms]; s4[moon_masks[4]] = True
    base4[my:my + ms, mx:mx + ms] = s4
    out_zone = int(((base0 ^ base4) & ~moon_zone).sum())
    assert out_zone == 0, '★ 换月相影响到了月亮框外 %d px' % out_zone
    print('换月相（新月↔满月）：框外差异 %d px ✓' % out_zone)

    print('-> 重组预览 %s' % PREVIEW)
    if bad:
        raise SystemExit(1)
    print('全部 0 像素差异 ✓')


if __name__ == '__main__':
    main()
