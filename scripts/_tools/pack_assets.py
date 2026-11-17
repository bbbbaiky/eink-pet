#!/usr/bin/env python3
# ============================================================================
# pack_assets.py —— assets 数据分区打包器（2026-10-20 重写版）
# ----------------------------------------------------------------------------
# 唯一真源 = common/assets.h（分区内布局那一段）。本脚本是它的「镜像实现」：
#   把若干**已构建好的区域 .bin**（明信片/UI/字库/图标/动画/猫精灵仓…）拼成
#   一个 AATS 容器，烧进硬件的 assets 分区（见 build2.sh 的 migrate / 整片两条路径）。
#
# ⚠ 关于「压缩」（★R196 改写）：
#   · 各区域**内部**仍有自己的格式（UIRL / RLE1 / ZFNT / 猫精灵仓…），由各自生成器负责。
#   · ★R196 起 AATS 追加**区域级 deflate 容器 'ZLZ1'**：u32 magic + u32 原长 +
#     zlib.compress(data, 9) 流。开机由 assets_shadow_load_all() 解到 PSRAM 影子缓冲，
#     assets_region_read() 优先 memcpy ⇒ 上层所有调用一行都不用改。
#   · 是否压缩按 **4KB 对齐后的净收益**自动判定：align4k(blob) < align4k(raw) 才压，
#     否则原样存（小区压了也占同样一页，白费一次解码）。
#   · 旧的 'RLZ1' 区域级 RLE 容器**不再由本脚本产出**，但固件仍认（兼容旧 assets，
#     这样手机网页 OTA 只升 app 时，旧 assets 照常能读）。
#
# 分区内布局（全 LE，详见 assets.h）：
#   0x0000..0x0FFF  4KB 头：magic 'AATS' + ver + n + payload_crc32 + payload_end
#                     + 目录 n×{u32 id, u32 off, u32 len}（每项 12B，从 0x18 起）
#   0x1000..         各区域，每个起点 4KB 对齐（off 是 0x1000 的整数倍），
#                    长度 = 区域真实字节数；区域之间用 0 填充到下一个 4KB。
#   文件总长 = payload_end（= 最后一个区域 off+len，无尾部填充）。
#
# 区域顺序 / ID 必须 = assets.h 的 enum（启动时会按 AID_N 核目录项数）。
#
# 用法：
#   python scripts/_tools/pack_assets.py --write            # 重建 common/assets.bin
#   python scripts/_tools/pack_assets.py --write OUT.bin    # 输出到指定文件
#   python scripts/_tools/pack_assets.py --inspect [BIN]    # 打印区域表 + 校验
#   python scripts/_tools/pack_assets.py --verify  [BIN]    # 用源文件重拼，逐字节比对 BIN
# ============================================================================
import argparse
import os
import re
import struct
import sys
import zlib
from pathlib import Path

# ── 路径 ────────────────────────────────────────────────────────────────────
# 脚本位于 <root>/scripts/_tools/pack_assets.py → root = 上溯三级
ROOT = Path(__file__).resolve().parent.parent.parent
# ★R108 分版输出：按键版 / 触屏版各一份（UI 容器只带本版那套）
OUT_BY_VARIANT = {
    "btn":   ROOT / "common" / "assets_btn.bin",
    "touch": ROOT / "common" / "assets_touch.bin",
}
DEFAULT_OUT = OUT_BY_VARIANT["btn"]

# ── 容器常量（与 assets.h 同步）─────────────────────────────────────────────
ASSETS_MAGIC   = 0x53544141   # "AATS"
ASSETS_VER     = 1
ASSETS_HDR_LEN = 0x1000       # 分区头 4KB


# ── ★R107（2026-10-28）动态护栏 ────────────────────────────────────────────
# 事故回放：BUILTIN_MAX 原先**写死** 0x34C000，注释里写的是 99 槽时代的
#   mypost 预留 0x64000。2026-10-22 用户把明信片扩到 200 槽（MYPOST_SIZE →
#   0xC9000），这里**没跟着改**，于是护栏形同虚设：R106 字库涨到 723 KB 时
#   pack_assets 一声不吭地打包出 pend=0x31F000，越过真正的界 0x2E7000，
#   开机后固件直接**禁用用户明信片仓**。
# 修法：两个输入都从唯一真源读，不再硬编码：
#   · assets 分区大小  ← fw_epaper_demo/partitions.csv
#   · MYPOST_SIZE      ← common/main_api.h
def _read_assets_part_size() -> int:
    csv = ROOT / "fw_epaper_demo" / "partitions.csv"
    for line in csv.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        cols = [c.strip() for c in line.split(",")]
        if cols and cols[0] == "assets":
            try:
                return int(cols[4], 16)
            except (IndexError, ValueError):
                break
    sys.exit("!! 读不到 assets 分区大小：%s 里没有合法 assets 行" % csv)


