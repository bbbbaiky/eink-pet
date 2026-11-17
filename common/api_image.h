/* ═══════════════════════════════════════════════════════════════════════════
 * api_image.h — 图像通道核心件 v1（AI 静默生图 → 明信片，2026-10-24 深夜）
 * -----------------------------------------------------------------------------
 * 用户口径：图像 API 的 key 一次性配置（现有 API 配置页），之后设备**全自动**：
 *   远行归来 → 提交生图任务（通义万相 dashscope 异步）→ 轮询结果 → 下载 JPEG
 *   → esp_jpeg 解码 → 缩放 + Bayer 抖动成 164×108 1-bit → 落 mypost 信箱。
 * 全程无手机、无人工干预；失败静默（不影响任何既有流程）。
 * 服务端口径：
 *   · base = NVS api_img_url（缺省 https://dashscope.aliyuncs.com）
 *   · 提交 = POST {base}/api/v1/services/aigc/text2image/image-synthesis
 *            （X-DashScope-Async: enable）
 *   · 轮询 = GET  {base}/api/v1/tasks/{task_id}（3s 一次，最多 30 次）
 *   · key  = NVS api_img_key（api_same_key=1 时用 api_key）
 *   · model = NVS api_img_model（缺省 wanx2.1-t2i-turbo）
 * 产出槽格式 = mypost 0xFF 格式（图 2268B + 字段块），经 sink 回调交给 main.c
 * 的 mypost_add（与好友转发共用 friend_post_recv_cb）。
 * ═══════════════════════════════════════════════════════════════════════════ */
#ifndef API_IMAGE_H
#define API_IMAGE_H

#include <stdbool.h>
#include <stdint.h>

typedef int (*api_image_sink_fn)(const uint8_t *photo, const uint8_t *rest, int restlen, int day);

void api_image_set_sink(api_image_sink_fn fn);   /* main.c 注册一次（= friend_post_recv_cb） */

bool api_image_ready(void);                      /* 图像 key 可用（api_img_key 或 同 key 的 api_key） */

/* ★R166：预制明信片叠猫。post_sprite_pick(seed) 按亲密度档位做**确定性**抽签
   （seed 传卡片 photo 号 ⇒ 同一张卡每次翻页/详情都是同一个结果），返回 CAT_SPRITES
   下标，-1 = 这张是纯风景（不叠）。post_sprite_draw 把该姿态画到 164×108 的 AID_POST
   缓冲区上（约定 1=墨，与 fb_blit1 一致；**不缩放不位移**）。 */
int  post_sprite_pick(uint32_t seed);
void post_sprite_draw(uint8_t *photo, int idx);

/* 生成一张明信片并落箱（阻塞 30~120s：任务+轮询+下载+解码）。
   prompt = 生图提示词；text/title/place = 明信片背面文字/标题/地点（随字段块带过去）。
   返回 true = 已落箱。 */
bool api_image_gen(const char *prompt, const char *text, const char *title, const char *place);

#endif /* API_IMAGE_H */
