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

/* -------------------- hardware config -------------------- */
#define OLED_I2C_BAUDRATE   (400 * 1000)

/* The 3 board keys -- R1(S1) and R2(S2) below the OLED display and the
 * USER key on the core board -- share GPIO5 through a resistor divider.
 * GPIO5 is muxed to ADC2 and the keys are told apart by voltage:
 *   no key ~3.23V, USER ~0.20V, R1(S1) ~0.57V, R2(S2) ~0.97V           */
#define ADC_KEY_CHANNEL     HI_ADC_CHANNEL_2
#define KEY_VLT_USER_MAX    0.35f
#define KEY_VLT_R1_MIN      0.40f
#define KEY_VLT_R1_MAX      0.75f
#define KEY_VLT_R2_MIN      0.80f
#define KEY_VLT_R2_MAX      1.20f

/* Passive buzzer: GPIO9/PWM0 (traffic-light / environment board) */
#define BUZZER_GPIO         9
#define BUZZER_PWM_PORT     0
#define BUZZER_DUTY         50

/* -------------------- game config -------------------- */
#define CELL                4    /* pixel size per grid cell               */
#define GRID_W              32   /* 128 / 4 = 32 columns                  */
#define GRID_H              12   /* (64 - 16) / 4 = 12 rows               */
#define GRID_Y              16   /* status bar occupies y 0..15           */
#define GRID_CELLS          (GRID_W * GRID_H)   /* 384                    */
#define MAX_SNAKE           GRID_CELLS

#define TASK_STACK_SIZE     (8192)
#define POLL_TICKS          2    /* 20 ms polling period                  */
#define LONG_PRESS_MS       500
#define BLINK_MS            500
#define WELCOME_SHOW_MS     2000

/* snake speed: start 420ms/move, -5ms per food, min 140ms (gentler ramp) */
#define MOVE_MS_INIT        420
#define MOVE_MS_DECREASE    5
#define MOVE_MS_MIN         140

/* directions: 0=RIGHT, 1=DOWN, 2=LEFT, 3=UP (clockwise) */
enum {
    DIR_RIGHT = 0,
    DIR_DOWN  = 1,
    DIR_LEFT  = 2,
    DIR_UP    = 3,
};
static const int8_t DX[4] = { 1, 0, -1, 0 };
static const int8_t DY[4] = { 0, 1, 0, -1 };

/* virtual key ids decoded from the ADC divider network */
enum {
    KEY_NONE = 0,
    KEY_USER = 1,
    KEY_R1   = 2,   /* S1 below the OLED: turn left               */
    KEY_R2   = 3,   /* S2 below the OLED: turn right / pause     */
};

typedef struct {
    uint8_t  level;
    uint8_t  lastRaw;
    uint8_t  debounce;
    uint8_t  held;
    uint8_t  longFired;
    uint32_t downMs;
    uint32_t lastRepMs;
} Key;

enum {
    EV_PRESS  = 0x01,
    EV_LONG   = 0x02,
    EV_REPEAT = 0x04,
    EV_SHORT  = 0x08,
};

/* -------------------- sound effects (buzzer on PWM0) -------------------- */
typedef struct {
    uint16_t freq;
    uint16_t durMs;
} Beep;

#define SOUND_QUEUE_LEN     16

/* -------------------- game state -------------------- */
static uint8_t  g_snakeX[MAX_SNAKE];
static uint8_t  g_snakeY[MAX_SNAKE];
static uint16_t g_head = 0;       /* index of head in ring buffer       */
static uint16_t g_len  = 0;       /* current snake length              */
static uint8_t  g_dir  = DIR_RIGHT;
#define TURN_QUEUE_LEN  3
static uint8_t  g_turnQueue[TURN_QUEUE_LEN];
static uint8_t  g_turnCount = 0;   /* pending turns in the queue          */
static uint8_t  g_foodX = 0;
static uint8_t  g_foodY = 0;
static uint16_t g_score = 0;
static uint32_t g_moveInterval = MOVE_MS_INIT;
static uint32_t g_lastMoveMs = 0;
static uint8_t  g_over = 0;       /* 0 = playing, 1 = game over         */
static uint8_t  g_paused = 0;
static uint8_t  g_dirty = 1;