def _read_mypost_size() -> int:
    h = ROOT / "common" / "main_api.h"
    m = re.search(r"#define\s+MYPOST_SIZE\s+(0x[0-9A-Fa-f]+|[0-9]+)",
                  h.read_text(encoding="utf-8", errors="ignore"))
    if not m:
        sys.exit("!! 读不到 MYPOST_SIZE：%s" % h)
    return int(m.group(1), 0)


ASSETS_PART_SZ = _read_assets_part_size()
MYPOST_SIZE    = _read_mypost_size()
BUILTIN_MAX    = ASSETS_PART_SZ - MYPOST_SIZE   # 内置内容上界（分区尾留出 mypost）

# 区域表：按 assets.h 的 enum 顺序（id = 下标）。每一项 = (名字, 相对 ROOT 的源文件)。
# 映射已用「现有 common/assets.bin 每个区域逐字节反查源文件」确认无误。
# 注意：共享的 assets.bin 由**按键版**源树构建（POST/CAT/WIN/ROOM/OFF/BOWL/PATCH/
#       ICON/MOON 取自 fw_epaper_demo/main/assets/），触屏专属 UI 取自触屏树。
# ★R108（2026-10-29）分版打包：UI 容器按版本只带一份
#   原先 UI_TOUCH(176KB) 与 UI_BTN(140KB) **同时在**同一个 assets.bin 里，
#   而固件 main.c 编译期就用 AID_UI_CUR 二选一（#if TOUCH_DEMO → UI_TOUCH，
#   否则 UI_BTN），非本版那份**永远读不到**。按版本拆包后：
#     按键版 assets 省 176,128 B · 触屏版省 139,264 B
#   ⚠ 目录项数必须等于 assets.h 的 AID_N（真源看 assets.h，注释里不写死数字）：
#     assets_boot_check() 校验 n != AID_N 直接
#     报 "dir count" 停兜底页；且每项 len 不能为 0（"bad dir"）。
#     ⇒ 非本版区域填 16B 哨兵 stub，而不是删除条目。
VARIANTS = ("btn", "touch")

REGIONS = [
    ("POST",       "fw_epaper_demo/main/assets/post.bin",   None),
    ("UI_TOUCH",   "fw_touch/main/assets_touch/ui_touch_rle.bin", "touch"),
    ("UI_BTN",     "fw_epaper_demo/main/assets/ui_rle.bin", "btn"),
    ("LOOT",       "common/loot_icons.bin",                 None),
    ("ZFONT",      "common/zfont.bin", None),
    ("ANIM_REJECT", "common/anim_reject.bin", None),
    ("ANIM1",      "common/anim1.bin", None),
    ("ANIM2",      "common/anim2.bin", None),
    ("ANIM3",      "common/anim3.bin", None),
    ("ANIM4",      "common/anim4.bin", None),
    ("ANIM5",      "common/anim5.bin", None),
    ("ANIM6",      "common/anim6.bin", None),
    ("ANIM7",      "common/anim7.bin", None),
    ("CAT",        "fw_epaper_demo/main/assets/cat.bin", None),
    # ★R202（2026-11-13）：WIN / ROOM / WIN_TECH 三个区**固件已不再读取**
    #   （R194 起窗外改成「天气层 + 风景层」，房间改成 room_plain/room_tech + 窗洞掩膜；
    #     全仓 grep 只有 assets.h 的枚举声明，没有任何 assets_region_read(AID_WIN/…)）。
    #   目录条目必须保留（启动校验要求项数 == AID_N），这里填 16B 哨兵，不打包真实字节。
    #   源文件 room.bin 仍被 gen_land_face.py 当「默认房间清晨/傍晚」的素材用，别删。
    ("WIN",        None, "unused"),
    ("ROOM",       None, "unused"),
    ("OFF",        "fw_epaper_demo/main/assets/off.bin", None),
    ("BOWL",       "fw_epaper_demo/main/assets/bowl.bin", None),
    ("PATCH",      "fw_epaper_demo/main/assets/patch.bin", None),
    ("ICON",       "fw_epaper_demo/main/assets/icon.bin", None),
    ("MOON",       "fw_epaper_demo/main/assets/moon.bin", None),
    ("I18N",       "common/i18n_asset.bin", None),
    ("WEBUI",      "common/webui.bin", None),
    ("ROOM_TECH",  "fw_epaper_demo/main/assets/room_tech.bin", None),
    ("WIN_TECH",   None, "unused"),
    ("ROOM_PLAIN", "fw_epaper_demo/main/assets/room_plain.bin", None),
    ("LAND_WEATHER", "fw_epaper_demo/main/assets/land_weather.bin", None),
    ("LAND_BASE",  "fw_epaper_demo/main/assets/land_base.bin", None),
]


