/*
 * Copyright (C) 2026 HiHope Open Source Organization .
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* arcade.c -- shared hardware layer, game-selection menu and the main
 * task. The three games live in their own .c files and only talk to
 * the hardware through the API declared in arcade.h. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ohos_init.h"
#include "cmsis_os2.h"
#include "iot_gpio.h"
#include "iot_i2c.h"
#include "iot_pwm.h"
#include "hi_io.h"
#include "hi_adc.h"
#include "hi_errno.h"

#include "ssd1306.h"
#include "arcade.h"

/* -------------------- hardware config -------------------- */
#define OLED_I2C_BAUDRATE   (400 * 1000)

/* R1(S1) / R2(S2) / USER share GPIO5 via resistor divider -> ADC2 */
#define ADC_KEY_CHANNEL     HI_ADC_CHANNEL_2
#define KEY_VLT_USER_MAX    0.35f
#define KEY_VLT_R1_MIN      0.40f
#define KEY_VLT_R1_MAX      0.75f
#define KEY_VLT_R2_MIN      0.80f
#define KEY_VLT_R2_MAX      1.20f

/* passive buzzer GPIO9/PWM0 */
#define BUZZER_GPIO         9
#define BUZZER_PWM_PORT     0
#define BUZZER_DUTY         50

#define LONG_PRESS_MS       500
#define REPEAT_MS           200
#define SOUND_QUEUE_LEN     16
#define POLL_TICKS          2
#define TASK_STACK_SIZE     8192

/* -------------------- internal key state -------------------- */
typedef struct {
    uint8_t  level;
    uint8_t  lastRaw;
    uint8_t  debounce;
    uint8_t  held;
    uint8_t  longFired;
    uint32_t downMs;
    uint32_t lastRepMs;
} Key;

static Key g_keyR1, g_keyR2, g_keyUser;
static uint8_t g_lastRawKey = KEY_NONE;

/* -------------------- sound queue -------------------- */
static Beep g_sndQueue[SOUND_QUEUE_LEN];
static uint8_t  g_sndHead = 0, g_sndTail = 0, g_sndBusy = 0;
static uint32_t g_sndEndMs = 0;

/* -------------------- LED timers -------------------- */
static uint32_t g_greenOffMs = 0, g_redOffMs = 0;

/* ===================== sound ===================== */
static void SoundQueue(uint16_t freq, uint16_t durMs)
{
    uint8_t next = (uint8_t)((g_sndTail + 1) % SOUND_QUEUE_LEN);
    if (next == g_sndHead) return;
    g_sndQueue[g_sndTail].freq = freq;
    g_sndQueue[g_sndTail].durMs = durMs;
    g_sndTail = next;
}
void SoundPlay(const Beep *notes, uint8_t count)
{
    for (uint8_t i = 0; i < count; i++) SoundQueue(notes[i].freq, notes[i].durMs);
}
static void SoundUpdate(uint32_t now)
{
    if (g_sndBusy) {
        if ((int32_t)(now - g_sndEndMs) < 0) return;
        IoTPwmStop(BUZZER_PWM_PORT);
        g_sndBusy = 0;
    }
    if (g_sndHead == g_sndTail) return;
    Beep *b = &g_sndQueue[g_sndHead];
    g_sndHead = (uint8_t)((g_sndHead + 1) % SOUND_QUEUE_LEN);
    if (b->freq > 0) IoTPwmStart(BUZZER_PWM_PORT, BUZZER_DUTY, b->freq);
    g_sndEndMs = now + b->durMs;
    g_sndBusy = 1;
}

/* ===================== LEDs ===================== */
void LedAllOff(void)
{
    IoTGpioSetOutputVal(LED_GREEN_GPIO, IOT_GPIO_VALUE0);
    IoTGpioSetOutputVal(LED_RED_GPIO, IOT_GPIO_VALUE0);
    g_greenOffMs = 0; g_redOffMs = 0;
}
void LedPulse(uint8_t pin, uint32_t durMs, uint32_t now)
{
    uint32_t offAt = now + durMs;
    if (pin == LED_RED_GPIO) {
        IoTGpioSetOutputVal(LED_GREEN_GPIO, IOT_GPIO_VALUE0);
        g_greenOffMs = 0;
        IoTGpioSetOutputVal(LED_RED_GPIO, IOT_GPIO_VALUE1);
        g_redOffMs = offAt;
    } else {
        if (g_redOffMs != 0) return;
        IoTGpioSetOutputVal(LED_GREEN_GPIO, IOT_GPIO_VALUE1);
        g_greenOffMs = offAt;
    }
}
static void LedUpdate(uint32_t now)
{
    if (g_redOffMs != 0 && (int32_t)(now - g_redOffMs) >= 0) {
        IoTGpioSetOutputVal(LED_RED_GPIO, IOT_GPIO_VALUE0); g_redOffMs = 0;
    }
    if (g_greenOffMs != 0 && (int32_t)(now - g_greenOffMs) >= 0) {
        IoTGpioSetOutputVal(LED_GREEN_GPIO, IOT_GPIO_VALUE0); g_greenOffMs = 0;
    }
}

