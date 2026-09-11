// tests/test_app_pet.c —— 宠物数值模型(衰减/动作/阶段)的主机测试。
// 只编译 main/app_pet_model.c,不依赖 ESP-IDF。
#include <assert.h>
#include <string.h>
#include "app_pet.h"

#define H 3600u          // 一小时的秒数

static void test_init_and_egg(void)
{
    pet_state_t s;
    memset(&s, 0xAA, sizeof(s));
    pet_model_init(&s, 1000000);
    assert(s.hatch_epoch == 1000000 + 2 * 60);   // 2 分钟孵化
    assert(pet_model_stage(&s, 1000000) == PET_STAGE_EGG);
    assert(pet_model_stage(&s, s.hatch_epoch - 1) == PET_STAGE_EGG);
    assert(pet_model_stage(&s, s.hatch_epoch) == PET_STAGE_BABY);

    // 蛋期数值不应衰减出负面效果,心情是未孵化前的中性值
    pet_model_tick(&s, 1000000 + 60);
    assert(s.hunger == 80);
}

static void test_decay_and_stage(void)
{
    pet_state_t s;
    pet_model_init(&s, 0);

    // 孵化 3 小时:过了 2 小时幼年期,进入少年期
    uint32_t now = s.hatch_epoch + 3 * H;
    pet_model_tick(&s, now);
    assert(pet_model_stage(&s, now) == PET_STAGE_CHILD);

    // 饱食:180 分钟 / 15 = 12 点
    assert(s.hunger == 80 - 12);
    assert(s.last_tick == now);

    // 时钟回拨不结算
    uint32_t hunger = s.hunger;
    pet_model_tick(&s, now - 600);
    assert(s.hunger == hunger && s.last_tick == now);
}

static void test_offline_cap(void)
{
    pet_state_t s;
    pet_model_init(&s, 0);
    uint32_t now = s.hatch_epoch + 10 * H;

    // 离线 30 天:按 3 天封顶 → 饱食 4320/15=288 点 → 归零但不为负
    pet_model_tick(&s, now + 30u * 24 * H);
    assert(s.hunger == 0);
    assert(s.happy == 0);
    assert(s.weight_g == 5);                     // 只衰减数值,不扣体重
    assert(pet_model_stage(&s, now + 30u * 24 * H) == PET_STAGE_ADULT);
}

static void test_actions(void)
{
    pet_state_t s;
    pet_model_init(&s, 0);
    uint32_t now = s.hatch_epoch + H;            // 幼年,已孵化
    pet_model_tick(&s, now);

    s.flags |= PET_FLAG_ASLERP;                  // 睡着时不能喂/玩
    assert(!pet_model_feed(&s, now));
    assert(!pet_model_play(&s, now));
    s.flags &= (uint8_t)~PET_FLAG_ASLERP;

    // 喂食:饱食 +30(封顶)、体重 +2
    s.hunger = 90;
    assert(pet_model_feed(&s, now));
    assert(s.hunger == PET_STAT_MAX);
    assert(s.weight_g == 5 + 2);

    // 玩耍:心情 +25、精力 -10、体重 -1;精力不足拒绝
    s.happy = 50;
    s.energy = 10;
    assert(pet_model_play(&s, now));
    assert(s.happy == 75);
    assert(s.energy == 0);
    assert(s.weight_g == 6);
    assert(!pet_model_play(&s, now));            // 精力 = 0 < 10

    // 便便:清洁动作只在有便便时生效
    s.flags |= PET_FLAG_POOP;
    s.clean = 40;
    assert(pet_model_clean(&s, now));
    assert(!(s.flags & PET_FLAG_POOP) && s.clean == PET_STAT_MAX);
    assert(!pet_model_clean(&s, now));

    // 睡觉开关
    bool asleep = (s.flags & PET_FLAG_ASLERP) != 0;
    assert(pet_model_toggle_sleep(&s, now));
    assert(((s.flags & PET_FLAG_ASLERP) != 0) != asleep);
}

static void test_sleep_energy_and_poop(void)
{
    pet_state_t s;
    pet_model_init(&s, 0);
    uint32_t now = s.hatch_epoch + H;
    pet_model_tick(&s, now);

    // 睡觉 4 小时:精力 +4h*60/8=30,饥饿按 2 倍时长衰减 4h*60/30=8
    // (先重置数值,隔离上一次 tick 的衰减)
    s.energy = 50;
    s.hunger = 80;
    s.flags |= PET_FLAG_ASLERP;
    pet_model_tick(&s, now + 4 * H);
    assert(s.energy == 50 + 30);
    assert(s.hunger == 80 - 8);

    // 清醒 4 小时无清理:精力 -8,出现便便(240 分钟周期)
    s.flags &= (uint8_t)~PET_FLAG_ASLERP;
    s.energy = 100;
    pet_model_tick(&s, now + 8 * H);
    assert(s.energy == 92);
    assert(s.flags & PET_FLAG_POOP);
}

static void test_mood(void)
{
    pet_state_t s;
    pet_model_init(&s, 0);
    uint32_t now = s.hatch_epoch + H;
    pet_model_tick(&s, now);

    s.happy = 80;
    s.hunger = 80;
    s.energy = 80;
    s.flags = 0;
    assert(pet_model_mood(&s) == PET_MOOD_HAPPY);
    s.hunger = 20;
    assert(pet_model_mood(&s) == PET_MOOD_HUNGRY);
    s.hunger = 80;
    s.energy = 10;
    assert(pet_model_mood(&s) == PET_MOOD_SLEEPY);
    s.energy = 80;
    s.flags |= PET_FLAG_ASLERP;
    assert(pet_model_mood(&s) == PET_MOOD_ASLEEP);
    s.flags = 0;
    s.happy = 10;
    assert(pet_model_mood(&s) == PET_MOOD_SAD);

    s.hatch_epoch = UINT32_MAX;
    assert(pet_model_mood(&s) == PET_MOOD_NA);
}

static void test_games(void)
{
    pet_state_t s;
    pet_model_init(&s, 0);
    s.hatch_epoch = 0;
    s.flags = 0;

    // 精力不足不能开局;够 10 点时扣除
    s.energy = 5;
    assert(!pet_model_game_start(&s));
    assert(s.energy == 5);
    s.energy = 40;
    assert(pet_model_game_start(&s));
    assert(s.energy == 30);

    // 结算:3/5 胜 → 心情 +3*8+2*2 = +28(封顶 100),体重 +3
    s.happy = 60;
    s.weight_g = 10;
    pet_model_game_finish(&s, 3, 5);
    assert(s.happy == 88);
    assert(s.weight_g == 13);

    // 心情封顶 100;胜局数越界按平局裁剪;体重上限无(脏数据由 NVS 层不管)
    s.happy = 95;
    pet_model_game_finish(&s, 7, 5);
    assert(s.happy == 100);
    assert(s.weight_g == 18);              // wins 裁剪为 5

    // 传 0 胜也有安慰奖
    s.happy = 10;
    pet_model_game_finish(&s, 0, 5);
    assert(s.happy == 20);
}

int main(void)
{
    test_init_and_egg();
    test_decay_and_stage();
    test_offline_cap();
    test_actions();
    test_sleep_energy_and_poop();
    test_mood();
    test_games();
    return 0;
}