def align4k(x: int) -> int:
    return (x + 0xFFF) & ~0xFFF


# ══ ★R196：区域级 deflate 容器（'ZLZ1'）══════════════════════════════════
# 为什么换：R182 的字节游程 RLE1 对 1bpp 抖动半调图 / 汉字字形表几乎无效，
#   实测整区换 zlib(level 9) 后触屏 assets 3,014,656 → 1,843,200 B（省 1,171,456 B）。
# 容器：u32 magic 'ZLZ1' + u32 raw_len + zlib 流（带 zlib 头 + adler32，
#   固件用 ESP32-S3 ROM 里现成的 tinfl_decompress_mem_to_mem(..., TINFL_FLAG_PARSE_ZLIB_HEADER)）。
# 为什么所有区都能压：影子缓冲是**整区**解压到 PSRAM，之后 assets_region_read()
#   读到的是解压后的原始字节 ⇒ UIRL / ZFNT / RLE1 动画这些「自带容器」的解析层
#   完全不用改（压的是容器文件本身，不是容器里的页/帧）。
# ⚠ 旧 'RLZ1'（R182）不再产出，但固件保留解码分支：新 app 读旧 assets 不崩，
#   网页 OTA 只写 app 不写 assets 的路径因此是安全的。
ZLZ_MAGIC = 0x315A4C5A  # 'ZLZ1'（LE: 5A 4C 5A 31）


# ★R196：下面两个函数**打包路径已不再调用**（新包一律走 zlib_wrap）。
#   保留作 RLE1 帧编码 / 旧 'RLZ1' 包的对照实现（动画容器是预生成 bin，没有独立生成器）。
def rle_encode(src: bytes) -> bytes:
    """与固件 anim_rle_decode 对应的编码（游程≥3 用游程，否则攒字面）。"""
    out = bytearray()
    i, n = 0, len(src)
    while i < n:
        # 先看能不能起一个游程
        j = i
        while j + 1 < n and src[j + 1] == src[i] and (j - i + 2) <= 128:
            j += 1
        run = j - i + 1
        if run >= 3:
            out.append(0x80 | (run - 1))
            out.append(src[i])
            i += run
            continue
        # 字面：一直攒到「下一个 3 连」之前，最长 128
        lit = bytearray()
        while i < n and len(lit) < 128:
            k = i
            while k + 1 < n and src[k + 1] == src[i] and (k - i + 2) <= 128:
                k += 1
            if k - i + 1 >= 3:
                break
            lit.append(src[i])
            i += 1
        out.append(len(lit) - 1)
        out += lit
    return bytes(out)


def rle_decode(src: bytes, raw_len: int) -> bytes:
    """固件解码器的 Python 复刻（写包前自检用）。"""
    out = bytearray()
    i = 0
    while i < len(src):
        c = src[i]
        i += 1
        if c & 0x80:
            n = (c & 0x7F) + 1
            out += bytes([src[i]]) * n
            i += 1
        else:
            n = c + 1
            out += src[i:i + n]
            i += n
    if len(out) != raw_len:
        raise SystemExit("!! RLE 自检失败：解出 %d B，应为 %d B" % (len(out), raw_len))
    return bytes(out)


def zlib_wrap(name: str, data: bytes) -> bytes:
    """★R196：整区 deflate。只在 **4KB 对齐后真的省页** 时才压。

    返回 'ZLZ1' 容器或原样字节；固件 assets_shadow_load_all() 按头 4 字节
    magic 分流（ZLZ1 → tinfl；RLZ1 → rle1_decode；其它 → 未压缩直读）。
    """
    enc = zlib.compress(data, 9)
    blob = struct.pack("<II", ZLZ_MAGIC, len(data)) + enc
    if align4k(len(blob)) >= align4k(len(data)):
        print("   %-13s 压后仍占同样页数（%d → %d）→ 保持原样" % (name, len(data), len(blob)))
        return data
    # 写包前自检：解回来必须逐字节一致（zlib 自带 adler32，固件侧再核一次长度）
    if zlib.decompress(enc) != data:
        raise SystemExit("!! %s deflate 往返不一致，拒绝打包" % name)
    print("   %-13s ZLZ1 %8d → %8d B（%.1f%%，对齐后省 %d B）"
          % (name, len(data), len(blob), 100.0 * len(blob) / len(data),
             align4k(len(data)) - align4k(len(blob))))
    return blob


