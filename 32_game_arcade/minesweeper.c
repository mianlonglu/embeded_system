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

/* minesweeper.c -- the Minesweeper game module.
 * Controls: R1 tap = move right, R1 hold = move down;
 *           R2 tap = open cell, R2 hold = flag / unflag.
 * Feedback: safe open -> green LED 1s; mine hit -> red LED 3s + beeps. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ssd1306.h"
#include "arcade.h"

#define MINE_COLS           16
#define MINE_ROWS           6
#define MINE_NUM            15
#define MINE_CELL_PITCH     8
#define MINE_CELL_BOX       7
#define MINE_GRID_Y         16
#define MINE_CELL_TOTAL     (MINE_COLS * MINE_ROWS)

typedef struct { uint8_t mine, opened, flagged, adj; } MineCell;
static MineCell m_board[MINE_ROWS][MINE_COLS];
static uint8_t  m_curX, m_curY, m_started, m_over, m_flags, m_dirty;
static uint16_t m_opened;

static const Beep ms_move[]  = { { 2000, 30 } };
static const Beep ms_open[]  = { { 1319, 60 } };
static const Beep ms_flag[]  = { { 1568, 50 }, { 0, 40 }, { 1047, 70 } };
static const Beep ms_boom[]  = { { 2500, 90 }, { 0, 70 }, { 2500, 90 }, { 0, 70 }, { 2500, 130 } };
static const Beep ms_win[]   = { { 523, 130 }, { 0, 20 }, { 659, 130 }, { 0, 20 }, { 784, 130 }, { 0, 20 }, { 1047, 220 } };
static const Beep ms_start[] = { { 784, 80 }, { 0, 40 }, { 1047, 140 } };

static void MineDrawFlag(uint8_t px, uint8_t py, SSD1306_COLOR c)
{
    ssd1306_DrawLine(px+2, py+1, px+2, py+6, c);
    ssd1306_DrawLine(px+3, py+1, px+5, py+1, c);
    ssd1306_DrawLine(px+3, py+2, px+4, py+2, c);
    ssd1306_DrawPixel(px+3, py+3, c);
    ssd1306_DrawLine(px+1, py+6, px+4, py+6, c);
}
static void MineDrawMine(uint8_t px, uint8_t py, SSD1306_COLOR c)
{
    ssd1306_DrawCircle(px+3, py+3, 2, c);
    ssd1306_DrawPixel(px+3, py+0, c); ssd1306_DrawPixel(px+3, py+6, c);
    ssd1306_DrawPixel(px+0, py+3, c); ssd1306_DrawPixel(px+6, py+3, c);
    ssd1306_DrawPixel(px+1, py+1, c); ssd1306_DrawPixel(px+5, py+1, c);
    ssd1306_DrawPixel(px+1, py+5, c); ssd1306_DrawPixel(px+5, py+5, c);
}
static void MineLayMines(uint8_t safeX, uint8_t safeY)
{
    int laid = 0;
    while (laid < MINE_NUM) {
        int idx = rand() % MINE_CELL_TOTAL;
        uint8_t x = idx % MINE_COLS, y = idx / MINE_COLS;
        if (m_board[y][x].mine) continue;
        if (abs(x - safeX) <= 1 && abs(y - safeY) <= 1) continue;
        m_board[y][x].mine = 1; laid++;
    }
    for (uint8_t y = 0; y < MINE_ROWS; y++)
        for (uint8_t x = 0; x < MINE_COLS; x++) {
            uint8_t cnt = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x+dx, ny = y+dy;
                    if ((dx==0&&dy==0) || nx<0||nx>=MINE_COLS||ny<0||ny>=MINE_ROWS) continue;
                    if (m_board[ny][nx].mine) cnt++;
                }
            m_board[y][x].adj = cnt;
        }
}
static uint8_t MineFloodOpen(uint8_t x, uint8_t y)
{
    uint8_t stack[MINE_CELL_TOTAL * 9][2];
    int sp = 0;
    stack[sp][0] = x; stack[sp][1] = y; sp++;
    while (sp > 0) {
        sp--;
        uint8_t cx = stack[sp][0], cy = stack[sp][1];
        MineCell *c = &m_board[cy][cx];
        if (c->opened || c->flagged) continue;
        c->opened = 1; m_opened++;
        if (c->mine) return 1;
        if (c->adj == 0) {
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = cx+dx, ny = cy+dy;
                    if (nx<0||nx>=MINE_COLS||ny<0||ny>=MINE_ROWS) continue;
                    stack[sp][0] = (uint8_t)nx; stack[sp][1] = (uint8_t)ny; sp++;
                }
        }
    }
    return 0;
}

void MineInit(void)
{
    memset(m_board, 0, sizeof(m_board));
    m_curX = 0; m_curY = 0; m_started = 0; m_over = 0; m_flags = 0;
    m_opened = 0; m_dirty = 1;
    LedAllOff();
    SoundPlay(ms_start, sizeof(ms_start)/sizeof(ms_start[0]));
}

void MineUpdate(uint32_t now, uint8_t *toMenu)
{
    uint8_t evM, evA, evU;
    KeysScan(&evM, &evA, &evU, now);

    if (evU & EV_LONG) { *toMenu = 1; return; }

    if (m_over) {
        if ((evM & EV_SHORT) || (evA & EV_SHORT)) { *toMenu = 1; return; }
        return;
    }
    if (evM & EV_PRESS) {
        m_curX = (m_curX + 1) % MINE_COLS;
        SoundPlay(ms_move, 1); m_dirty = 1;
    }
    if (evM & (EV_LONG | EV_REPEAT)) {
        m_curY = (m_curY + 1) % MINE_ROWS;
        SoundPlay(ms_move, 1); m_dirty = 1;
    }
    if (evA & EV_SHORT) {
        MineCell *c = &m_board[m_curY][m_curX];
        if (!c->opened && !c->flagged) {
            if (!m_started) { MineLayMines(m_curX, m_curY); m_started = 1; }
            if (MineFloodOpen(m_curX, m_curY)) {
                m_over = 1;
                LedPulse(LED_RED_GPIO, 3000, now);
                SoundPlay(ms_boom, sizeof(ms_boom)/sizeof(ms_boom[0]));
            } else if (m_opened == MINE_CELL_TOTAL - MINE_NUM) {
                m_over = 2;
                LedPulse(LED_GREEN_GPIO, 1000, now);
                SoundPlay(ms_win, sizeof(ms_win)/sizeof(ms_win[0]));
            } else {
                LedPulse(LED_GREEN_GPIO, 1000, now);
                SoundPlay(ms_open, 1);
            }
        }
        m_dirty = 1;
    }
    if (evA & EV_LONG) {
        MineCell *c = &m_board[m_curY][m_curX];
        if (!c->opened) { c->flagged = !c->flagged; m_flags += c->flagged ? 1 : -1; }
        SoundPlay(ms_flag, sizeof(ms_flag)/sizeof(ms_flag[0]));
        m_dirty = 1;
    }
}

void MineRender(void)
{
    if (!m_dirty) return;
    ssd1306_Fill(Black);
    char buf[24];
    snprintf(buf, sizeof(buf), "M:%02d F:%02d", MINE_NUM, m_flags);
    ssd1306_SetCursor(0, 2);
    ssd1306_DrawString(buf, Font_7x10, White);
    if (m_over == 1) { ssd1306_SetCursor(96, 2); ssd1306_DrawString("BOOM", Font_7x10, White); }
    else if (m_over == 2) { ssd1306_SetCursor(96, 2); ssd1306_DrawString("WIN!", Font_7x10, White); }
    else { snprintf(buf, sizeof(buf), "R%02dC%02d", m_curY+1, m_curX+1); ssd1306_SetCursor(84, 2); ssd1306_DrawString(buf, Font_7x10, White); }
    ssd1306_DrawLine(0, MINE_GRID_Y-1, SSD1306_WIDTH-1, MINE_GRID_Y-1, White);

    for (uint8_t y = 0; y < MINE_ROWS; y++) {
        for (uint8_t x = 0; x < MINE_COLS; x++) {
            uint8_t px = x * MINE_CELL_PITCH;
            uint8_t py = MINE_GRID_Y + y * MINE_CELL_PITCH;
            MineCell *c = &m_board[y][x];
            uint8_t cur = (x == m_curX && y == m_curY);
            if (m_over && c->mine && !c->flagged) {
                if (c->opened) {
                    FillRect(px, py, px+MINE_CELL_BOX-1, py+MINE_CELL_BOX-1, White);
                    ssd1306_DrawLine(px+1, py+1, px+5, py+5, Black);
                    ssd1306_DrawLine(px+5, py+1, px+1, py+5, Black);
                } else MineDrawMine(px, py, White);
                continue;
            }
            if (m_over && c->flagged && !c->mine) {
                FillRect(px, py, px+MINE_CELL_BOX-1, py+MINE_CELL_BOX-1, White);
                ssd1306_DrawLine(px+1, py+1, px+5, py+5, Black);
                ssd1306_DrawLine(px+5, py+1, px+1, py+5, Black);
                continue;
            }
            if (!c->opened) {
                FillRect(px, py, px+MINE_CELL_BOX-1, py+MINE_CELL_BOX-1, White);
                if (c->flagged) MineDrawFlag(px, py, Black);
                if (cur) ssd1306_DrawRectangle(px+1, py+1, px+5, py+5, Black);
            } else {
                if (c->adj > 0) {
                    ssd1306_SetCursor(px, py);
                    ssd1306_DrawChar((char)('0'+c->adj), Font_6x8, White);
                }
                if (cur) ssd1306_DrawRectangle(px, py, px+MINE_CELL_BOX-1, py+MINE_CELL_BOX-1, White);
            }
        }
    }
    ssd1306_UpdateScreen();
    m_dirty = 0;
}
