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

/* arcade.h -- shared definitions and hardware abstraction for the
 * multi-game cartridge. Game modules include only this header (plus
 * ssd1306.h) and never touch GPIO / ADC / PWM directly. */

#ifndef ARCADE_H
#define ARCADE_H

#include <stdint.h>
#include "ssd1306.h"

/* -------------------- key events -------------------- */
enum {
    KEY_NONE = 0,
    KEY_USER = 1,
    KEY_R1   = 2,
    KEY_R2   = 3,
};

enum {
    EV_PRESS  = 0x01,   /* key just pressed                 */
    EV_LONG   = 0x02,   /* held over LONG_PRESS_MS (once)   */
    EV_REPEAT = 0x04,   /* still held after long (repeat)   */
    EV_SHORT  = 0x08,   /* released before long             */
};

/* -------------------- sound -------------------- */
typedef struct { uint16_t freq; uint16_t durMs; } Beep;

/* -------------------- LEDs -------------------- */
#define LED_RED_GPIO        10
#define LED_GREEN_GPIO      11

/* -------------------- game ids -------------------- */
enum {
    GAME_MINESWEEPER = 0,
    GAME_SNAKE,
    GAME_BREAKOUT,
    GAME_COUNT,
};

/* -------------------- shared API (arcade.c) -------------------- */

/* Scan the ADC divider keys once and return the debounced event
 * bit-masks for R1, R2 and USER. Must be called once per tick. */
void KeysScan(uint8_t *evR1, uint8_t *evR2, uint8_t *evUser, uint32_t now);

/* Reset internal key state (call when switching between screens). */
void KeysReset(void);

/* Non-blocking buzzer: enqueue a sequence of notes (freq 0 = silence). */
void SoundPlay(const Beep *notes, uint8_t count);

/* Indicator LEDs (active high). LedPulse fires a timed pulse; red has
 * priority over green. */
void LedAllOff(void);
void LedPulse(uint8_t pin, uint32_t durMs, uint32_t now);

/* OLED helper: filled rectangle. */
void FillRect(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2,
              SSD1306_COLOR color);

/* -------------------- game module interface --------------------
 * Each game implements these three functions. Update sets *toMenu=1
 * to request returning to the selection menu. */
void MineInit(void);
void MineUpdate(uint32_t now, uint8_t *toMenu);
void MineRender(void);

void SnakeInit(void);
void SnakeUpdate(uint32_t now, uint8_t *toMenu);
void SnakeRender(void);

void BrkInit(void);
void BrkUpdate(uint32_t now, uint8_t *toMenu);
void BrkRender(void);

#endif /* ARCADE_H */
