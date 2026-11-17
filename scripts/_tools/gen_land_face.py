# -*- coding: utf-8 -*-
"""窗外分层素材生成：**通用天气层（底） + 各地标风景层（中）**

用户定稿的分层（2026-11-11 / 2026-11-12 复用化）：
  上层：猫 / 食物
  中层：房间图 —— 只有窗户那块 alpha=0（挖空）
  下层：风景图（富士山 / 东方明珠 / 星球）—— 只有中间一条有内容，其余透明
  底层：天气图 —— **整屏通用**（weather_field.py），从风景透明处露出来

★复用（用户 2026-11-12 要求）：
  · 天气层与地标 / 房间 / 窗洞**完全无关**，整屏生成 ⇒
    窗洞挪位置、改大小、换房间、换风景都不用重画天气；
  · 新增地标只需要画「风景图 + alpha 掩膜」（build_land_base），天气自动复用；
  · 所以天气资产是 4 时段 × 7 天气 = 28 张，不是每个地标一套。

资产：
  land_weather.bin = 28 × 5000（4 时段 × 7 天气，整屏；1=白）
  land_base.bin    = 12 × 5000（3 地标 × 4 时段风景）+ 3 × 5000 不透明掩膜（1=画）
  room_plain.bin   = 4 × 5000 默认房间 + 5000 洞掩膜（alpha，来自用户图）
"""
import os
import sys

import numpy as np
from PIL import Image

import weather_field
from weather_field import N_PERIOD, N_WEATHER as N_WX

BASE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(BASE, "..", ".."))
import pet_paths                      # ★R217：统一路径解析（不再写死 C:/epaper）
INBOX = str(pet_paths.inbox())
ASSETS = os.path.join(ROOT, "fw_epaper_demo", "main", "assets")
OLD_ROOM = os.path.join(ASSETS, "room.bin")

SZ = 200
BIN = 5000
PERIODS = ["清晨", "正午", "傍晚", "深夜"]
LANDS = [("富士山", 0), ("东方明珠", 1), ("星球", 2)]
HOLE = (39, 32, 130, 116)
# 窗洞矩形 —— 只给天气层当"渐变锚点"（雾上疏下密 / 闪电位置），图案本身整屏无缝。
# 窗洞以后要挪/改大小，只改这一行，不用重画任何天气。
WIN_FOCUS = (40, 32, 130, 116)


def pack(a01):
    b = np.packbits(a01.astype(np.uint8), axis=1, bitorder="big").tobytes()
    if len(b) != BIN:
        raise SystemExit("打包 %d != %d" % (len(b), BIN))
    return b


def load_face(name):
    """新风景图 → (墨, 不透明掩膜)。位置完全按原图，不做任何位移（用户令）。"""
    a = np.asarray(Image.open(os.path.join(INBOX, name + ".png")).convert("RGBA"))
    ink = (a[:, :, 3] > 0) & (a[:, :, :3].mean(axis=2) < 128)
    alpha = a[:, :, 3] > 0
    return ink, alpha


def load_room_png(name):
    a = np.asarray(Image.open(os.path.join(INBOX, name + ".png")).convert("RGBA"))
    ink = (a[:, :, 3] > 0) & (a[:, :, :3].mean(axis=2) < 128)
    return ink, a[:, :, 3] > 0


def build_room_plain():
    old = open(OLD_ROOM, "rb").read()
    out = bytearray()
    hole = None
    for i, per in enumerate(PERIODS):
        if per == "正午":
            ink, draw = load_room_png("普通-正午")
            hole = draw
        elif per == "深夜":
            ink, _ = load_room_png("普通-深夜")
        else:
            d = np.frombuffer(old[i * BIN:(i + 1) * BIN], dtype=np.uint8)
            ink = ~np.unpackbits(d).reshape(SZ, SZ).astype(bool)
        out += pack(~ink)
    out += pack(hole)
    open(os.path.join(ASSETS, "room_plain.bin"), "wb").write(out)
    print(">>> room_plain.bin（4×%d + 洞掩膜）" % BIN)


def fs_dither(gray, mask):
    """Floyd–Steinberg 误差扩散：把 0..1「白点概率」二值化，点均匀分散、无 bayer 行条纹。"""
    h, w = gray.shape
    g = np.where(mask, gray, 0.0).astype(np.float64).copy()
    out = np.zeros((h, w), bool)
    for y in range(h):
        for x in range(w):
            if not mask[y, x]:
                continue
            v = g[y, x]
            wb = v >= 0.5
            out[y, x] = wb
            err = v - (1.0 if wb else 0.0)
            if x + 1 < w and mask[y, x + 1]: g[y, x + 1] += err * 7.0 / 16.0
            if y + 1 < h:
                if x > 0 and mask[y + 1, x - 1]: g[y + 1, x - 1] += err * 3.0 / 16.0
                if mask[y + 1, x]: g[y + 1, x] += err * 5.0 / 16.0
                if x + 1 < w and mask[y + 1, x + 1]: g[y + 1, x + 1] += err * 1.0 / 16.0
    return out


