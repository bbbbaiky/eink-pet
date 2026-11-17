/* 本文件由 _preview/gen_zfont.py 自动生成，别手改。 */
/* ★ 2026-10-01 深夜（用户反馈③「文本里出现字库缺字导致方块」）——**例外补字**，非手改：
   标题档补入 檐/漪/舫/铛/馍 5 字（明信片正文与收藏品名用字，原表没有 → 上屏是空方框）。
   补字脚本 = 会话目录 gen_zfont_patch.py，用工程内 fonts/ZLabsPixel_12px_M_CN.ttf
   以 size=12 / 落点(0,0) / 阈值100 渲染；脚本先拿表内抽样 40 字做**逐位复现自检**
   （40/40 一致才允许写入），因此新字与既有字形同源、行框对齐一致。
   正文档（BODY 表）**未补**：该表标定不收敛（表内存在单像素伪影，疑为缩放渲染产物），
   本批唯一落在正文档的缺字「黏」已通过改文案规避（见 main.c g_loot 铃铛说明）。 */
/* 固件端运行时字形表：两套点阵字体的**最小子集**（不是字体库）。
   正文档 = 精品点阵体9x9@10px（比例 ASCII + 10px 等宽汉字）
   标题档 = Z Labs 像素12px（等宽 ASCII，前进 6）
   表语义：每字一个「墨迹格」，行 0 = 全表所有字的最小墨迹行；
           落屏时把**本串第一个非空行**对齐到调用方给的 y（见 fb_text_c）。
           每行 1 字节（汉字 2 字节），bit7 = 最左列，1 = 墨。 */
#pragma once
#include <stdint.h>

#define ZF_ASCII_LO 0x20
#define ZF_ASCII_HI 0x7E
#define ZF_ASCII_N  (ZF_ASCII_HI - ZF_ASCII_LO + 1)

/* ---- 正文档（精品点阵体9x9 @10px）：比例 ASCII ---- */
#define ZF_BODY_H    10
#define ZF_BODY_W    8   /* ASCII 格宽（位） */
#define ZF_BODY_RB   ((ZF_BODY_W + 7) / 8)
typedef struct { uint8_t adv; uint8_t b[ZF_BODY_H * ZF_BODY_RB]; } zf_body_t;


/* ---- 正文档：汉字全表（6764 字 = GB2312 全 6763 + …，格宽 10 位 / 前进 10，与 ASCII 共用 10 行）---- */
#define ZF_CJK_W    10   /* 格宽（位），≤ 10 位 → 每行 2 字节 */
#define ZF_CJK_RB   ((ZF_CJK_W + 7) / 8)
#define ZF_CJK_ADV  10   /* ★ 前进宽度 ≠ 格宽，别混用（标题档两者就不相等） */
typedef struct { uint32_t cp; uint8_t b[ZF_BODY_H * ZF_CJK_RB]; } zf_cjk_t;

#define ZF_BODY_CJK_N 12306u /* ★R154：补 3 字（豐銭錶），与 zfont.bin 字数同步；check_zfont_glyphs.py 会自动校验 */ /* ★R107(2026-10-28) 字库裁剪：12023→5107（源码用字+GB2312一级兜底），zfont.bin 723KB→308KB */   /* ★2026-10-16 拆分：表已搬 assets 分区（zfont.bin），此处只留计数（定 RAM 索引数组大小）*/

/* ---- 标题档（Z Labs 像素12px）：等宽 ASCII，前进恒为 6 ---- */
#define ZF_TITLE_H    12
#define ZF_TITLE_W    6
#define ZF_TITLE_RB   ((ZF_TITLE_W + 7) / 8)
#define ZF_TITLE_ADV  6


/* ---- 标题档：汉字全表（6764 字 = GB2312 全 6763 + …，格宽 12 位 / 前进 12）----
   ★ 2026-09-22 新增：WiFi 管理页 / 连接页的网名要能画中文，
     标题档原来只有 ASCII → 中文网名会整串变空心方框。 */
#define ZF_TITLE_CJK_W    12
#define ZF_TITLE_CJK_RB   ((ZF_TITLE_CJK_W + 7) / 8)
#define ZF_TITLE_CJK_ADV  12
typedef struct { uint32_t cp; uint8_t b[ZF_TITLE_H * ZF_TITLE_CJK_RB]; } zf_tcjk_t;

#define ZF_TITLE_CJK_N 12306u   /* ★2026-10-16 拆分：同上 */
