#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# ============================================================================
# check_i18n_full.py —— 三语翻译「全量覆盖」检查（R141 立）
# ----------------------------------------------------------------------------
# 为什么需要它（这段时间反复出错的根因）：
#   1) 以前只查 `i18n_table_data.h`（源码 T("…") 的那张表），
#      而**明信片译文另有一张 `post_i18n_data.h`**（1727 条）—— 漏查它就会误报
#      「明信片缺 459 条」（R140 我就这么误报过一次）。
#   2) 以前的判据是「key 在不在表里」，抓不到「有条目但某语种的值等于中文」
#      这种**质量缺口**（R141 实测 77 条：44 条缺英文、68 条缺繁体、2 条缺日文）。
#
# 本脚本做三件事：
#   A. 收集所有「会上屏的中文字符串」：源码 T("…") 字面量 + 各数据表字段
#      （明信片 title/place/city/body、收藏品名/说明/小传、成就名/描述、目的地名）
#   B. 合并两张译文表（i18n_table_data.h 的 EN/JA/ZHT + post_i18n_data.h 的四语行）
#   C. 逐条检查四项，任一不过 → 退出码 1（构建失败）：
#        ① 缺条目（该串在两张表里都找不到）
#        ② 空值（EN/JA/ZHT 任一为空）
#        ③ 未翻（EN 或 JA 与中文完全相同，且中文含汉字）
#        ④ 繁体未转（opencc s2tw 认为该变，而 ZHT 值仍是简体）
#
# 用法：
#   python scripts/_tools/check_i18n_full.py            # 检查（默认）
#   python scripts/_tools/check_i18n_full.py --list     # 只列缺口，不阻断
#   python scripts/_tools/check_i18n_full.py --strict   # 把「可疑但可接受」也算缺口
# 依赖：opencc（pip install opencc-python-reimplemented；★R217 缺了构建**直接拦下**，
#       依赖清单见仓库根 requirements.txt，须装进「构建用的 IDF python」；--list 模式仅提示）
# ============================================================================
import re
import sys
import glob
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

try:
    from opencc import OpenCC
    _CC = OpenCC("s2tw")
except Exception:                                    # noqa: BLE001
    _CC = None

HAN = re.compile(r"[\u4e00-\u9fff]")
ESC = r'(?:[^"\\]|\\.)'
# ★2026-11-12（全量检查 P1-1 / R148 遗留）：上屏绘制入口。
# text_core.h:15 规定 fb_text_* 自包 t_()、调用点**本来就该传裸中文**，
# 所以规则是「把绘制参数里的裸中文纳入受检」，不是「禁止裸串 / 强制包 T()」。
DRAW_API = re.compile(
    r"\b(?:fb_text_[a-z_]+|fb_btn_text_[a-z_]+|mypost_text_page|"
    r"ui_[a-z_]*text[a-z_]*|draw_[a-z_]*text[a-z_]*)\s*\(([^\n;]*)")


def unesc(s):
    return (s.replace('\\"', '"').replace("\\n", "\n")
             .replace("\\t", "\t").replace("\\\\", "\\"))


def read(path):
    return open(os.path.join(ROOT, path), encoding="utf-8").read()


# ── B. 两张译文表 ────────────────────────────────────────────────────────────
def load_main_table():
    """i18n_table_data.h → [{key: value} × 3]（EN / JA / ZHT 各一张）。"""
    src = read("common/i18n_table_data.h")
    out = []
    for m in re.finditer(r"TABLE\[\]\s*=\s*\{", src):
        j = src.index("\n};", m.end())
        blk = src[m.end():j]
        out.append({unesc(a): unesc(b) for a, b in
                    re.findall(r'\{\s*"(' + ESC + r'*)"\s*,\s*"(' + ESC + r'*)"\s*\}', blk)})
    if len(out) != 3:
        sys.exit("!! i18n_table_data.h 里应有 3 张表，实际 %d 张" % len(out))
    return out