def face_variant(ink, alpha, period, planet=False):
    """风景变体：傍晚亮部压网点；**只有深夜**把风景网点化（雪天等其它天气保持正常山体）。"""
    out = ink.copy()
    yy, xx = np.mgrid[0:SZ, 0:SZ]
    hole = np.zeros((SZ, SZ), bool)
    hole[HOLE[1]:HOLE[3], HOLE[0]:HOLE[2]] = True
    if period == "深夜":
        # ★R194 用户令：深夜山体是**白点压黑**，并且**从密到疏**渐变：
        #   山脊/雪顶一带白点最密（80%），往下越来越疏（15%），山脚融进夜色。
        bayer = np.array([
            [0, 32,  8, 40,  2, 34, 10, 42],
            [48, 16, 56, 24, 50, 18, 58, 26],
            [12, 44,  4, 36, 14, 46,  6, 38],
            [60, 28, 52, 20, 62, 30, 54, 22],
            [3, 35, 11, 43,  1, 33,  9, 41],
            [51, 19, 59, 27, 49, 17, 57, 25],
            [15, 47,  7, 39, 13, 45,  5, 37],
            [63, 31, 55, 23, 61, 29, 53, 21],
        ]) / 64.0
        if planet:
            # ★R195（星球）：竖直渐变会让球体的左半边缘读成"山坡"（用户报「还是带着山」）。
            #   星球改为**保留自身结构**：环缝/亮带是实白，球体本身只压 25% 白点 ——
            #   球体偏暗、亮带和环最跳，夜里才像"暗球 + 亮环"，不像一座山丘。
            body = alpha & hole & ink
            white = (alpha & hole & ~ink) | (body & (bayer[yy % 8, xx % 8] < 0.25))
            out = alpha & ~white
        else:
            # 渐变基准跟着这张图自己的最高点走（富士山山脊 / 东方明珠塔尖）
            rows = np.where(alpha.any(axis=1))[0]
            y_top = float(rows.min()) if len(rows) else float(HOLE[1])
            # ★2026-11-13 用户令（第5轮）：暗的部分再往上移 + 整体再压暗一档。
            t = np.clip((yy - y_top) / max(1.0, HOLE[3] - y_top), 0.0, 1.0) ** 0.55
            base = 0.66 - 0.62 * t
            top_zone = np.clip((yy - y_top) / 25.0, 0.0, 1.0)
            prob_white = base * (0.18 + 0.82 * top_zone)
            white = alpha & hole & (bayer[yy % 8, xx % 8] < prob_white)
            # ★2026-11-13 用户令（第3轮）：坚持 bayer 规则网点（不要误差扩散）；但细塔
            #   （东方明珠）某行落在 bayer 高阈值行会整段黑/虚线 → 逐行保底至少 1 个白点
            #   （取 bayer 阈值最小的位置，仍是规则点阵，不整段黑）。
            _rowmask = alpha & hole
            for _y in range(SZ):
                _r = _rowmask[_y]
                if not _r.any():
                    continue
                if not (white[_y] & _r).any():
                    _xs = np.where(_r)[0]
                    _bx = _xs[int(np.argmin(bayer[_y % 8, _xs % 8]))]
                    white[_y, _bx] = True
            out = alpha & ~white          # 山体底色是墨，白点按渐变挖出来
    elif period == "傍晚":
        out |= (~ink) & alpha & hole & ((xx % 2) == 0) & ((yy % 2) == 0)
    return out


def assert_hole_matches_room():
    """★R217：HOLE 是硬编码窗洞矩形，房间底图的掩膜帧才是真源。
    两者不一致时地标会露到墙外 / 被裁出黑带 —— 生成前直接拦下，别等上屏才发现。"""
    p = os.path.join(ASSETS, "room_plain.bin")
    d = open(p, "rb").read()
    if len(d) < 5 * BIN:
        raise SystemExit("HOLE 校验：%s 不足 5 帧（%d B）" % (p, len(d)))
    mask = np.unpackbits(np.frombuffer(d[4 * BIN:5 * BIN], dtype=np.uint8)).reshape(SZ, SZ)
    ys, xs = np.where(mask == 0)
    if len(xs) == 0:
        raise SystemExit("HOLE 校验：%s 掩膜里没有窗洞" % p)
    rect = (int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1)
    if rect != HOLE:
        raise SystemExit("HOLE 与房间窗洞掩膜不一致：代码 %s / 掩膜 %s —— 改窗洞必须同步改 HOLE"
                         % (HOLE, rect))
    print("  HOLE 校验通过：%s == 房间掩膜窗洞" % (HOLE,))


def build_land_base():
    assert_hole_matches_room()
    """12 张：3 地标 × 4 时段风景（雪天/其它天气都用正常山体），+ 3 张掩膜。"""
    out = bytearray()
    masks = []
    for name, land in LANDS:
        ink, alpha = load_face(name)
        masks.append(alpha)
        for per in PERIODS:
            out += pack(~face_variant(ink, alpha, per, planet=(name == "星球")))
        print("  land_base %s：4 时段风景" % name)
    for m in masks:
        out += pack(m)
    open(os.path.join(ASSETS, "land_base.bin"), "wb").write(out)
    print(">>> land_base.bin（%d×%d + %d×掩膜）" % (len(LANDS) * 4, BIN, len(LANDS)))


def build_land_weather():
    """★2026-11-12 用户定稿：天气层抽成**通用图层**（weather_field.py），整屏生成。

    · 与地标 / 房间 / 窗洞全部无关 ⇒ 窗洞挪位置、改大小、换风景都不用重画天气；
    · 资产从「3 地标 × 4 × 7 = 84 张」降到「4 时段 × 7 天气 = 28 张」，新增地标零成本；
    · 4 时段（清晨/正午/傍晚/深夜）× 7 天气（晴/小雨/雨/大雨/雾/雪/雷）。"""
    out = bytearray()
    for pi in range(N_PERIOD):
        for wi in range(N_WX):
            out += pack(~weather_field.render(pi, wi, SZ, SZ, focus=WIN_FOCUS))
        print("  land_weather %s：%d 天气" % (PERIODS[pi], N_WX))
    open(os.path.join(ASSETS, "land_weather.bin"), "wb").write(out)
    print(">>> land_weather.bin（%d×%d）" % (N_PERIOD * N_WX, BIN))


def main():
    build_room_plain()
    build_land_base()
    build_land_weather()
    return 0


if __name__ == "__main__":
    sys.exit(main())
