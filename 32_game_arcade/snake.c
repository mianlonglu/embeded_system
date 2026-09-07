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

/* snake.c -- the Snake game module.
 * Controls: R1 = turn left, R2 = turn right, R2 hold = pause.
 * Speed: starts 420ms/step, -5ms per food, min 140ms. */

#include <stdio.h>
#include <stdlib.h>

#include "ssd1306.h"
#include "arcade.h"

#define SNAKE_CELL          4
#define SNAKE_W             32
#define SNAKE_H             12
#define SNAKE_MAX           (SNAKE_W * SNAKE_H)
#define SNAKE_GRID_Y        16
#define SNAKE_MOVE_INIT     420
#define SNAKE_MOVE_DEC      5
#define SNAKE_MOVE_MIN      140
#define SNAKE_TURN_QLEN     3

enum { DIR_RIGHT=0, DIR_DOWN=1, DIR_LEFT=2, DIR_UP=3 };
static const int8_t s_dx[4] = { 1, 0, -1, 0 };
static const int8_t s_dy[4] = { 0, 1, 0, -1 };

static uint8_t  s_x[SNAKE_MAX], s_y[SNAKE_MAX];
static uint16_t s_head, s_len;
static uint8_t  s_dir, s_tq[SNAKE_TURN_QLEN], s_tcnt;
static uint8_t  s_fx, s_fy, s_over, s_paused, s_dirty;
static uint16_t s_score;
static uint32_t s_interval, s_lastMove;

static const Beep sk_eat[]   = { { 1319, 60 }, { 0, 30 }, { 1568, 80 } };
static const Beep sk_crash[] = { { 220, 200 }, { 0, 40 }, { 165, 400 } };
static const Beep sk_start[] = { { 659, 80 }, { 0, 40 }, { 880, 80 }, { 0, 40 }, { 1047, 140 } };
static const Beep sk_pause[] = { { 880, 60 } };

static void SnakeSpawnFood(void)
{
    for (int t = 0; t < 500; t++) {
        uint8_t fx = rand() % SNAKE_W, fy = rand() % SNAKE_H;
        uint8_t on = 0;
        for (uint16_t i = 0; i < s_len; i++) {
            uint16_t idx = (s_head - i + SNAKE_MAX) % SNAKE_MAX;
            if (s_x[idx] == fx && s_y[idx] == fy) { on = 1; break; }
        }
        if (!on) { s_fx = fx; s_fy = fy; return; }
    }
    s_fx = 0; s_fy = 0;
}

void SnakeInit(void)
{
    uint8_t sx = SNAKE_W / 2, sy = SNAKE_H / 2;
    s_head = 0; s_len = 3;
    for (uint16_t i = 0; i < s_len; i++) {
        uint16_t idx = (s_head - i + SNAKE_MAX) % SNAKE_MAX;
        s_x[idx] = sx - (uint8_t)i; s_y[idx] = sy;
    }
    s_dir = DIR_RIGHT; s_tcnt = 0; s_score = 0;
    s_interval = SNAKE_MOVE_INIT; s_over = 0; s_paused = 0; s_dirty = 1;
    SnakeSpawnFood();
    s_lastMove = 0;   /* first move waits one full interval */
    SoundPlay(sk_start, sizeof(sk_start)/sizeof(sk_start[0]));
}

static uint8_t SnakeMove(void)
{
    uint8_t hx = s_x[s_head], hy = s_y[s_head];
    uint8_t nx = (uint8_t)(hx + s_dx[s_dir]), ny = (uint8_t)(hy + s_dy[s_dir]);
    if (nx >= SNAKE_W || ny >= SNAKE_H) return 1;
    uint8_t eating = (nx == s_fx && ny == s_fy);
    uint16_t checkLen = s_len;
    if (!eating && s_len > 1) checkLen = s_len - 1;
    for (uint16_t i = 0; i < checkLen; i++) {
        uint16_t idx = (s_head - i + SNAKE_MAX) % SNAKE_MAX;
        if (s_x[idx] == nx && s_y[idx] == ny) return 1;
    }
    s_head = (uint16_t)((s_head + 1) % SNAKE_MAX);
    s_x[s_head] = nx; s_y[s_head] = ny;
    if (eating) {
        s_len++; s_score++;
        if (s_interval > SNAKE_MOVE_MIN) {
            s_interval -= SNAKE_MOVE_DEC;
            if (s_interval < SNAKE_MOVE_MIN) s_interval = SNAKE_MOVE_MIN;
        }
        SoundPlay(sk_eat, sizeof(sk_eat)/sizeof(sk_eat[0]));
        if (s_len >= SNAKE_MAX) { s_over = 1; return 0; }
        SnakeSpawnFood();
    }
    return 0;
}