static Key g_keyLeft;   /* R1: turn left                              */
static Key g_keyRight;  /* R2: turn right / pause                     */
static Key g_keyUser;   /* USER: restart                              */
static uint8_t g_lastRawKey = KEY_NONE;

static Beep g_sndQueue[SOUND_QUEUE_LEN];
static uint8_t  g_sndHead = 0;
static uint8_t  g_sndTail = 0;
static uint8_t  g_sndBusy = 0;
static uint32_t g_sndEndMs = 0;

/* -------------------- sound (non-blocking beep queue) -------------------- */

static void SoundQueue(uint16_t freq, uint16_t durMs)
{
    uint8_t next = (uint8_t)((g_sndTail + 1) % SOUND_QUEUE_LEN);
    if (next == g_sndHead) {
        return;
    }
    g_sndQueue[g_sndTail].freq = freq;
    g_sndQueue[g_sndTail].durMs = durMs;
    g_sndTail = next;
}

static void SoundPlay(const Beep *notes, uint8_t count)
{
    for (uint8_t i = 0; i < count; i++) {
        SoundQueue(notes[i].freq, notes[i].durMs);
    }
}

static void SoundUpdate(uint32_t now)
{
    if (g_sndBusy) {
        if ((int32_t)(now - g_sndEndMs) < 0) {
            return;
        }
        IoTPwmStop(BUZZER_PWM_PORT);
        g_sndBusy = 0;
    }
    if (g_sndHead == g_sndTail) {
        return;
    }
    Beep *b = &g_sndQueue[g_sndHead];
    g_sndHead = (uint8_t)((g_sndHead + 1) % SOUND_QUEUE_LEN);
    if (b->freq > 0) {
        IoTPwmStart(BUZZER_PWM_PORT, BUZZER_DUTY, b->freq);
    }
    g_sndEndMs = now + b->durMs;
    g_sndBusy = 1;
}

/* eat / crash / start / pause */
static const Beep g_sndEat[]   = { { 1319, 60 }, { 0, 30 }, { 1568, 80 } };
static const Beep g_sndCrash[] = { { 220, 200 }, { 0, 40 }, { 165, 400 } };
static const Beep g_sndStart[] = { { 659, 80 }, { 0, 40 }, { 880, 80 },
                                   { 0, 40 }, { 1047, 140 } };
static const Beep g_sndPause[] = { { 880, 60 } };

/* -------------------- game logic -------------------- */

static void NewGame(void)
{
    /* start in the centre, length 3, heading right */
    uint8_t sx = GRID_W / 2;
    uint8_t sy = GRID_H / 2;
    g_head = 0;
    g_len = 3;
    for (uint16_t i = 0; i < g_len; i++) {
        uint16_t idx = (g_head - i + MAX_SNAKE) % MAX_SNAKE;
        g_snakeX[idx] = sx - (uint8_t)i;
        g_snakeY[idx] = sy;
    }
    g_dir = DIR_RIGHT;
    g_turnCount = 0;
    g_score = 0;
    g_moveInterval = MOVE_MS_INIT;
    g_over = 0;
    g_paused = 0;
    g_dirty = 1;
    SoundPlay(g_sndStart, sizeof(g_sndStart) / sizeof(g_sndStart[0]));
    printf("[Snake] New game: %dx%d grid.\r\n", GRID_W, GRID_H);
}

/* Pick a random empty cell for food. */
static void SpawnFood(void)
{
    for (int tries = 0; tries < 500; tries++) {
        uint8_t fx = (uint8_t)(rand() % GRID_W);
        uint8_t fy = (uint8_t)(rand() % GRID_H);
        uint8_t onSnake = 0;
        for (uint16_t i = 0; i < g_len; i++) {
            uint16_t idx = (g_head - i + MAX_SNAKE) % MAX_SNAKE;
            if (g_snakeX[idx] == fx && g_snakeY[idx] == fy) {
                onSnake = 1;
                break;
            }
        }
        if (!onSnake) {
            g_foodX = fx;
            g_foodY = fy;
            return;
        }
    }
    /* board full → win */
    g_foodX = 0;
    g_foodY = 0;
}

