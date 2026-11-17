#pragma once
/* ---------------------------------------------------------------------------
 * city_place.h —— 城市名 → 英文出图关键词（★R158 新增）
 *
 * 为什么要有它：
 *   免费图源（pollinations/flux）**完全听不懂中文地名**。实测同一模板下
 *   「从上海出发去附近散步…」（中文）生成的是无名峡谷，看不出任何上海元素；
 *   换成 "Shanghai Bund skyline, Oriental Pearl Tower" 立刻出外滩天际线。
 *   ⇒ 出图提示词必须整条英文化，且地名要落到**具体地标/地貌**，只给拼音不够。
 *
 * 三层查找（city_place_en）：
 *   ① 中国城市关键词表（本文件 + net_sync.c 的 CITY_EN[]）：带地标/地貌，
 *      如 上海→"Shanghai Bund skyline, Oriental Pearl Tower, Huangpu river"。
 *      匹配用**子串双向**（配网存的是「河北省石家庄市」这种带省前缀的全名）。
 *   ② 海外城市：走 world_cities_data.h 的 600 条（有 GeoNames 英文名）。
 *   ③ 都没有 → "China, quiet landscape"，宁可是泛化风景，也别生成火山。
 * ------------------------------------------------------------------------- */
void city_place_en(const char *zh, char *out, int cap);

/* ★R168：以用户城市为中心，掷出「这一张明信片寄自哪座城」。
   local_pct = 留在本地的百分比（地标片用 50），其余按**距离分档**抽邻城：
   <150km 权重 1000 / 400km 300 / 900km 110 / 1800km 45 / 更远 18 —— 近的为主，远的也会有。
   海外城市与远行目的地原样返回。
   输出中文短名，喂给 city_place_en() / ai_place_line() / post_brief_make() 同一个值，
   避免「图是杭州、地点行写上海」。 */
void ai_visit_city(const char *home, char *out, int cap, int local_pct);

/* ★R170：拼一条「日常通用场景」英文描述（不含城市地标）—— **组合式**，不是清单。
   场所 16 × 物件 18 × 光线 8 × 细节 10 × 取景 6 = 138240 种，兜底也够玩几百年；
   模型通的时候画面由它现编（见 ws_api_scene），这里只在断网/限流时兜底。
   画面看不出是哪座城，地点行就标用户所在城市。 */
void ai_daily_scene_build(char *out, int cap);
