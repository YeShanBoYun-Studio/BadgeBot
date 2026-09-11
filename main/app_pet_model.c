// main/app_pet_model.c —— 宠物数值模型:时间衰减、互动动作、成长阶段。
// 不包含任何 ESP-IDF 头文件,tests/test_app_pet.c 在主机上直接编译本文件。
// 设计参照经典黑白像素宠物机(拓麻歌子):数值随时间衰减、便便需要清理、
// 睡觉恢复精力;离线时长按 3 天封顶, neglect 只会让宠物难过而不会死亡。
#include "app_pet.h"
#include <stddef.h>

#define EGG_MIN_MINUTES 2        // 蛋到孵化时长(分钟);很短,让主人尽快见到宠物
#define BABY_MINUTES    (2 * 60)      // 幼年期时长
#define CHILD_MINUTES   (24 * 60)     // 少年期时长

// 衰减速率(每多少分钟掉 1 点);睡觉时饥饿按 2 倍时长衰减,精力转为恢复
#define HUNGER_MIN_AWAKE   15
#define HUNGER_MIN_ASLEEP  30
#define HAPPY_MIN          20
#define CLEAN_MIN          60
#define ENERGY_DRAIN_MIN   30     // 清醒时掉精力
#define ENERGY_GAIN_MIN    8      // 睡觉时回精力
#define POOP_MINUTES       240    // 平均每 4 小时一次便便

#define OFFLINE_CAP_MINUTES (3 * 24 * 60)   // 离线结算上限:3 天
#define STAT_ACC_MAX        60000

static void stat_decay(uint8_t *stat, uint16_t *acc, uint16_t minutes, uint16_t per_min)
{
    if (per_min == 0) return;
    *acc = (uint16_t)(*acc + minutes);
    if (*acc > STAT_ACC_MAX) *acc = STAT_ACC_MAX;
    while (*acc >= per_min && *stat > 0) {
        (*stat)--;
        *acc = (uint16_t)(*acc - per_min);
    }
    if (*stat == 0) *acc = 0;
}

static void stat_gain(uint8_t *stat, uint8_t amount)
{
    int v = *stat + amount;
    *stat = (uint8_t)(v > PET_STAT_MAX ? PET_STAT_MAX : v);
}

void pet_model_init(pet_state_t *s, uint32_t now)
{
    // hatch_epoch 存的是"孵化时刻"= 生蛋时刻 + 蛋期
    s->hatch_epoch = now + (uint32_t)EGG_MIN_MINUTES * 60;
    s->last_tick = now;
    s->age_min = 0;
    s->hunger = 80;
    s->happy = 80;
    s->clean = 100;
    s->energy = 80;
    s->flags = 0;
    s->weight_g = 5;
    s->acc_hunger = s->acc_happy = s->acc_clean = s->acc_energy = s->acc_poop = 0;
    s->acc_sec = 0;
}

