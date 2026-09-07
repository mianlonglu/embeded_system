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

/* breakout.c -- the Breakout (bounce-the-ball) game module.
 * Controls: R1 = paddle left, R2 = paddle right, R2 tap = launch ball. */

#include <stdio.h>
#include <stdlib.h>

#include "ssd1306.h"
#include "arcade.h"

#define BRICK_COLS      8
#define BRICK_ROWS      3
#define BRICK_W         16
#define BRICK_H         8
#define BRICK_TOP       8
#define PADDLE_W        24
#define PADDLE_H        3
#define PADDLE_Y        58
#define BALL_SIZE       3
#define BALL_STEP_MS    45
/* paddle moves PADDLE_SPEED pixels per main-loop tick (20 ms) while
 * the key is held. 2 px/tick = 100 px/s, crossing the 104 px travel
 * in about one second. */
#define PADDLE_SPEED    2

static uint8_t  b_bricks[BRICK_ROWS][BRICK_COLS];
static uint16_t b_brickCnt;
static int16_t  b_ballX, b_ballY;
static int8_t   b_ballDX, b_ballDY;
static uint8_t  b_paddleX;
static uint8_t  b_over, b_dirty, b_launched;
static uint16_t b_score;
static uint32_t b_lastMove;

static const Beep bk_hit[]   = { { 1568, 30 } };
static const Beep bk_brick[] = { { 1047, 40 }, { 0, 20 }, { 1568, 40 } };
static const Beep bk_lose[]  = { { 220, 200 }, { 0, 40 }, { 165, 400 } };
static const Beep bk_win[]   = { { 523, 100 }, { 0, 20 }, { 659, 100 }, { 0, 20 }, { 784, 100 }, { 0, 20 }, { 1047, 200 } };
static const Beep bk_start[] = { { 784, 80 }, { 0, 40 }, { 1047, 140 } };

static void BrkResetBall(void)
{
    b_paddleX = (SSD1306_WIDTH - PADDLE_W) / 2;
    b_ballX = b_paddleX + (PADDLE_W - BALL_SIZE) / 2;
    b_ballY = PADDLE_Y - BALL_SIZE - 1;
    b_ballDX = 0; b_ballDY = 0;
    b_launched = 0;
}

void BrkInit(void)
{
    for (uint8_t r = 0; r < BRICK_ROWS; r++)
        for (uint8_t c = 0; c < BRICK_COLS; c++) b_bricks[r][c] = 1;
    b_brickCnt = BRICK_ROWS * BRICK_COLS;
    b_score = 0; b_over = 0; b_dirty = 1;
    BrkResetBall();
    b_lastMove = 0;
    SoundPlay(bk_start, sizeof(bk_start)/sizeof(bk_start[0]));
}

static void BrkLaunch(void)
{
    b_ballDX = (rand() & 1) ? 1 : -1;
    b_ballDY = -1;
    b_launched = 1;
}