def load_post_table():
    """post_i18n_data.h → {zh: (en, ja, zht)}（四语一行）。"""
    src = read("common/post_i18n_data.h")
    d = {}
    pat = (r'\{\s*"(' + ESC + r'*)"\s*,\s*"(' + ESC + r'*)"\s*,\s*"('
           + ESC + r'*)"\s*,\s*"(' + ESC + r'*)"\s*\}')
    for m in re.finditer(pat, src):
        zh, en, ja, zht = (unesc(m.group(1)), unesc(m.group(2)),
                           unesc(m.group(3)), unesc(m.group(4)))
        d[zh] = (en, ja, zht)
    return d


# ── A. 会上屏的中文串 ────────────────────────────────────────────────────────
def source_files():
    """★2026-11-12：连 .h 一起扫（次级盲区：旧脚本只看 *.c）。"""
    return (glob.glob(os.path.join(ROOT, "common", "*.c"))
            + glob.glob(os.path.join(ROOT, "common", "*.h")))


def scan_source_literals():
    """源码里 T("…") 的字面量；★2026-11-12 起支持 C 相邻字符串拼接 T("a" "b")。"""
    out = set()
    for f in source_files():
        s = strip_c(open(f, encoding="utf-8", errors="replace").read())
        # 拼接串在译文表里的 key 是**拼完的整串**，不能拆成多个 key
        for m in re.finditer(r'\bT\(\s*((?:"' + ESC + r'*"\s*)+)\)', s):
            out.add("".join(unesc(q.group(1))
                            for q in re.finditer(r'"(' + ESC + r'*)"', m.group(1))))
        # ★R144：fb_text_*() 内部的 t_() 也会查表，行内裸中文同样要覆盖
        for m in re.finditer(r'\bfb_text_[a-z_]+\([^;]*?' + r'"(' + ESC + r'{2,})"', s):
            v = unesc(m.group(1))
            if HAN.search(v) and "%" not in v:
                out.add(v)
    return out


def scan_draw_literals():
    """★2026-11-12（P1-1 / R148 遗留）：绘制入口参数里的**全部**裸中文也进 items。
    旧规则的盲区：只认 T("…")、且 fb_text_* 每行只取第一个串 ⇒ 漏包/漏译时
    闸门照样报「通过」。这里取全部串、并把 .h 也纳入扫描。
    只负责「纳入检查」，不要求调用点包 T()（见 DRAW_API 注释）。"""
    out = set()
    for f in source_files():
        s = strip_c(open(f, encoding="utf-8", errors="replace").read())
        for m in DRAW_API.finditer(s):
            for q in re.finditer(r'"(' + ESC + r'*)"', m.group(1)):
                v = unesc(q.group(1))
                if HAN.search(v) and "%" not in v:
                    out.add(v)
    return out


def strip_c(src):
    """去掉 C 注释，避免把注释里的引号对/说明文字当成 UI 字符串。"""
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def _block(src, start_pat, end="\n};"):
    m = re.search(start_pat, src)
    if not m:
        raise RuntimeError("!! 数据表没找到（正则失配）：%s" % start_pat)
    j = src.index(end, m.end())
    blk = src[m.end():j]
    if not blk.strip():
        raise RuntimeError("!! 数据表内容为空：%s" % start_pat)
    return blk