def stub_for(name: str, variant: str) -> bytes:
    """★R108 非本版区域的占位哨兵（16B，非零长度，固件不会去读它）。"""
    tag = ("ASTUB:%s:%s" % (name, variant)).encode("ascii")[:16]
    return tag + b"\x00" * (16 - len(tag))


def load_regions(variant: str):
    """读取所有区域源文件，返回 [(name, off, bytes)] 并算好 4KB 对齐偏移。

    variant='btn'|'touch'：REGIONS 第三列标了别的版本的区域 → 换成 16B stub，
    不打包真实字节（固件编译期就选死只读 AID_UI_CUR，那份永远读不到）。
    """
    entries = []
    off = ASSETS_HDR_LEN
    for name, rel, only in REGIONS:
        if rel is None or (only is not None and only != variant):
            # ★R202：rel=None = 固件已不再读取的退役区（WIN / ROOM / WIN_TECH）→ 哨兵占位
            entries.append((name, off, stub_for(name, "none")))
            off = align4k(off + 16)
            continue
        p = ROOT / rel
        if not p.is_file():
            sys.exit(f"!! 缺源文件：{rel}（{p}）\n"
                     f"   请先构建该区域（见 assets.h 各区域格式说明），再重跑本脚本。")
        data = p.read_bytes()
        if len(data) == 0:
            sys.exit(f"!! 源文件为空：{rel}")
        entries.append((name, off, zlib_wrap(name, data)))
        off = align4k(off + len(entries[-1][2]))
    return entries


def build(variant: str = "btn"):
    """组装 AATS 容器，返回完整 bytes。"""
    entries = load_regions(variant)
    n = len(entries)
    # 末尾也对齐到 4KB（整块烧录友好；与旧 bin 行为一致）。
    # 实测现有 common/assets.bin 总长 = 392×4096，即最后一个区域末尾向上对齐所得。
    pend = align4k(entries[-1][1] + len(entries[-1][2]))
    # ★2026-10-20 护栏 · ★R107 改为动态：内置内容不得越过 mypost 预留界
    #   （BUILTIN_MAX = partitions.csv 的 assets 大小 − main_api.h 的 MYPOST_SIZE）
    if pend > BUILTIN_MAX:
        sys.exit("!! 内置内容 pend=0x%X 超过上界 0x%X\n"
                 "   assets 分区 0x%X − MYPOST_SIZE 0x%X = 0x%X\n"
                 "   请收敛内置区域，或调整 mypost 预留大小后再打包。"
                 % (pend, BUILTIN_MAX, ASSETS_PART_SZ, MYPOST_SIZE, BUILTIN_MAX))
    # 头部 4KB
    hdr = bytearray(ASSETS_HDR_LEN)
    struct.pack_into("<IIIII", hdr, 0,
                     ASSETS_MAGIC, ASSETS_VER, n, 0, pend)
    for i, (name, off, data) in enumerate(entries):
        struct.pack_into("<III", hdr, 0x18 + i * 12, i, off, len(data))
    # 负载
    payload = bytearray(pend - ASSETS_HDR_LEN)
    for name, off, data in entries:
        payload[off - ASSETS_HDR_LEN: off - ASSETS_HDR_LEN + len(data)] = data
    # 全量 CRC32（zlib 同款，与固件 a_crc32_up 一致）
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    struct.pack_into("<I", hdr, 0x0C, crc)
    return bytes(hdr) + bytes(payload)


def write_out(out_path: Path, variant: str = "btn"):
    blob = build(variant)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(blob)
    print(f">>> 写出 {out_path}（{len(blob)} B，{len(REGIONS)} 区，variant={variant}）")


