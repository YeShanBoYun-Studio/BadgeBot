// main/app_pet.h —— 像素宠物(拓麻歌子式养成):状态、衰减结算与互动动作。
//
// 分两层,便于主机测试纯逻辑:
//   app_pet_model.c  数值衰减/动作/成长阶段 —— 不依赖 ESP-IDF
//   app_pet.c        NVS 持久化与时间源 —— 仅目标端
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PET_STAT_MAX 100

// 成长阶段:蛋 → 幼年 → 少年 → 成年(按孵化后时长推进)
typedef enum {
    PET_STAGE_EGG = 0,
    PET_STAGE_BABY,
    PET_STAGE_CHILD,
    PET_STAGE_ADULT,
} pet_stage_t;

// 心情(展示用,由数值推导)
typedef enum {
    PET_MOOD_ASLEEP,     // 睡着了
    PET_MOOD_HUNGRY,     // 饿了
    PET_MOOD_SLEEPY,     // 困了
    PET_MOOD_SAD,        // 难过
    PET_MOOD_HAPPY,      // 开心
    PET_MOOD_FINE,       // 还不错
    PET_MOOD_NA,         // 未初始化/无数据
} pet_mood_t;

// flags 位
#define PET_FLAG_POOP   0x01   // 需要清理
#define PET_FLAG_ASLERP 0x02   // 睡觉中

typedef struct {
    uint32_t hatch_epoch;    // 蛋破壳时刻(epoch 秒);UINT32_MAX = 尚未开始养
    uint32_t last_tick;      // 上次结算时刻(epoch 秒)
    uint16_t age_min;        // 孵化后经过的分钟数(tick 结算累加,离线也计入)
    uint8_t hunger;          // 饱食 0..100
    uint8_t happy;           // 心情 0..100
    uint8_t clean;           // 清洁 0..100
    uint8_t energy;          // 精力 0..100
    uint8_t flags;           // PET_FLAG_*
    uint16_t weight_g;       // 体重(克)
    // 衰减零头累积:秒级累加,凑满整分钟才扣数值,避免小步长丢失
    uint32_t acc_sec;
    uint16_t acc_hunger, acc_happy, acc_clean, acc_energy, acc_poop;
} pet_state_t;

// ---- 纯逻辑(app_pet_model.c) ----

// 首次开始养:生一颗蛋,EGG_MIN_MINUTES 后孵化。
void pet_model_init(pet_state_t *s, uint32_t now);

// 结算衰减/便便/孵化时长;now 必须是有效 epoch(已校时)。离线时长按 3 天封顶。
void pet_model_tick(pet_state_t *s, uint32_t now);

pet_stage_t pet_model_stage(const pet_state_t *s, uint32_t now);
pet_mood_t pet_model_mood(const pet_state_t *s);

// 互动动作;返回是否生效(如睡着时不能玩耍)。
bool pet_model_feed(pet_state_t *s, uint32_t now);
bool pet_model_play(pet_state_t *s, uint32_t now);
bool pet_model_clean(pet_state_t *s, uint32_t now);
bool pet_model_toggle_sleep(pet_state_t *s, uint32_t now);

// 小游戏:开局扣 10 点精力(不足/睡着/未出生返回 false);
// 结束按胜局结算心情(胜 +8 / 负 +2,封顶 100)与体重(每胜 +1g)。
bool pet_model_game_start(pet_state_t *s);
void pet_model_game_finish(pet_state_t *s, int wins, int rounds);

// ---- 目标端(app_pet.c) ----
#ifdef ESP_PLATFORM
#include "esp_err.h"
// 从 NVS 载入宠物状态;首次开机不初始化(等第一次进宠物页且校时后生蛋)。
void app_pet_load(void);
const pet_state_t *app_pet_get(void);          // 永不为 NULL;未初始化时 hatch=UINT32_MAX
pet_state_t *app_pet_mut(void);                // 可变访问(动作/结算用),写完记得 app_pet_save()
bool app_pet_initialized(void);
// 生蛋并持久化(仅在 !app_pet_initialized() 时有效)。
void app_pet_birth(uint32_t now);
void app_pet_save(void);                       // 把当前状态写回 NVS
void app_pet_reset(void);                      // 清空宠物(内存;NVS 由恢复出厂统一擦除)
#endif