/* Advance the snake one step. Returns 1 on collision (game over). */
static uint8_t MoveSnake(void)
{
    uint8_t hx = g_snakeX[g_head];
    uint8_t hy = g_snakeY[g_head];
    uint8_t nx = (uint8_t)(hx + DX[g_dir]);
    uint8_t ny = (uint8_t)(hy + DY[g_dir]);

    /* wall collision */
    if (nx >= GRID_W || ny >= GRID_H) {
        return 1;
    }

    /* self collision: check all segments except the tail
     * (tail moves away unless we're eating) */
    uint8_t eating = (nx == g_foodX && ny == g_foodY);
    uint16_t checkLen = g_len;
    if (!eating && g_len > 1) {
        checkLen = g_len - 1;   /* skip the tail */
    }
    for (uint16_t i = 0; i < checkLen; i++) {
        uint16_t idx = (g_head - i + MAX_SNAKE) % MAX_SNAKE;
        if (g_snakeX[idx] == nx && g_snakeY[idx] == ny) {
            return 1;
        }
    }

    /* advance head */
    g_head = (uint16_t)((g_head + 1) % MAX_SNAKE);
    g_snakeX[g_head] = nx;
    g_snakeY[g_head] = ny;

    if (eating) {
        g_len++;
        g_score++;
        if (g_moveInterval > MOVE_MS_MIN) {
            g_moveInterval -= MOVE_MS_DECREASE;
            if (g_moveInterval < MOVE_MS_MIN) {
                g_moveInterval = MOVE_MS_MIN;
            }
        }
        SoundPlay(g_sndEat, sizeof(g_sndEat) / sizeof(g_sndEat[0]));
        printf("[Snake] Eat! score=%d, len=%d, speed=%ums\r\n",
               g_score, g_len, g_moveInterval);
        if (g_len >= MAX_SNAKE) {
            g_over = 1;
            printf("[Snake] You win! Board full.\r\n");
            return 0;
        }
        SpawnFood();
    }
    return 0;
}

/* -------------------- key scanning (ADC divider, debounced) -------------------- */