void BrkUpdate(uint32_t now, uint8_t *toMenu)
{
    uint8_t evL, evR, evU;
    KeysScan(&evL, &evR, &evU, now);

    if (evU & EV_LONG) { *toMenu = 1; return; }

    if (b_over) {
        if ((evL & EV_SHORT) || (evR & EV_SHORT)) { *toMenu = 1; return; }
        return;
    }

    /* continuous paddle movement while a key is held (not event-based),
     * so the paddle glides instead of inching one pixel every 200 ms. */
    uint8_t hL, hR, hU;
    KeysHeld(&hL, &hR, &hU);
    if (hL) {
        uint8_t step = (b_paddleX > PADDLE_SPEED) ? PADDLE_SPEED : b_paddleX;
        b_paddleX -= step;
        if (!b_launched) b_ballX = b_paddleX + (PADDLE_W - BALL_SIZE) / 2;
        b_dirty = 1;
    }
    if (hR) {
        uint8_t room = SSD1306_WIDTH - PADDLE_W - b_paddleX;
        uint8_t step = (room > PADDLE_SPEED) ? PADDLE_SPEED : room;
        b_paddleX += step;
        if (!b_launched) b_ballX = b_paddleX + (PADDLE_W - BALL_SIZE) / 2;
        b_dirty = 1;
    }
    if ((evR & EV_SHORT) && !b_launched) BrkLaunch();

    if (b_launched) {
        if (b_lastMove == 0) b_lastMove = now;
        if ((now - b_lastMove) >= BALL_STEP_MS) {
            b_lastMove = now;
            int16_t nx = b_ballX + b_ballDX;
            int16_t ny = b_ballY + b_ballDY;

            if (nx < 0) { nx = 0; b_ballDX = -b_ballDX; SoundPlay(bk_hit, 1); }
            else if (nx > SSD1306_WIDTH - BALL_SIZE) { nx = SSD1306_WIDTH - BALL_SIZE; b_ballDX = -b_ballDX; SoundPlay(bk_hit, 1); }
            if (ny < BRICK_TOP) { ny = BRICK_TOP; b_ballDY = -b_ballDY; SoundPlay(bk_hit, 1); }

            if (b_ballDY > 0 && ny + BALL_SIZE >= PADDLE_Y && ny + BALL_SIZE <= PADDLE_Y + PADDLE_H + 2 &&
                nx + BALL_SIZE > b_paddleX && nx < b_paddleX + PADDLE_W) {
                ny = PADDLE_Y - BALL_SIZE;
                b_ballDY = -b_ballDY;
                int16_t hit = (nx + BALL_SIZE/2) - b_paddleX;
                if (hit < PADDLE_W / 3) b_ballDX = -1;
                else if (hit > 2 * PADDLE_W / 3) b_ballDX = 1;
                else b_ballDX = 0;
                SoundPlay(bk_hit, 1);
            }

            {
                int16_t cx = nx + BALL_SIZE / 2;
                int16_t cy = ny + BALL_SIZE / 2;
                if (cy >= BRICK_TOP && cy < BRICK_TOP + BRICK_ROWS * BRICK_H &&
                    cx >= 0 && cx < SSD1306_WIDTH) {
                    uint8_t col = (uint8_t)(cx / BRICK_W);
                    uint8_t row = (uint8_t)((cy - BRICK_TOP) / BRICK_H);
                    if (row < BRICK_ROWS && col < BRICK_COLS && b_bricks[row][col]) {
                        b_bricks[row][col] = 0;
                        b_brickCnt--;
                        b_score++;
                        b_ballDY = -b_ballDY;
                        SoundPlay(bk_brick, sizeof(bk_brick)/sizeof(bk_brick[0]));
                        if (b_brickCnt == 0) {
                            b_over = 1;
                            SoundPlay(bk_win, sizeof(bk_win)/sizeof(bk_win[0]));
                        }
                    }
                }
            }

            b_ballX = nx; b_ballY = ny;

            if (b_ballY >= SSD1306_HEIGHT) {
                b_over = 1;
                SoundPlay(bk_lose, sizeof(bk_lose)/sizeof(bk_lose[0]));
            }
            b_dirty = 1;
        }
    }
}

void BrkRender(void)
{
    if (!b_dirty) return;
    ssd1306_Fill(Black);
    char buf[24];
    snprintf(buf, sizeof(buf), "S:%02d B:%02d", b_score, b_brickCnt);
    ssd1306_SetCursor(0, 0);
    ssd1306_DrawString(buf, Font_7x10, White);
    if (b_over) {
        ssd1306_SetCursor(86, 0);
        ssd1306_DrawString(b_brickCnt == 0 ? "WIN!" : "OVER", Font_7x10, White);
    } else if (!b_launched) {
        ssd1306_SetCursor(92, 0);
        ssd1306_DrawString("R2:go", Font_7x10, White);
    }

    for (uint8_t r = 0; r < BRICK_ROWS; r++) {
        for (uint8_t c = 0; c < BRICK_COLS; c++) {
            if (b_bricks[r][c]) {
                uint8_t bx = c * BRICK_W, by = BRICK_TOP + r * BRICK_H;
                FillRect(bx, by, bx + BRICK_W - 2, by + BRICK_H - 2, White);
            }
        }
    }
    FillRect(b_paddleX, PADDLE_Y, b_paddleX + PADDLE_W - 1, PADDLE_Y + PADDLE_H - 1, White);
    FillRect(b_ballX, b_ballY, b_ballX + BALL_SIZE - 1, b_ballY + BALL_SIZE - 1, White);

    ssd1306_UpdateScreen();
    b_dirty = 0;
}