def scan_data_tables():
    """各数据表里会上屏的中文（按 t_() 查表的那些）。"""
    out = set()

    # 明信片：5 字段（title / place / date / city / body）；date 是 "01-02" 这类，跳过
    p = read("common/post_system.c")
    blk = _block(p, r"const post_t g_posts\[\]\s*=\s*\{")
    for m in re.finditer(r'\{\s*"(' + ESC + r'*)"\s*,\s*"(' + ESC
                         + r'*)"\s*,\s*"(' + ESC + r'*)"\s*,\s*"(' + ESC
                         + r'*)"\s*,\s*"(' + ESC + r'*)"', blk):
        for idx in (0, 1, 3, 4):                            # 跳过 idx=2（date）
            v = unesc(m.group(idx + 1))
            if v:
                out.add(v)

    # 收藏品：名字 + 说明
    ls = read("common/loot_system.c")
    blk = _block(ls, r"const loot_t g_loot\[\]\s*=\s*\{")
    for m in re.finditer(r'\{\s*"(' + ESC + r'*)"\s*,\s*"(' + ESC + r'*)"', blk):
        for g in m.groups():
            v = unesc(g)
            if v:
                out.add(v)
    # 收藏品小传
    # ★R198：g_loot_desc 定义加了 const 限定符（指针数组进 .flash.rodata）；两种声明都要能扫到
    blk = _block(ls, r"const char \*\s*(?:const\s+)?g_loot_desc\[N_LOOT\]\s*=\s*\{")
    for m in re.finditer(r'"(' + ESC + r'{2,})"', blk):
        out.add(unesc(m.group(1)))

    # 成就：名字 + 描述
    mc = read("common/main.c")
    blk = _block(mc, r"static const achv_t ACHV_TBL\[\]")
    for m in re.finditer(r'\{\s*ACHV_KIND_\w+,\s*\d+,\s*\{[^}]*\},\s*"(' + ESC
                         + r'*)"\s*,\s*"(' + ESC + r'*)"', blk):
        for g in m.groups():
            v = unesc(g)
            if v:
                out.add(v)

    # 目的地名
    blk = _block(mc, r"const exp_dest_t EXP_TBL")
    for m in re.finditer(r'\{\s*"(' + ESC + r'*)"', blk):
        v = unesc(m.group(1))
        if v:
            out.add(v)

    # ★R144：日记模板句（经 t_(变量) 上屏，不是 T("...") 字面量）
    # ★R145：DIARY_LEAD 整组删除（场景句改为原样上屏），只剩 TPL/TAIL 两个池。
    STR = r'"([^"\n]{2,})"'
    for name in ("DIARY_TPL", "DIARY_TAIL"):
        blk = strip_c(_block(mc, r"static const char \*const " + name + r"\[\d+\]\s*=\s*\{"))
        for m in re.finditer(STR, blk):
            out.add(unesc(m.group(1)))
    # ★R144：商店池商品名 / 底部提示带裸串（只收真字符串，注释已剥）
    for pat in (r"const shop_pool_t SHOP_POOL\[\]\s*=\s*\{",
                r"static const char \*const PAGE_HINT\[\]\s*=\s*\{"):
        blk = strip_c(_block(mc, pat))
        for m in re.finditer(STR, blk):
            v = unesc(m.group(1))
            if HAN.search(v) and "%" not in v:
                out.add(v)
    return out



# ── ★R144：单行字段的译文宽度闸（按 zfont.bin 真实前进宽度）────────────────
def zf_width(s, title=False):
    """与固件 zf_width() 同口径：BODY 比例 ASCII / CJK 10；TITLE ASCII 6 / CJK 12。"""
    try:
        raw = open(os.path.join(ROOT, "common", "zfont.bin"), "rb").read()
        adv = [raw[16 + i * 11] for i in range(95)]
    except Exception:                                    # noqa: BLE001
        # ★2026-11-12：不再静默降级 —— 读不到要留一句，免得把「没检查」当「通过」
        if not zf_width.warned:
            print("  ⚠ zfont.bin 读不到 → 译文超宽检查跳过（不等于通过）")
            zf_width.warned = True
        return 0                                         # 量不了就跳过，不误拦
    w = 0
    for c in s:
        cp = ord(c)
        if 0x20 <= cp <= 0x7E:
            w += 6 if title else adv[cp - 0x20]
        else:
            w += 12 if title else 10
    return w


zf_width.warned = False


def single_line_fields():
    """返回 [(中文, 是否 TITLE 档, max_w)]：只列真正单行显示、超宽会被截成省略号的字段。"""
    out = []
    loot = read("common/loot_system.c")
    blk = _block(loot, r"const loot_t g_loot\[\]\s*=\s*\{")
    for m in re.finditer(r'\{\s*"(' + ESC + r'*)"\s*,\s*"(' + ESC + r'*)"', blk):
        out.append((unesc(m.group(2)), False, 184))     # 收藏品 note，main.c:10778
    mc = read("common/main.c")
    blk = _block(mc, r"static const achv_t ACHV_TBL\[\]")
    for m in re.finditer(r'\{\s*ACHV_KIND_\w+,\s*\d+,\s*\{[^}]*\},\s*"(' + ESC
                         + r'*)"\s*,\s*"(' + ESC + r'*)"', blk):
        out.append((unesc(m.group(2)), True, 184))      # 成就 desc，main.c:11881
    for name in ("DIARY_TPL", "DIARY_TAIL"):            # ★R145：DIARY_LEAD 已删
        blk = strip_c(_block(mc, r"static const char \*const " + name + r"\[\d+\]\s*=\s*\{"))
        for m in re.finditer(r'"([^"\n]{2,})"', blk):
            v = unesc(m.group(1))
            if "%" not in v:                            # %s 骨架运行时才定长，待核实
                out.append((v, True, 158))              # 日记模板，main.c:12647/12653
    return out