/* ===================== key scanning ===================== */
static uint8_t AdcScanKey(void)
{
    hi_u16 data = 0;
    uint8_t key = KEY_NONE;
    if (hi_adc_read(ADC_KEY_CHANNEL, &data, HI_ADC_EQU_MODEL_4,
                    HI_ADC_CUR_BAIS_DEFAULT, 0) != HI_ERR_SUCCESS) return KEY_NONE;
    float v = (float)data * 1.8f * 4.0f / 4096.0f;
    if (v < KEY_VLT_USER_MAX) key = KEY_USER;
    else if (v >= KEY_VLT_R1_MIN && v <= KEY_VLT_R1_MAX) key = KEY_R1;
    else if (v >= KEY_VLT_R2_MIN && v <= KEY_VLT_R2_MAX) key = KEY_R2;
    if (key != g_lastRawKey) {
        if (key != KEY_NONE) {
            printf("[Arcade] ADC raw=%u, v=%.2fV -> %s\r\n", (unsigned int)data, v,
                   key == KEY_R1 ? "R1" : (key == KEY_R2 ? "R2" : "USER"));
        }
        g_lastRawKey = key;
    }
    return key;
}
static uint8_t KeyUpdate(Key *k, uint8_t pressed, uint32_t now)
{
    uint8_t ev = 0;
    uint8_t raw = pressed ? 0 : 1;
    if (raw != k->lastRaw) { k->debounce = 0; k->lastRaw = raw; }
    else if (k->debounce < 3) k->debounce++;
    if (k->debounce >= 3 && raw != k->level) {
        k->level = raw;
        if (raw == 0) {
            k->held = 1; k->longFired = 0; k->downMs = now; k->lastRepMs = now;
            ev |= EV_PRESS;
        } else {
            if (k->held && !k->longFired) ev |= EV_SHORT;
            k->held = 0;
        }
    }
    if (k->held && !k->longFired && (now - k->downMs) >= LONG_PRESS_MS) {
        k->longFired = 1; k->lastRepMs = now; ev |= EV_LONG;
    }
    if (k->held && k->longFired && (now - k->lastRepMs) >= REPEAT_MS) {
        k->lastRepMs = now; ev |= EV_REPEAT;
    }
    return ev;
}
void KeysScan(uint8_t *evR1, uint8_t *evR2, uint8_t *evUser, uint32_t now)
{
    uint8_t raw = AdcScanKey();
    if (evR1)   *evR1   = KeyUpdate(&g_keyR1,   raw == KEY_R1,   now);
    if (evR2)   *evR2   = KeyUpdate(&g_keyR2,   raw == KEY_R2,   now);
    if (evUser) *evUser = KeyUpdate(&g_keyUser, raw == KEY_USER, now);
}
void KeysReset(void)
{
    Key *keys[] = { &g_keyR1, &g_keyR2, &g_keyUser };
    for (uint8_t i = 0; i < 3; i++) {
        keys[i]->level = 1; keys[i]->lastRaw = 1; keys[i]->debounce = 3;
        keys[i]->held = 0; keys[i]->longFired = 0;
    }
    g_lastRawKey = KEY_NONE;
}

/* ===================== OLED helper ===================== */
void FillRect(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2,
              SSD1306_COLOR color)
{
    for (uint8_t y = y1; y <= y2; y++) ssd1306_DrawLine(x1, y, x2, y, color);
}

/* ===================== MENU ===================== */
static uint8_t g_menuSel = 0;
static const char *g_gameNames[GAME_COUNT] = {
    "MINESWEEPER", "SNAKE", "BREAKOUT"
};

static void MenuRender(void)
{
    ssd1306_Fill(Black);
    ssd1306_SetCursor(3, 0);            /* 11 chars * 11px = 121px, +3 = 124 */
    ssd1306_DrawString("GAME ARCADE", Font_11x18, White);
    for (uint8_t i = 0; i < GAME_COUNT; i++) {
        ssd1306_SetCursor(8, 24 + i * 11);
        char buf[22];
        snprintf(buf, sizeof(buf), "%c %s",
                 (i == g_menuSel) ? '>' : ' ', g_gameNames[i]);
        ssd1306_DrawString(buf, Font_7x10, White);
    }
    ssd1306_SetCursor(2, 64 - 10);
    ssd1306_DrawString("R1:next R2/USER:start", Font_6x8, White);
    ssd1306_UpdateScreen();
}