def inspect(bin_path: Path):
    d = bin_path.read_bytes()
    if len(d) < ASSETS_HDR_LEN:
        sys.exit("!! 文件太短，不是合法 assets.bin")
    magic, ver, n, crc, pend = struct.unpack_from("<IIIII", d, 0)
    print(f"magic=0x{magic:08X} ver={ver} n={n} crc=0x{crc:08X} pend={pend} "
          f"filesize={len(d)}")
    ok = (magic == ASSETS_MAGIC and ver == ASSETS_VER and n == len(REGIONS)
          and pend <= len(d))
    print(f"magic_ok={magic==ASSETS_MAGIC} ver_ok={ver==ASSETS_VER} "
          f"n_ok={n==len(REGIONS)} pend_ok={pend<=len(d)}")
    for i in range(n):
        rid, off, ln = struct.unpack_from("<III", d, 0x18 + i * 12)
        tag = REGIONS[i][0] if i < len(REGIONS) else "?"
        print(f"  [{i:2d}] {tag:<11} id={rid} off=0x{off:06X} len={ln}")
    # ★R153（全量检阅 P1-1）：CRC 不符必须让 --inspect 返回失败。
    # 原来只打印 MISMATCH 却照样 return True，等于没校验。
    crc_ok = True
    if pend > ASSETS_HDR_LEN:
        calc = zlib.crc32(d[ASSETS_HDR_LEN:pend]) & 0xFFFFFFFF
        crc_ok = (calc == crc)
        print(f"payload CRC32: 声明=0x{crc:08X} 重算=0x{calc:08X} "
              f"-> {'OK' if crc_ok else 'MISMATCH'}")
    return ok and crc_ok


def verify(bin_path: Path, variant: str = "btn"):
    """用源文件重拼，与给定 BIN 逐字节比对；并校验头部字段。"""
    rebuilt = build(variant)
    cur = bin_path.read_bytes()
    if rebuilt == cur:
        print(f">>> 逐字节一致：{bin_path} == 源文件重拼结果（{len(cur)} B）✅")
        print(">>> 说明：pack_assets.py 的打包逻辑与现有 assets.bin 完全吻合，"
              "可放心用于后续资源改动。")
        return True
    # 找出首个差异
    lim = min(len(rebuilt), len(cur))
    first = next((i for i in range(lim) if rebuilt[i] != cur[i]), lim)
    print(f"!! 不一致：{bin_path} 与源文件重拼结果在偏移 0x{first:X} 起不同")
    print(f"   rebuilt={len(rebuilt)} B  cur={len(cur)} B")
    return False


def main():
    ap = argparse.ArgumentParser(description="assets 分区打包器 (AATS)")
    ap.add_argument("--variant", choices=VARIANTS, default=None,
                    help="版本：btn=按键(只带 UI_BTN) / touch=触屏(只带 UI_TOUCH)。"
                         "省略 = 两个版本各打一份。")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--write",   nargs="?", const="__default__",
                   metavar="OUT", help="重建 assets bin（默认按 variant 落到 "
                                       "common/assets_btn.bin / assets_touch.bin）")
    g.add_argument("--inspect", nargs="?", const="__default__",
                   metavar="BIN", help="打印区域表 + 校验")
    g.add_argument("--verify",  nargs="?", const="__default__",
                   metavar="BIN", help="用源文件重拼，逐字节比对 BIN")
    args = ap.parse_args()

    variants = (args.variant,) if args.variant else VARIANTS

    def out_for(v):
        # ★R144：原先只看 args.write，导致 --inspect/--verify 传的 BIN 被静默忽略
        # （想验回读镜像却验了默认包，假阳性）。现在按当前动作取参数。
        chosen = (args.write if args.write is not None else
                  args.inspect if args.inspect is not None else args.verify)
        if chosen in (None, "__default__"):
            return OUT_BY_VARIANT[v]
        if len(variants) > 1:
            raise SystemExit("!! 指定自定义 BIN 时必须同时给 --variant btn|touch")
        return Path(chosen)

    # ★R153（全量检阅 P1-1）：原来 verify()/inspect() 的 False 被丢弃，且末尾裸
    # main() ⇒ 退出码恒 0，build2*.sh 的 `|| exit 1` 永不触发（assets 闸形同虚设）。
    rc = 0
    if args.write is not None:
        for v in variants:
            write_out(out_for(v), v)
    elif args.inspect is not None:
        for v in variants:
            print(f"──────── variant={v} ────────")
            if not inspect(out_for(v)): rc = 1
    elif args.verify is not None:
        for v in variants:
            print(f"──────── variant={v} ────────")
            if not verify(out_for(v), v): rc = 1
    else:
        ap.print_help()
        # 默认动作 = 重建两版（与 build2.sh / build2_touch.sh 的调用习惯一致）
        for v in variants:
            write_out(out_for(v), v)
    return rc


if __name__ == "__main__":
    sys.exit(main())