void pet_model_tick(pet_state_t *s, uint32_t now)
{
    if (now <= s->last_tick) return;                 // 时钟回拨/未校时:不结算
    uint32_t dt = now - s->last_tick;
    s->last_tick = now;

    // 秒级零头累积成整分钟再结算,高频 tick 也不丢时间
    s->acc_sec += dt;
    uint32_t dt_min = s->acc_sec / 60;
    s->acc_sec %= 60;
    if (dt_min == 0) return;

    bool sleeping = (s->flags & PET_FLAG_ASLERP) != 0;
    // 离线封顶:主人几天不开机,宠物只是"饿瘦了",不会死
    uint16_t capped = dt_min > OFFLINE_CAP_MINUTES ? (uint16_t)OFFLINE_CAP_MINUTES
                                                   : (uint16_t)dt_min;

    if (s->hatch_epoch != UINT32_MAX && now > s->hatch_epoch) {
        uint32_t age = (now - s->hatch_epoch) / 60;  // 孵化后分钟数
        s->age_min = age > 0xFFFF ? 0xFFFF : (uint16_t)age;
    }

    stat_decay(&s->hunger, &s->acc_hunger, capped,
               (uint16_t)(sleeping ? HUNGER_MIN_ASLEEP : HUNGER_MIN_AWAKE));
    stat_decay(&s->happy, &s->acc_happy, capped, HAPPY_MIN);
    stat_decay(&s->clean, &s->acc_clean, capped, CLEAN_MIN);

    if (sleeping) {
        stat_gain(&s->energy, (uint8_t)(capped / ENERGY_GAIN_MIN));
        uint32_t regain_rest = ((uint32_t)s->acc_energy + capped) % ENERGY_GAIN_MIN;
        s->acc_energy = (uint16_t)regain_rest;
    } else {
        stat_decay(&s->energy, &s->acc_energy, capped, ENERGY_DRAIN_MIN);
    }

    // 便便:清醒时按周期出现;已有没有新便便
    if (!sleeping && !(s->flags & PET_FLAG_POOP)) {
        s->acc_poop = (uint16_t)(s->acc_poop + capped);
        if (s->acc_poop > STAT_ACC_MAX) s->acc_poop = STAT_ACC_MAX;
        if (s->acc_poop >= POOP_MINUTES) {
            s->flags |= PET_FLAG_POOP;
            s->acc_poop = 0;
        }
    }

    // 精力耗尽会自己睡着;睡满自然醒
    if (!sleeping && s->energy == 0) s->flags |= PET_FLAG_ASLERP;
    if (sleeping && s->energy >= PET_STAT_MAX) s->flags &= (uint8_t)~PET_FLAG_ASLERP;
}

pet_stage_t pet_model_stage(const pet_state_t *s, uint32_t now)
{
    if (s->hatch_epoch == UINT32_MAX) return PET_STAGE_EGG;
    if (now < s->hatch_epoch) return PET_STAGE_EGG;
    uint32_t age = now - s->hatch_epoch;
    if (age < (uint32_t)BABY_MINUTES * 60) return PET_STAGE_BABY;
    if (age < (uint32_t)CHILD_MINUTES * 60) return PET_STAGE_CHILD;
    return PET_STAGE_ADULT;
}

pet_mood_t pet_model_mood(const pet_state_t *s)
{
    if (s->hatch_epoch == UINT32_MAX) return PET_MOOD_NA;
    if (s->flags & PET_FLAG_ASLERP) return PET_MOOD_ASLEEP;
    if (s->hunger < 30) return PET_MOOD_HUNGRY;
    if (s->energy < 25) return PET_MOOD_SLEEPY;
    if (s->happy < 30 || s->clean < 30) return PET_MOOD_SAD;
    if (s->happy >= 70) return PET_MOOD_HAPPY;
    return PET_MOOD_FINE;
}

bool pet_model_feed(pet_state_t *s, uint32_t now)
{
    (void)now;
    if (s->hatch_epoch == UINT32_MAX || (s->flags & PET_FLAG_ASLERP)) return false;
    stat_gain(&s->hunger, 30);
    stat_gain(&s->happy, 5);
    s->weight_g = (uint16_t)(s->weight_g + 2);
    return true;
}

bool pet_model_play(pet_state_t *s, uint32_t now)
{
    (void)now;
    if (s->hatch_epoch == UINT32_MAX || (s->flags & PET_FLAG_ASLERP)) return false;
    if (s->energy < 10) return false;                // 太累玩不动
    stat_gain(&s->happy, 25);
    s->energy = (uint8_t)(s->energy > 10 ? s->energy - 10 : 0);
    if (s->weight_g > 1) s->weight_g--;
    return true;
}

bool pet_model_clean(pet_state_t *s, uint32_t now)
{
    (void)now;
    if (s->hatch_epoch == UINT32_MAX) return false;
    if (!(s->flags & PET_FLAG_POOP)) return false;   // 没有便便可清
    s->flags &= (uint8_t)~PET_FLAG_POOP;
    s->clean = PET_STAT_MAX;
    return true;
}

bool pet_model_toggle_sleep(pet_state_t *s, uint32_t now)
{
    (void)now;
    if (s->hatch_epoch == UINT32_MAX) return false;
    s->flags ^= PET_FLAG_ASLERP;
    return true;
}