/* returns GAME_COUNT to stay, otherwise the chosen game id */
static uint8_t MenuUpdate(uint32_t now)
{
    uint8_t ev1, ev2, evU;
    KeysScan(&ev1, &ev2, &evU, now);
    if (ev1 & EV_PRESS) {
        g_menuSel = (g_menuSel + 1) % GAME_COUNT;
        MenuRender();
    }
    if ((ev2 & EV_SHORT) || (evU & EV_SHORT)) {
        return g_menuSel;
    }
    return GAME_COUNT;
}

/* ===================== hardware init ===================== */
static void InitOled(void)
{
    IoTGpioInit(HI_IO_NAME_GPIO_13);
    IoTGpioInit(HI_IO_NAME_GPIO_14);
    hi_io_set_func(HI_IO_NAME_GPIO_13, HI_IO_FUNC_GPIO_13_I2C0_SDA);
    hi_io_set_func(HI_IO_NAME_GPIO_14, HI_IO_FUNC_GPIO_14_I2C0_SCL);
    IoTI2cInit(0, OLED_I2C_BAUDRATE);
    usleep(200 * 1000);
    ssd1306_Init();
}
static void InitKeys(void)
{
    IoTGpioInit(HI_IO_NAME_GPIO_5);
    hi_io_set_func(HI_IO_NAME_GPIO_5, HI_IO_FUNC_GPIO_5_GPIO);
    IoTGpioSetDir(HI_IO_NAME_GPIO_5, IOT_GPIO_DIR_IN);
    KeysReset();
}
static void InitBuzzer(void)
{
    IoTGpioInit(BUZZER_GPIO);
    hi_io_set_func(BUZZER_GPIO, HI_IO_FUNC_GPIO_9_PWM0_OUT);
    IoTGpioSetDir(BUZZER_GPIO, IOT_GPIO_DIR_OUT);
    IoTPwmInit(BUZZER_PWM_PORT);
}
static void InitLeds(void)
{
    IoTGpioInit(LED_RED_GPIO);
    IoTGpioSetDir(LED_RED_GPIO, IOT_GPIO_DIR_OUT);
    IoTGpioInit(LED_GREEN_GPIO);
    IoTGpioSetDir(LED_GREEN_GPIO, IOT_GPIO_DIR_OUT);
    LedAllOff();
}

/* ===================== main task ===================== */
enum {
    STATE_MENU = 0,
    STATE_MINESWEEPER,
    STATE_SNAKE,
    STATE_BREAKOUT,
};

static void StartGame(uint8_t id)
{
    KeysReset();
    if (id == GAME_MINESWEEPER)      { MineInit(); }
    else if (id == GAME_SNAKE)       { SnakeInit(); }
    else if (id == GAME_BREAKOUT)    { BrkInit(); }
    printf("[Arcade] starting game: %s\r\n", g_gameNames[id]);
}

static void ArcadeTask(void)
{
    InitOled();
    InitKeys();
    InitBuzzer();
    InitLeds();
    srand((unsigned int)HAL_GetTick());

    uint8_t state = STATE_MENU;
    MenuRender();

    while (1) {
        uint32_t now = HAL_GetTick();
        uint8_t toMenu = 0;

        if (state == STATE_MENU) {
            uint8_t pick = MenuUpdate(now);
            if (pick < GAME_COUNT) {
                StartGame(pick);
                state = (uint8_t)(STATE_MINESWEEPER + pick);
            }
        } else if (state == STATE_MINESWEEPER) {
            MineUpdate(now, &toMenu);
            MineRender();
        } else if (state == STATE_SNAKE) {
            SnakeUpdate(now, &toMenu);
            SnakeRender();
        } else if (state == STATE_BREAKOUT) {
            BrkUpdate(now, &toMenu);
            BrkRender();
        }

        if (toMenu) {
            state = STATE_MENU;
            KeysReset();
            LedAllOff();
            MenuRender();
        }

        SoundUpdate(now);
        LedUpdate(now);
        osDelay(POLL_TICKS);
    }
}

static void ArcadeEntry(void)
{
    osThreadAttr_t attr;
    attr.name = "ArcadeTask";
    attr.attr_bits = 0U;
    attr.cb_mem = NULL;
    attr.cb_size = 0U;
    attr.stack_mem = NULL;
    attr.stack_size = TASK_STACK_SIZE;
    attr.priority = osPriorityNormal;
    if (osThreadNew((osThreadFunc_t)ArcadeTask, NULL, &attr) == NULL) {
        printf("[Arcade] Failed to create ArcadeTask!\r\n");
    }
}
APP_FEATURE_INIT(ArcadeEntry);
