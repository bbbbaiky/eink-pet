# -*- coding: utf-8 -*-
"""通用「窗外天气层」生成器 —— 与地标 / 房间 / 窗洞**完全无关**。

设计（2026-11-12 用户定稿的分层复用）：
  · 这一层是**整屏铺满**的（默认 200×200），风景层（land_base）只是盖在它上面；
  · 所以窗洞挪位置、改大小、换房间、换风景，都**不用重新生成天气**；
  · 新增地标时只画风景 + 掩膜，天气层直接复用（4 时段 × 7 天气 = 28 张，不再是每地标 28 张）。
  · 只有「雾的渐变方向」和「闪电位置」需要一个窗洞锚点 focus（一个矩形），
    图案本身是整屏无缝的，挪窗洞只改 focus 一处。

几何全部照旧素材实测，不靠"补洞"：
  雨   1px、45°（dx/dy=+1，向右下）分段斜线；小雨 4.6% / 中雨 8.4% / 大雨 13.8%
  雾   4px 点阵，**上疏下密**（9.4% → 12.5% → 25%，与东方明珠那套一致）
  雪   7×7「米」字（旧素材字形），约 13px 网格 + 抖动，约 12%
  雷   中雨 + 一道折线闪电
时段：
  清晨/正午 白天空；傍晚 斜向点阵（约 34%，旧素材实测）；深夜 黑天空 + 星点
"""
import numpy as np
from PIL import Image, ImageDraw

N_PERIOD = 4
N_WEATHER = 7
P_MORNING, P_NOON, P_DUSK, P_NIGHT = range(4)
WX_CLEAR, WX_RAIN_L, WX_RAIN_M, WX_RAIN_H, WX_FOG, WX_SNOW, WX_THUNDER = range(7)

# 旧素材雪花的字形（7×7「米」字，23px，占绝大多数）
FLAKE = np.array([
    [1, 0, 0, 1, 0, 0, 1],
    [0, 1, 0, 1, 0, 1, 0],
    [0, 0, 1, 0, 1, 0, 0],
    [1, 1, 1, 1, 1, 1, 1],
    [0, 0, 1, 0, 1, 0, 0],
    [0, 1, 0, 1, 0, 1, 0],
    [1, 0, 0, 1, 0, 0, 1],
], bool)

# ★2026-11-12 用户令：雷的雨要比中雨密（但又不必等于大雨）⇒ 单独给 11.5%
RAIN_DENSITY = {WX_RAIN_L: 0.046, WX_RAIN_M: 0.084, WX_RAIN_H: 0.138, WX_THUNDER: 0.115}


def grow(m):
    """8 连通膨胀 1px（给傍晚点阵上的雨/雪留白边）。"""
    g = m.copy()
    g[1:, :] |= m[:-1, :]
    g[:-1, :] |= m[1:, :]
    g[:, 1:] |= m[:, :-1]
    g[:, :-1] |= m[:, 1:]
    g[1:, 1:] |= m[:-1, :-1]
    g[1:, :-1] |= m[:-1, 1:]
    g[:-1, 1:] |= m[1:, :-1]
    g[:-1, :-1] |= m[1:, 1:]
    return g


def _dusk(w, h):
    """傍晚点阵（旧素材实测，严格 8 行循环，约 34%）。"""
    yy, xx = np.mgrid[0:h, 0:w]
    p = (yy % 2 == 0) & (xx % 2 == 0)
    p |= (yy % 4 == 1) & (xx % 4 == 1)
    p |= (yy % 8 == 3) & (xx % 8 == 3)
    p |= (yy % 8 == 7) & (xx % 8 == 7)
    return p


