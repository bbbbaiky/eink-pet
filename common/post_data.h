#ifndef POST_DATA_H
#define POST_DATA_H

/* 重构 2a：post_t 自 main.c 迁入 */
typedef struct {
    const char *title;     /* 列表行标题 */
    const char *place;     /* 正面左下：地点 + 天气 */
    const char *date;      /* 正面右下：MM-DD（背面日戳复用，拆成 MM / DD 两段画） */
    const char *city;      /* ★ 2026-10-01 深夜：背面左上「地点」（纯城市名，见 POST_YR_X） */
    const char *note;      /* 背面正文：一段话（固件折行，见 fb_text_note()） */
    int         photo;     /* post.bin 里的下标 */
} post_t;

/* ★2026-10-25 重构第 1 步：数据表外置（自 main.c 原位剪切，内容零改动）。 */
/* 重构 2d：表定义搬至 post_system.c（extern 见下） */

extern const post_t g_posts[];   /* 明信片总表（定义在 post_system.c） */
#endif