void SnakeUpdate(uint32_t now, uint8_t *toMenu)
{
    uint8_t evL, evR, evU;
    KeysScan(&evL, &evR, &evU, now);

    if (evU & EV_LONG) { *toMenu = 1; return; }

    if (s_over) {
        if ((evL & EV_SHORT) || (evR & EV_SHORT)) { *toMenu = 1; return; }
        return;
    }
    if (evR & EV_LONG) {
        s_paused = !s_paused; s_dirty = 1;
        SoundPlay(sk_pause, 1);
        if (!s_paused) s_lastMove = now;
        return;
    }
    if (s_paused) return;

    if (s_tcnt < SNAKE_TURN_QLEN) {
        uint8_t base = (s_tcnt > 0) ? s_tq[s_tcnt-1] : s_dir;
        if (evL & EV_PRESS) {
            uint8_t nd = (base + 3) % 4;
            if (nd != (base + 2) % 4) s_tq[s_tcnt++] = nd;
        } else if (evR & EV_PRESS) {
            uint8_t nd = (base + 1) % 4;
            if (nd != (base + 2) % 4) s_tq[s_tcnt++] = nd;
        }
    }
    if (s_lastMove == 0) s_lastMove = now;
    if ((now - s_lastMove) >= s_interval) {
        s_lastMove = now;
        if (s_tcnt > 0) {
            s_dir = s_tq[0];
            for (uint8_t i = 1; i < s_tcnt; i++) s_tq[i-1] = s_tq[i];
            s_tcnt--;
        }
        if (SnakeMove()) {
            s_over = 1;
            SoundPlay(sk_crash, sizeof(sk_crash)/sizeof(sk_crash[0]));
        }
        s_dirty = 1;
    }
}

void SnakeRender(void)
{
    if (!s_dirty) return;
    ssd1306_Fill(Black);
    char buf[24];
    snprintf(buf, sizeof(buf), "S:%02d L:%02d", s_score, s_len);
    ssd1306_SetCursor(0, 2);
    ssd1306_DrawString(buf, Font_7x10, White);
    if (s_over) { ssd1306_SetCursor(90, 2); ssd1306_DrawString("OVER", Font_7x10, White); }
    else if (s_paused) { ssd1306_SetCursor(90, 2); ssd1306_DrawString("PAUSE", Font_7x10, White); }
    else { snprintf(buf, sizeof(buf), "%3ums", s_interval); ssd1306_SetCursor(92, 2); ssd1306_DrawString(buf, Font_7x10, White); }
    ssd1306_DrawLine(0, SNAKE_GRID_Y-1, SSD1306_WIDTH-1, SNAKE_GRID_Y-1, White);

    FillRect(s_fx*SNAKE_CELL+1, SNAKE_GRID_Y+s_fy*SNAKE_CELL+1,
             s_fx*SNAKE_CELL+SNAKE_CELL-2, SNAKE_GRID_Y+s_fy*SNAKE_CELL+SNAKE_CELL-2, White);
    for (uint16_t i = 0; i < s_len; i++) {
        uint16_t idx = (s_head - i + SNAKE_MAX) % SNAKE_MAX;
        uint8_t px = s_x[idx]*SNAKE_CELL, py = SNAKE_GRID_Y + s_y[idx]*SNAKE_CELL;
        if (i == 0) FillRect(px, py, px+SNAKE_CELL-1, py+SNAKE_CELL-1, White);
        else FillRect(px, py, px+SNAKE_CELL-2, py+SNAKE_CELL-2, White);
    }
    ssd1306_UpdateScreen();
    s_dirty = 0;
}