def _fog(w, h, focus, sparse=False):
    """雾：整屏 4px 点阵，**上疏下密**（旧素材实测三档：起雾区上 21% → 21..38% → 38% 以下）。

    渐变锚点用 focus（窗洞矩形）而不是整屏：图层照样整屏画，但"上疏下密"是跟着
    窗洞走的 —— 窗洞挪了只改这一个矩形，画法不用动。
    sparse=True 只留最上面那一档（夜里雾是白点压在黑天上，太密会糊成一片）。"""
    yy, xx = np.mgrid[0:h, 0:w]
    out = (yy % 4 == 0) & (xx % 4 == 0)
    if sparse:
        return out
    fx0, fy0, fx1, fy1 = focus
    fh = max(1, fy1 - fy0)
    lo, hi = fy0 + int(round(fh * 0.21)), fy0 + int(round(fh * 0.38))
    ph = np.where(((yy // 4) % 2) == 0, 2, 6)
    out |= (yy % 4 == 2) & (xx % 8 == ph)
    out |= (yy >= lo) & (yy % 4 == 2) & (xx % 4 == 2)
    out |= (yy >= hi) & (yy % 2 == 0) & (xx % 2 == 0)
    return out


def _rain(w, h, weather, seed):
    """雨 / 雷的雨丝：1px 45° 斜线（dx/dy=+1），随机分段、密度照旧素材。"""
    dens = RAIN_DENSITY.get(weather, 0.084)
    rng = np.random.RandomState(seed)
    out = np.zeros((h, w), bool)
    need = int(dens * w * h)
    got, guard = 0, 0
    while got < need and guard < 8000:
        guard += 1
        c = int(rng.randint(-h + 2, w))
        y0 = int(rng.randint(-8, max(1, h - 6)))
        for k in range(int(rng.randint(8, 30))):
            y, x = y0 + k, c + y0 + k
            if 0 <= y < h and 0 <= x < w and not out[y, x]:
                out[y, x] = True
                got += 1
    return out


def _snow(w, h, seed):
    """雪：按 13px 网格 + 抖动撒完整「米」字（约 12%）。

    网格间距是**绝对像素**（雪花本身 7px，间距 13px），与画布多大无关 ⇒
    换更大的画布只是多撒几片，窗口里看到的疏密不变。"""
    rng = np.random.RandomState(seed)
    out = np.zeros((h, w), bool)
    step = 13
    fh, fw = FLAKE.shape
    for gy in range(-1, h // step + 2):
        for gx in range(-1, w // step + 2):
            if rng.rand() < 0.18:
                continue
            cy = gy * step + int(rng.randint(0, 7)) - 2
            cx = gx * step + int(rng.randint(0, 7)) - 2
            if cy < 1 or cx < 1 or cy + fh > h - 1 or cx + fw > w - 1:
                continue
            out[cy:cy + fh, cx:cx + fw] |= FLAKE
    return out


def _unit(v):
    n = (v[0] * v[0] + v[1] * v[1]) ** 0.5
    return (v[0] / n, v[1] / n) if n > 1e-9 else (0.0, 0.0)


def _bolt(w, h, focus):
    """闪电：上粗下尖的折线，**尖角拐弯**（miter 折角，不是圆头描边）。

    ★2026-11-12 用户令：拐角要"凌厉"——之前用圆片叠出来，拐角是圆弧的，看着软绵绵。
      现在按每个折点的角平分线做 miter 偏移，再整块多边形填充 ⇒ 拐角是硬折角。
      宽度：顶上 6px，一路收到尖端 1px。"""
    fx0, fy0, fx1, fy1 = focus
    fw, fh = max(1, fx1 - fx0), max(1, fy1 - fy0)
    bx0, bw = fx0 + 0.62 * fw, 0.16 * fw          # ★右移：让开深夜的月相（月亮在窗口 x24..51）
    by0, bh = fy0 + 0.02 * fh, 0.66 * fh
    # 起点画到窗洞上沿之外（-0.06）：顶端被窗框截平，露出来就是最粗的一截。
    # ★2026-11-12 用户令「更凌厉」：横向摆幅加大、折段更短，转角落差 ~100°。
    # ★再调：折段长短/幅度不一，末尾一段长斜线收尖；折数比上一版少一个。
    pts = [(0.62, -0.06), (0.12, 0.16), (0.64, 0.34), (0.14, 0.58),
           (0.44, 0.80), (0.12, 1.00)]
    P = [(bx0 + x * bw, by0 + y * bh) for (x, y) in pts]
    segs = [((P[i + 1][0] - P[i][0]) ** 2 + (P[i + 1][1] - P[i][1]) ** 2) ** 0.5
            for i in range(len(P) - 1)]
    total = max(1e-6, sum(segs))
    halfs, acc = [], 0.0
    for i in range(len(P)):
        s = acc / total
        halfs.append(2.5 * (1.0 - s ** 1.4) + 0.5)     # 半宽：顶 3px → 尖 0.5px
        if i < len(segs):
            acc += segs[i]
    left, right = [], []
    for i, (x, y) in enumerate(P):
        if i == 0:
            din = dout = (P[1][0] - x, P[1][1] - y)
        elif i == len(P) - 1:
            din = dout = (x - P[i - 1][0], y - P[i - 1][1])
        else:
            din = (x - P[i - 1][0], y - P[i - 1][1])
            dout = (P[i + 1][0] - x, P[i + 1][1] - y)
        din, dout = _unit(din), _unit(dout)
        nin = (-din[1], din[0])
        nout = (-dout[1], dout[0])
        nb = _unit((nin[0] + nout[0], nin[1] + nout[1]))
        den = nb[0] * nin[0] + nb[1] * nin[1]
        miter = min(2.2, 1.0 / den) if den > 0.25 else 2.2   # 硬折角，但别戳太远
        off = halfs[i] * miter
        left.append((x + nb[0] * off, y + nb[1] * off))
        right.append((x - nb[0] * off, y - nb[1] * off))
    poly = left + right[::-1]
    img = Image.new("1", (w, h), 0)
    ImageDraw.Draw(img).polygon(poly, fill=1)
    return np.asarray(img, bool)


def _night(w, h, seed, stars):
    """深夜：黑天空（墨），晴天再撒星点（白）。

    ★2026-11-12 用户令：星星太密，再收少一截（单点 ~0.65%，2×2 亮点也再稀）。"""
    out = np.ones((h, w), bool)
    if not stars:
        return out
    rng = np.random.RandomState(20261112 + seed)
    n = int(0.0065 * w * h)
    out[rng.randint(0, h, n), rng.randint(0, w, n)] = False
    for _ in range(max(1, int(w * h / 1800))):
        y = int(rng.randint(0, h - 1))
        x = int(rng.randint(0, w - 1))
        out[y:y + 2, x:x + 2] = False
    return out


def _strokes(weather, w, h, focus, seed, night=False):
    """该天气的笔画（True = 墨），不含天色。"""
    if weather == WX_CLEAR:
        return np.zeros((h, w), bool)
    if weather == WX_FOG:
        return _fog(w, h, focus, sparse=night)
    if weather == WX_SNOW:
        return _snow(w, h, seed)
    out = _rain(w, h, weather, seed)
    if weather == WX_THUNDER:
        out |= _bolt(w, h, focus)
    return out


def render(period, weather, w, h, focus=None, seed=0):
    """整屏生成一帧天气层。返回 h×w 的布尔图，True = 墨（1bpp 打包时取反成 1=白）。

    focus = 窗洞矩形 (x0, y0, x1, y1)，只用来给「雾的上疏下密」「闪电位置」定锚；
    图案本身整屏无缝，窗洞怎么挪都不用重画（改 focus 一处即可）。"""
    if focus is None:
        focus = (0, 0, w, h)
    if period == P_NIGHT:
        base = _night(w, h, seed, stars=(weather == WX_CLEAR))
        if weather == WX_CLEAR:
            return base
        return base & ~_strokes(weather, w, h, focus, seed, night=True)  # 黑天里雨/雪/雾是白的
    strokes = _strokes(weather, w, h, focus, seed)
    if period == P_DUSK:
        if weather == WX_FOG:
            return strokes                               # 雾把傍晚点阵整个盖掉（旧素材就是这样）
        if weather == WX_CLEAR:
            return _dusk(w, h)
        return (_dusk(w, h) & ~grow(strokes)) | strokes  # 笔画周围留白边，傍晚也看得清
    return strokes
