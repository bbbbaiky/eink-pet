# -*- coding: utf-8 -*-
"""★R217：项目路径统一解析 —— 脚本里不再写死 C:/epaper。

优先级：**环境变量 > 相对仓库推导 > 约定默认值**。
  PET_INBOX       素材目录（用户投放区，默认 <epaper 根>/inbox）
  PET_BUILD_BASE  构建产物根目录（默认 <epaper 根>/_build）
  PET_TMP         临时/预览输出（默认系统 temp）
  PET_IDF_PY      IDF 自带 python（shell 侧 find_idf_py.sh 用）

布局假定（换电脑只要保持相对关系即可）：
    <epaper 根>/<工作区>/<本仓库>/scripts/_tools/pet_paths.py
    <epaper 根>/inbox/…                 ← 素材
    <epaper 根>/_build/…                ← 编译产物（可再生）
"""
from __future__ import annotations

import os
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]          # .../base_wired


def inbox() -> Path:
    """素材目录：环境变量优先，其次按仓库相对位置找已存在的那个。"""
    env = os.environ.get("PET_INBOX")
    if env:
        return Path(env)
    for c in (REPO.parent.parent / "inbox", REPO.parent / "inbox", REPO / "inbox"):
        if c.is_dir():
            return c
    return REPO.parent.parent / "inbox"            # 约定位置；调用方按需报缺文件


def build_base() -> Path:
    """构建产物根目录（不含 base_key / base_touch 子目录名）。"""
    env = os.environ.get("PET_BUILD_BASE")
    if env:
        return Path(env)
    for c in (REPO.parent.parent / "_build", REPO.parent / "_build", REPO / "_build"):
        if c.exists():
            return c
    return REPO.parent.parent / "_build"


def assets_dir() -> Path:
    return REPO / "fw_epaper_demo" / "main" / "assets"


def tmp_dir() -> Path:
    return Path(os.environ.get("PET_TMP", tempfile.gettempdir()))