static uint8_t AdcScanKey(void)
{
    hi_u16 data = 0;
    uint8_t key = KEY_NONE;

    if (hi_adc_read(ADC_KEY_CHANNEL, &data, HI_ADC_EQU_MODEL_4,
                    HI_ADC_CUR_BAIS_DEFAULT, 0) != HI_ERR_SUCCESS) {
        return KEY_NONE;
    }

    float v = (float)data * 1.8f * 4.0f / 4096.0f;
    if (v < KEY_VLT_USER_MAX) {
        key = KEY_USER;
    } else if (v >= KEY_VLT_R1_MIN && v <= KEY_VLT_R1_MAX) {
        key = KEY_R1;
    } else if (v >= KEY_VLT_R2_MIN && v <= KEY_VLT_R2_MAX) {
        key = KEY_R2;
    }

    if (key != g_lastRawKey) {
        if (key != KEY_NONE) {
            printf("[Snake] ADC raw=%u, v=%.2fV -> %s\r\n",
                   (unsigned int)data, v,
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

    if (raw != k->lastRaw) {
        k->debounce = 0;
        k->lastRaw = raw;
    } else if (k->debounce < 3) {
        k->debounce++;
    }

    if (k->debounce >= 3 && raw != k->level) {
        k->level = raw;
        if (raw == 0) {
            k->held = 1;
            k->longFired = 0;
            k->downMs = now;
            k->lastRepMs = now;
            ev |= EV_PRESS;
        } else {
            if (k->held && !k->longFired) {
                ev |= EV_SHORT;
            }
            k->held = 0;
        }
    }

    if (k->held && !k->longFired && (now - k->downMs) >= LONG_PRESS_MS) {
        k->longFired = 1;
        k->lastRepMs = now;
        ev |= EV_LONG;
    }
    if (k->held && k->longFired && (now - k->lastRepMs) >= 200) {
        k->lastRepMs = now;
        ev |= EV_REPEAT;
    }
    return ev;
}

static void HandleKeys(uint32_t now)
{
    uint8_t rawKey = AdcScanKey();
    uint8_t evL = KeyUpdate(&g_keyLeft,  rawKey == KEY_R1, now);
    uint8_t evR = KeyUpdate(&g_keyRight, rawKey == KEY_R2, now);
    uint8_t evU = KeyUpdate(&g_keyUser,  rawKey == KEY_USER, now);

    if (g_over) {
        if ((evL & EV_SHORT) || (evR & EV_SHORT) || (evU & EV_SHORT)) {
            NewGame();
            g_lastMoveMs = now;
        }
        return;
    }

    /* R2 long press = pause / resume */
    if (evR & EV_LONG) {
        g_paused = !g_paused;
        g_dirty = 1;
        SoundPlay(g_sndPause, sizeof(g_sndPause) / sizeof(g_sndPause[0]));
        printf("[Snake] %s\r\n", g_paused ? "Paused" : "Resumed");
        if (!g_paused) {
            g_lastMoveMs = now;   /* resume timing cleanly */
        }
        return;
    }

    if (g_paused) {
        return;   /* ignore turn keys while paused */
    }

    /* R1 = turn left (CCW), R2 = turn right (CW).
     * Queue up to TURN_QUEUE_LEN turns so rapid taps between move steps
     * are not lost. Each new turn is validated against the last queued
     * direction (or current g_dir) to forbid 180 degree reversal. */
    if (g_turnCount < TURN_QUEUE_LEN) {
        uint8_t base = (g_turnCount > 0)
                       ? g_turnQueue[g_turnCount - 1]
                       : g_dir;
        if (evL & EV_PRESS) {
            uint8_t nd = (uint8_t)((base + 3) % 4);
            if (nd != (uint8_t)((base + 2) % 4)) {
                g_turnQueue[g_turnCount++] = nd;
            }
        } else if (evR & EV_PRESS) {
            uint8_t nd = (uint8_t)((base + 1) % 4);
            if (nd != (uint8_t)((base + 2) % 4)) {
                g_turnQueue[g_turnCount++] = nd;
            }
        }
    }
}

/* -------------------- OLED rendering -------------------- */

static void FillRect(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2,
                     SSD1306_COLOR color)
{
    for (uint8_t y = y1; y <= y2; y++) {
        ssd1306_DrawLine(x1, y, x2, y, color);
    }
}

static void DrawSnake(void)
{
    for (uint16_t i = 0; i < g_len; i++) {
        uint16_t idx = (g_head - i + MAX_SNAKE) % MAX_SNAKE;
        uint8_t px = g_snakeX[idx] * CELL;
        uint8_t py = GRID_Y + g_snakeY[idx] * CELL;
        if (i == 0) {
            /* head: full cell (4x4) */
            FillRect(px, py, px + CELL - 1, py + CELL - 1, White);
        } else {
            /* body: 3x3 with 1px gap */
            FillRect(px, py, px + CELL - 2, py + CELL - 2, White);
        }
    }
}

static void DrawFood(void)
{
    uint8_t px = g_foodX * CELL;
    uint8_t py = GRID_Y + g_foodY * CELL;
    /* food: 2x2 center dot, visually smaller than the 3x3 body blocks */
    FillRect(px + 1, py + 1, px + CELL - 2, py + CELL - 2, White);
}

static void DrawStatus(void)
{
    char buf[24];

    snprintf(buf, sizeof(buf), "S:%02d L:%02d", g_score, g_len);
    ssd1306_SetCursor(0, 2);
    ssd1306_DrawString(buf, Font_7x10, White);

    if (g_over) {
        ssd1306_SetCursor(90, 2);
        ssd1306_DrawString("OVER", Font_7x10, White);
    } else if (g_paused) {
        ssd1306_SetCursor(90, 2);
        ssd1306_DrawString("PAUSE", Font_7x10, White);
    } else {
        snprintf(buf, sizeof(buf), "%3ums", g_moveInterval);
        ssd1306_SetCursor(92, 2);
        ssd1306_DrawString(buf, Font_7x10, White);
    }

    ssd1306_DrawLine(0, GRID_Y - 1, SSD1306_WIDTH - 1, GRID_Y - 1, White);
}

static void Render(void)
{
    ssd1306_Fill(Black);
    DrawStatus();
    DrawFood();
    DrawSnake();
    ssd1306_UpdateScreen();
}

/* -------------------- task / init -------------------- */

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

    Key *keys[] = { &g_keyLeft, &g_keyRight, &g_keyUser };
    for (uint8_t i = 0; i < 3; i++) {
        keys[i]->level = 1;
        keys[i]->lastRaw = 1;
        keys[i]->debounce = 3;
        keys[i]->held = 0;
        keys[i]->longFired = 0;
    }
}

static void InitBuzzer(void)
{
    IoTGpioInit(BUZZER_GPIO);
    hi_io_set_func(BUZZER_GPIO, HI_IO_FUNC_GPIO_9_PWM0_OUT);
    IoTGpioSetDir(BUZZER_GPIO, IOT_GPIO_DIR_OUT);
    IoTPwmInit(BUZZER_PWM_PORT);
}

static void ShowWelcome(void)
{
    ssd1306_Fill(Black);
    ssd1306_SetCursor(28, 0);
    ssd1306_DrawString("SNAKE", Font_11x18, White);
    ssd1306_SetCursor(4, 24);
    ssd1306_DrawString("R1: turn left", Font_7x10, White);
    ssd1306_SetCursor(4, 36);
    ssd1306_DrawString("R2: turn right", Font_7x10, White);
    ssd1306_SetCursor(4, 48);
    ssd1306_DrawString("R2 hold: pause", Font_7x10, White);
    ssd1306_UpdateScreen();
    printf("[Snake] Keys share GPIO5/ADC2 via resistor divider:\r\n");
    printf("[Snake]   R1(S1) ~0.57V, R2(S2) ~0.97V, USER ~0.20V, none ~3.23V.\r\n");
    printf("[Snake] R1 = turn left, R2 = turn right, R2 hold = pause.\r\n");
}

static void SnakeTask(void)
{
    InitOled();
    InitKeys();
    InitBuzzer();

    srand((unsigned int)HAL_GetTick());
    NewGame();
    SpawnFood();
    ShowWelcome();
    HAL_Delay(WELCOME_SHOW_MS);
    g_dirty = 1;
    g_lastMoveMs = HAL_GetTick();

    while (1) {
        uint32_t now = HAL_GetTick();
        HandleKeys(now);
        SoundUpdate(now);

        /* advance the snake at the current speed */
        if (!g_over && !g_paused) {
            if ((now - g_lastMoveMs) >= g_moveInterval) {
                g_lastMoveMs = now;
                /* pop one pending turn (already validated against 180°) */
                if (g_turnCount > 0) {
                    g_dir = g_turnQueue[0];
                    for (uint8_t i = 1; i < g_turnCount; i++) {
                        g_turnQueue[i - 1] = g_turnQueue[i];
                    }
                    g_turnCount--;
                }
                if (MoveSnake()) {
                    g_over = 1;
                    SoundPlay(g_sndCrash,
                              sizeof(g_sndCrash) / sizeof(g_sndCrash[0]));
                    printf("[Snake] Game over! score=%d, len=%d\r\n",
                           g_score, g_len);
                }
                g_dirty = 1;
            }
        }

        if (g_dirty) {
            Render();
            g_dirty = 0;
        }
        osDelay(POLL_TICKS);
    }
}

static void SnakeEntry(void)
{
    osThreadAttr_t attr;

    attr.name = "SnakeTask";
    attr.attr_bits = 0U;
    attr.cb_mem = NULL;
    attr.cb_size = 0U;
    attr.stack_mem = NULL;
    attr.stack_size = TASK_STACK_SIZE;
    attr.priority = osPriorityNormal;

    if (osThreadNew((osThreadFunc_t)SnakeTask, NULL, &attr) == NULL) {
        printf("[Snake] Failed to create SnakeTask!\r\n");
    }
}
APP_FEATURE_INIT(SnakeEntry);
