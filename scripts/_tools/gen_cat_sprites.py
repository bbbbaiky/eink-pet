#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_cat_sprites.py —— 从 200x200 RGBA 精灵图生成 common/cat_sprites.h

★2026-11-12（用户令）：「把原来的明信片合成姿势图全部取消，用新的这一批，原本的删掉」。
旧脚本（_upload/gen_cat_sprites.py）已随清理删除，这里重建一份，参数与原口径一致：

  · 输入：每张 200×200 RGBA（与明信片页同尺寸，猫已画在"地面"上）
  · 照片区 = 页面 x18..181 / y46..153（164×108），精灵只允许落在这个框里
  · 输出 mask（alpha>128）+ ink（alpha>128 且亮度<110，保住眼睛白点）
  · ox/oy = 相对照片区左上角的偏移；w/h/row 按位图打包

用法：
    python gen_cat_sprites.py <图片目录> <输出头文件>
默认读 C:\\epaper\\inbox 下的「明信片底图_600x600_可加工*.png」按序号排序。
"""
import sys, os, glob, re
from PIL import Image

PHOTO_X0, PHOTO_Y0, PHOTO_W, PHOTO_H = 18, 46, 164, 108
PHOTO_X1, PHOTO_Y1 = PHOTO_X0 + PHOTO_W - 1, PHOTO_Y0 + PHOTO_H - 1
ALPHA_MIN = 128
INK_MAX   = 110          # 亮度 < 110 判为墨

# 分类（对应 api_image.c 的 CAT_TIER，顺序与输入图片一一对应）
#   B=背影  F=全身  C=特写/半身
TIERS = ['F', 'F', 'F', 'F', 'B', 'B', 'F', 'C', 'F', 'F', 'F', 'C', 'C']


def collect(src, pattern='明信片底图*.png'):
    """按 1..N 的自然序收集文件（名字里的序号不可靠，但排序要稳定）。
    目录里混着别的 UI 图，用 pattern 只取精灵那一组。"""
    fs = glob.glob(os.path.join(src, pattern))
    def key(p):
        m = re.search(r'(\d+)\.png$', os.path.basename(p))
        return (int(m.group(1)) if m else 1, p)
    return sorted(fs, key=key)


def pack(bits_row):
    """bits_row: 该行 0/1 列表 → 每 8 位一字节，MSB first。"""
    out = []
    for i in range(0, len(bits_row), 8):
        b = 0
        for j, v in enumerate(bits_row[i:i + 8]):
            if v:
                b |= 0x80 >> j
        out.append(b)
    return out


def one(path):
    im = Image.open(path).convert('RGBA')
    if im.size != (200, 200):
        raise SystemExit('%s 尺寸不是 200x200，是 %s' % (path, im.size))
    px = im.load()
    # ① 求 alpha>128 的包围盒
    x0, y0, x1, y1 = 200, 200, -1, -1
    for y in range(200):
        for x in range(200):
            if px[x, y][3] > ALPHA_MIN:
                if x < x0: x0 = x
                if x > x1: x1 = x
                if y < y0: y0 = y
                if y > y1: y1 = y
    if x1 < x0:
        raise SystemExit('%s 全透明' % path)
    # ② 夹进照片区（超出的部分裁掉，不缩放不位移）
    cx0, cy0 = max(x0, PHOTO_X0), max(y0, PHOTO_Y0)
    cx1, cy1 = min(x1, PHOTO_X1), min(y1, PHOTO_Y1)
    if cx1 < cx0 or cy1 < cy0:
        raise SystemExit('%s 完全落在照片区外' % path)
    w, h = cx1 - cx0 + 1, cy1 - cy0 + 1
    ox, oy = cx0 - PHOTO_X0, cy0 - PHOTO_Y0
    # ③ 逐行打包
    mask, ink = [], []
    for y in range(cy0, cy1 + 1):
        mr, ir = [], []
        for x in range(cx0, cx1 + 1):
            r, g, b, a = px[x, y]
            on = a > ALPHA_MIN
            mr.append(1 if on else 0)
            lum = (r * 3 + g * 6 + b) // 10
            ir.append(1 if (on and lum < INK_MAX) else 0)
        mask += pack(mr)
        ink += pack(ir)
    clipped = (cx0 != x0 or cy0 != y0 or cx1 != x1 or cy1 != y1)
    return dict(path=path, ox=ox, oy=oy, w=w, h=h, row=(w + 7) // 8,
                mask=mask, ink=ink, clipped=clipped,
                srcbox=(x0, y0, x1, y1), box=(cx0, cy0, cx1, cy1))


def main():
    import pet_paths                  # ★R217：统一路径解析（不再写死 C:\epaper）
    src = sys.argv[1] if len(sys.argv) > 1 else str(pet_paths.inbox())
    dst = sys.argv[2] if len(sys.argv) > 2 else \
        str(pet_paths.REPO / 'common' / 'cat_sprites.h')
    files = collect(src)
    if not files:
        raise SystemExit('没找到 png：%s' % src)
    if len(TIERS) != len(files):
        raise SystemExit('TIERS 有 %d 项，但找到 %d 张图 —— 先对齐分类表' % (len(TIERS), len(files)))

    items = [one(f) for f in files]
    lines = []
    A = lines.append
    A('/* 自动生成，勿手改 —— 重跑 base_wired/scripts/_tools/gen_cat_sprites.py */')
    A('/* 猫精灵（%d 姿态）。坐标口径 = 明信片照片区 1:1：' % len(items))
    A('   页 200×200 的照片区 x18..181 / y46..153，正好 164×108 = photo buffer 尺寸，')
    A('   所以 (ox,oy) 是相对照片区左上角的偏移，不缩放不位移（用户 2026-11-09 明确要求）。')
    A('   ★2026-11-12（用户令）：整批换成新画的 13 张，旧的 5 张全部作废。')
    A('   mask = 不透明区（决定画不画）；ink = 深色（1=墨，0=纸，保住眼睛白点）。 */')
    A('#pragma once')
    A('#include <stdint.h>')
    A('#include <stddef.h>')
    A('')
    A('typedef struct {')
    A('    const uint8_t *mask;   /* 1-bit，行 (w+7)/8，MSB first */')
    A('    const uint8_t *ink;    /* 同上 */')
    A('    uint8_t ox, oy;        /* 在 164×108 photo buffer 里的左上角 */')
    A('    uint8_t w, h;          /* 尺寸 */')
    A('    uint8_t row;           /* 每行字节数 = (w+7)/8 */')
    A('} cat_sprite_t;')
    A('')
    for i, it in enumerate(items, 1):
        A('static const uint8_t CAT%d_MASK[] = {%s};' % (i, ','.join(str(v) for v in it['mask'])))
        A('static const uint8_t CAT%d_INK[] = {%s};' % (i, ','.join(str(v) for v in it['ink'])))
    A('')
    A('static const cat_sprite_t CAT_SPRITES[] = {')
    for i, it in enumerate(items, 1):
        A('    { CAT%d_MASK, CAT%d_INK, %d, %d, %d, %d, %d },' %
          (i, i, it['ox'], it['oy'], it['w'], it['h'], it['row']))
    A('};')
    A('#define N_CAT_SPRITES ((int)(sizeof(CAT_SPRITES) / sizeof(CAT_SPRITES[0])))')
    A('')
    open(dst, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))

    print('已生成 %s（%d 项）' % (dst, len(items)))
    for i, it in enumerate(items, 1):
        flag = '  ← 超出照片区已裁' if it['clipped'] else ''
        print('  CAT%-3d %-6s ox=%-3d oy=%-3d w=%-3d h=%-3d%s' %
              (i, TIERS[i - 1], it['ox'], it['oy'], it['w'], it['h'], flag))


if __name__ == '__main__':
    main()
