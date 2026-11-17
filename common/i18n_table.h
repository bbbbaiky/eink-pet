#pragma once

/* i18n translation table TYPE-DEFINITION file.
 * 2026-10-20: translation data (EN_TABLE/JA_TABLE/ZHT_TABLE) moved to
 *   common/i18n_table_data.h and built into common/i18n_asset.bin (assets I18N region)
 *   by scripts/_tools/gen_i18n_asset.py. This file no longer holds data, so it does
 *   not cost firmware rodata; runtime t_() now reads from the assets partition (main.c).
 * i18n_table_data.h is parsed by the generator ONLY -- never #include it in any .c.
 * Edit flow: edit i18n_table_data.h -> gen_i18n_asset.py -> pack_assets.py --write.
 */
typedef struct { const char *zh; const char *tr; } i18n_ent_t;
