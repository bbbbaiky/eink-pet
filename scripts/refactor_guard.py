#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
重构闸门（refactor guard）：拆巨函数期间每次改完都跑一次，替人盯「不该变的东西变没变」。

为什么要它
----------
拆函数是「纯重构」——对外行为、上屏像素、内存布局都不该变。但拆错了编译器一句话都不说：
* static 计时器留错函数 → 手势判定串味
* 双版本只改一份 → 两版行为分叉
* 全局读写被切开 → 时序契约丢失

编译器不报的，只能拿「可观测量的漂移」兜：
  1) 固件体积（次版本级改动应几乎不变，纯重构尤其如此）
  2) 内部 DRAM 余量（静态变量搬家会挪段）
  3) 编译告警数（必须保持 0，不能让重构顺手带 warning 回来）
  4) 两版都要过（单版通过=没通过）

用法
----
    python scripts/refactor_guard.py            # 编两版 + 对比基线 + 出结论
    python scripts/refactor_guard.py --fast     # 只检查当前 bin，不重新编译（秒出）

退出码：0 = 通过；1 = 有告警/编译失败；2 = 体积或 DRAM 漂移超阈（要人看）
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_BUILD = os.environ.get('PET_BUILD_BASE', os.path.abspath(os.path.join(ROOT, '..', '..', '_build')))  # ★R217
BASE = {
    # name: (构建脚本, 基线体积, 基线DRAM余KB, bin 路径)
    # ★R157 基线重设（最终）：R156（0x1a7190 / 0x1adaa0）→ R157 实测。
    # R157 = API 双通道改造 + 日常 AI 图 + 测试「推 AI 图」入口（+6 条三语译文）。
    # 文字响应缓冲 2KB→4KB ⇒ DRAM 余量各 -2.0KB；属预期，非越界。
    # 下面两行 = 2026-11-09 R157 最终编译实测。
    # ★R217：bin 路径按仓库相对位置推导，不再写死 C:\epaper
    'key':   ('build2.sh',       0x1a7d30, 105.6, os.path.join(_BUILD, 'base_key', 'pet_epaper_demo.bin')),
    'touch': ('build2_touch.sh', 0x1ae6a0, 104.3, os.path.join(_BUILD, 'base_touch', 'pet_epaper_touch.bin')),
}
# 纯重构允许的体积漂移：超过就得说明为什么（函数边界变化会改变内联与对齐）
TOL_SIZE = 256
TOL_DRAM = 4.0   # KB

RE_SIZE = re.compile(r'固件体积 (0x[0-9a-fA-F]+) bytes')
RE_DRAM = re.compile(r'内部 DRAM：已用 [\d.]+ KB / [\d.]+ KB，余 ([\d.]+) KB')


def run_build(script, fast, binpath):
    if fast:
        return None, ''
    cmd = 'cd /c/epaper/_fw_1025/base_wired && ./%s only 2>&1' % script
    try:
        # ★ 必须按 UTF-8 解码：Windows 上 text=True 会走本地代码页（GBK），
        #   而 git-bash 里的构建脚本输出是 UTF-8，中文全部变乱码 → 下面的中文正则一条都匹配不上。
        r = subprocess.run(['bash', '-lc', cmd], capture_output=True,
                           timeout=1200)
        out = (r.stdout or b'') + (r.stderr or b'')
        return r.returncode, out.decode('utf-8', 'replace')
    except Exception as e:                                    # noqa: BLE001
        return 1, '构建调用失败：%s' % e


def main():
    fast = '--fast' in sys.argv
    problems, warns = [], []
    print('=' * 72)
    for name, (script, base_size, base_dram, binpath) in BASE.items():
        rc, out = run_build(script, fast, binpath)
        if not fast:
            if rc not in (0, None):
                problems.append('%s 版编译失败（rc=%s）' % (name, rc))
                print('  [%s] 编译失败 rc=%s' % (name, rc))
                continue
            m = RE_SIZE.search(out)
            d = RE_DRAM.search(out)
            size = int(m.group(1), 16) if m else None
            dram = float(d.group(1)) if d else None
            bad = [l for l in out.splitlines()
                   if re.search(r'\bwarning:|\berror:', l) and '_fw_1025' in l]
            if bad:
                warns.extend(['%s: %s' % (name, b.strip()[:120]) for b in bad])
        else:
            size = os.path.getsize(binpath) if os.path.exists(binpath) else None
            dram = None

        line = '  [%-5s] ' % name
        if size is None:
            line += '体积读取失败'
            problems.append('%s 版读不到体积' % name)
        else:
            diff = size - base_size
            line += '体积 %#x（基线 %#x，差 %+d B）' % (size, base_size, diff)
            if abs(diff) > TOL_SIZE:
                problems.append('%s 版体积漂移 %+d B > %d B' % (name, diff, TOL_SIZE))
                line += '  ← 超阈'
        if dram is not None:
            dd = dram - base_dram
            line += '  DRAM余 %.1f KB（差 %+.1f）' % (dram, dd)
            if abs(dd) > TOL_DRAM:
                problems.append('%s 版 DRAM 余量变化 %+.1f KB > %.1f KB' % (name, dd, TOL_DRAM))
        print(line)
    print('=' * 72)

    if warns:
        print('编译告警（必须清零）：')
        for w in warns:
            print('   ', w)
        return 1
    if problems:
        print('需要人看：')
        for p in problems:
            print('   -', p)
        return 2
    print('通过：两版体积/DRAM 在容差内、告警 0。注意——这只能证明「没大变」，'
          '上屏像素是否等价仍需逐页回归确认。')
    return 0


if __name__ == '__main__':
    sys.exit(main())