def check_widths(tr):
    bad = []
    for zh, title, lim in single_line_fields():
        t = tr(zh)
        if t is None:
            continue
        for i, lang in enumerate(("EN", "JA", "ZHT")):
            v = t[i]
            if not v:
                continue
            w = zf_width(v, title)
            if w > lim:
                bad.append((lang, zh[:20], v[:28], w, lim))
    return bad

# ── C. 检查 ─────────────────────────────────────────────────────────────────
def main():
    strict = "--strict" in sys.argv
    only_list = "--list" in sys.argv

    main_tbl = load_main_table()
    post_tbl = load_post_table()
    EN, JA, ZHT = main_tbl

    def tr(s):
        """返回 (en, ja, zht) 或 None。★R144：与实机 t_() 一致，POST_I18N 优先。"""
        if s in post_tbl:
            return post_tbl[s]
        if s in EN:
            return EN[s], JA.get(s, ""), ZHT.get(s, "")
        return None

    items = scan_data_tables() | scan_source_literals() | scan_draw_literals()
    items = {s for s in items if HAN.search(s)}           # 纯 ASCII 不需要翻
    # ★2026-11-12：译文表孤儿键只提示、不拦（多一条译文是历史遗留，不是故障）
    orphan = {k for k in (set(EN) | set(post_tbl)) if HAN.search(k)} - items
    if orphan:
        print("  · 译文表孤儿键 %d 条（没有对应上屏串，不拦构建）" % len(orphan))
    problems = {"缺条目": [], "空值": [], "未翻": [], "繁体未转": [], "译文超宽": []}
    problems["译文超宽"] = check_widths(tr)               # ★R144：单行字段三语宽度

    for s in sorted(items):
        t = tr(s)
        if t is None:
            problems["缺条目"].append(s)
            continue
        en, ja, zht = t
        if not en.strip() or not ja.strip() or not zht.strip():
            problems["空值"].append((s, en[:12], ja[:12], zht[:12]))
            continue
        if en == s:
            problems["未翻"].append((s, "EN"))
        elif ja == s and len(s) > 6:                      # 短词（上海/京都）日文同形是正常的
            problems["未翻"].append((s, "JA"))
        if _CC and _CC.convert(s) != s and zht == s:
            problems["繁体未转"].append(s)          # ★R144：去掉 len>4 豁免
        if strict and _CC and _CC.convert(zht) != zht:
            problems.setdefault("繁体软差异", []).append((s, zht[:16]))

    total = sum(len(v) for v in problems.values())
    print("i18n 全量检查：待检中文串 %d（主表 %d 条 · 明信片表 %d 条）"
          % (len(items), len(EN), len(post_tbl)))
    for k, v in problems.items():
        if v:
            print("  !! %s %d 条" % (k, len(v)))
            for it in v[:8]:
                print("       ", it)
    if not _CC and not only_list:
        # ★R217（用户令）：原来只打印警告后照样"通过"，等于三语检查悄悄缺了一环。
        #   现在硬拦；依赖见仓库根 requirements.txt，须装进**构建用的 IDF python**。
        print("!! 未装 opencc：无法检查「繁体未转」，构建已拦下")
        print("   请用构建解释器执行：<IDF python> -m pip install opencc-python-reimplemented")
        return 1
    if not _CC:
        print("  ⚠ 未装 opencc：--list 模式跳过「繁体未转」检查（正式构建会拦下）")

    if total == 0:
        print(">>> 三语检查通过：无缺口")
        return 0
    if only_list:
        return 0
    print("!! 共 %d 处缺口，构建已拦下（R141 用户令：三语必须全）" % total)
    return 1


if __name__ == "__main__":
    sys.exit(main())
