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
#define KEY_VLT_USER_MAX    0.35f   /* <  0.35V          -> USER         */
#define KEY_VLT_R1_MIN      0.40f   /* 0.40V ~ 0.75V     -> R1 (S1)      */
#define KEY_VLT_R1_MAX      0.75f
#define KEY_VLT_R2_MIN      0.80f   /* 0.80V ~ 1.20V     -> R2 (S2)      */
#define KEY_VLT_R2_MAX      1.20f

/* Passive buzzer: GPIO9/PWM0 (traffic-light / environment board) */
#define BUZZER_GPIO         9
#define BUZZER_PWM_PORT     0
#define BUZZER_DUTY         50

/* -------------------- game config -------------------- */
#define COLS                16
#define ROWS                6
#define MINE_NUM            15
#define CELL_PITCH          8   /* pixel pitch between cells                */
#define CELL_BOX            7   /* cell drawn as 7x7, 1px gap = grid line   */
#define GRID_X              0
#define GRID_Y              16  /* status bar occupies y 0..15              */
#define CELL_TOTAL          (COLS * ROWS)

#define TASK_STACK_SIZE     (8192)
#define POLL_TICKS          2   /* 20 ms polling period                     */
#define LONG_PRESS_MS       500
#define REPEAT_MS           200
#define BLINK_MS            500
#define WELCOME_SHOW_MS     2000

typedef struct {
    uint8_t mine;     /* 1 = mine                            */
    uint8_t opened;   /* 1 = revealed                        */
    uint8_t flagged;  /* 1 = flagged with a flag             */
    uint8_t adj;      /* adjacent mine count, 0..8           */
} Cell;

/* virtual key ids decoded from the ADC divider network */
enum {
    KEY_NONE = 0,
    KEY_USER = 1,
    KEY_R1   = 2,      /* S1 below the OLED: cursor movement    */
    KEY_R2   = 3,      /* S2 below the OLED: open / flag        */
};

typedef struct {
    uint8_t  level;     /* debounced level: 1 released, 0 pressed */
    uint8_t  lastRaw;
    uint8_t  debounce;
    uint8_t  held;      /* stable pressed state                  */
    uint8_t  longFired;
    uint32_t downMs;
    uint32_t lastRepMs;
} Key;

enum {
    EV_PRESS  = 0x01,  /* just pressed                          */
    EV_LONG   = 0x02,  /* held over LONG_PRESS_MS (once)        */
    EV_REPEAT = 0x04,  /* still held after long (auto repeat)   */
    EV_SHORT  = 0x08,  /* released within LONG_PRESS_MS         */
};

/* -------------------- sound effects (buzzer on PWM0) -------------------- */
typedef struct {
    uint16_t freq;      /* tone in Hz, 0 = rest (silence)        */
    uint16_t durMs;     /* duration in ms                        */
} Beep;

#define SOUND_QUEUE_LEN     16

static Cell g_board[ROWS][COLS];
static uint8_t  g_curX = 0;
static uint8_t  g_curY = 0;
static uint8_t  g_started = 0;   /* mines are laid after the first open */
static uint8_t  g_over = 0;      /* 0 = playing, 1 = lose, 2 = win      */
static uint8_t  g_flags = 0;
static uint16_t g_openedCnt = 0;
static uint8_t  g_dirty = 1;

static Key g_keyMove;   /* R1(S1): cursor movement                 */
static Key g_keyAct;    /* R2(S2): open / flag                     */
static Key g_keyUser;   /* USER : restart when game over           */
static uint8_t g_lastRawKey = KEY_NONE;

static Beep g_sndQueue[SOUND_QUEUE_LEN];
static uint8_t  g_sndHead = 0;
static uint8_t  g_sndTail = 0;
static uint8_t  g_sndBusy = 0;
static uint32_t g_sndEndMs = 0;

/* -------------------- sound effects (non-blocking beep queue) -------------------- */

static void SoundQueue(uint16_t freq, uint16_t durMs)
{
    uint8_t next = (uint8_t)((g_sndTail + 1) % SOUND_QUEUE_LEN);
    if (next == g_sndHead) {
        return;   /* queue full, drop the note */
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

/* Advance the beep queue; call once per main-loop iteration. */
static void SoundUpdate(uint32_t now)
{
    if (g_sndBusy) {
        if ((int32_t)(now - g_sndEndMs) < 0) {
            return;   /* current note is still playing */
        }
        IoTPwmStop(BUZZER_PWM_PORT);
        g_sndBusy = 0;
    }
    if (g_sndHead == g_sndTail) {
        return;   /* queue empty */
    }
    Beep *b = &g_sndQueue[g_sndHead];
    g_sndHead = (uint8_t)((g_sndHead + 1) % SOUND_QUEUE_LEN);
    if (b->freq > 0) {
        IoTPwmStart(BUZZER_PWM_PORT, BUZZER_DUTY, b->freq);
    }
    g_sndEndMs = now + b->durMs;
    g_sndBusy = 1;
}

/* note sequences: cursor move / open cell / flag / boom / win / start */
static const Beep g_sndMove[]  = { { 2000, 30 } };
static const Beep g_sndOpen[]  = { { 1319, 60 } };
static const Beep g_sndFlag[]  = { { 1568, 50 }, { 0, 40 }, { 1047, 70 } };
static const Beep g_sndBoom[]  = { { 330, 200 }, { 0, 30 }, { 220, 380 } };
static const Beep g_sndWin[]   = { { 523, 130 }, { 0, 20 }, { 659, 130 },
                                   { 0, 20 }, { 784, 130 }, { 0, 20 },
                                   { 1047, 220 } };
static const Beep g_sndStart[] = { { 784, 80 }, { 0, 40 }, { 1047, 140 } };

/* -------------------- game logic -------------------- */

static void NewGame(void)
{
    memset(g_board, 0, sizeof(g_board));
    g_curX = 0;
    g_curY = 0;
    g_started = 0;
    g_over = 0;
    g_flags = 0;
    g_openedCnt = 0;
    g_dirty = 1;
    SoundPlay(g_sndStart, sizeof(g_sndStart) / sizeof(g_sndStart[0]));
    printf("[Minesweeper] New game: %d cols x %d rows, %d mines.\r\n",
           COLS, ROWS, MINE_NUM);
}

/* Lay mines after the first open, keeping the 3x3 area around the
 * first click mine-free so the game always opens with a flood. */
static void LayMines(uint8_t safeX, uint8_t safeY)
{
    int laid = 0;
    while (laid < MINE_NUM) {
        int idx = rand() % CELL_TOTAL;
        uint8_t x = idx % COLS;
        uint8_t y = idx / COLS;
        if (g_board[y][x].mine) {
            continue;
        }
        if (abs(x - safeX) <= 1 && abs(y - safeY) <= 1) {
            continue;
        }
        g_board[y][x].mine = 1;
        laid++;
    }

    for (uint8_t y = 0; y < ROWS; y++) {
        for (uint8_t x = 0; x < COLS; x++) {
            uint8_t cnt = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx;
                    int ny = y + dy;
                    if ((dx == 0 && dy == 0) || nx < 0 || nx >= COLS ||
                        ny < 0 || ny >= ROWS) {
                        continue;
                    }
                    if (g_board[ny][nx].mine) {
                        cnt++;
                    }
                }
            }
            g_board[y][x].adj = cnt;
        }
    }
}

/* Open a cell, flood-filling the zero area. Returns 1 if a mine was hit. */
static uint8_t FloodOpen(uint8_t x, uint8_t y)
{
    uint8_t stack[CELL_TOTAL * 9][2];
    int sp = 0;
    stack[sp][0] = x;
    stack[sp][1] = y;
    sp++;

    while (sp > 0) {
        sp--;
        uint8_t cx = stack[sp][0];
        uint8_t cy = stack[sp][1];
        Cell *c = &g_board[cy][cx];
        if (c->opened || c->flagged) {
            continue;
        }
        c->opened = 1;
        g_openedCnt++;
        if (c->mine) {
            return 1;
        }
        if (c->adj == 0) {
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = cx + dx;
                    int ny = cy + dy;
                    if (nx < 0 || nx >= COLS || ny < 0 || ny >= ROWS) {
                        continue;
                    }
                    stack[sp][0] = (uint8_t)nx;
                    stack[sp][1] = (uint8_t)ny;
                    sp++;
                }
            }
        }
    }
    return 0;
}

static void DoOpen(void)
{
    Cell *c = &g_board[g_curY][g_curX];
    if (c->opened || c->flagged) {
        return;
    }
    if (!g_started) {
        LayMines(g_curX, g_curY);
        g_started = 1;
    }
    if (FloodOpen(g_curX, g_curY)) {
        g_over = 1;
        printf("[Minesweeper] BOOM! Game over at (%d,%d).\r\n", g_curX, g_curY);
    } else if (g_openedCnt == CELL_TOTAL - MINE_NUM) {
        g_over = 2;
        printf("[Minesweeper] You win! All safe cells opened.\r\n");
    }
}

static void DoFlag(void)
{
    Cell *c = &g_board[g_curY][g_curX];
    if (c->opened) {
        return;
    }
    c->flagged = !c->flagged;
    g_flags += c->flagged ? 1 : -1;
    printf("[Minesweeper] (%d,%d) %s, flags: %d.\r\n",
           g_curX, g_curY, c->flagged ? "flagged" : "unflagged", g_flags);
}

static void MoveRight(void)
{
    g_curX = (g_curX + 1) % COLS;
}

static void MoveDown(void)
{
    g_curY = (g_curY + 1) % ROWS;
}

/* -------------------- key scanning (ADC divider, debounced, polled) -------- */

/* Read GPIO5/ADC2 and classify the pressed key by the divider voltage. */
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
            printf("[Minesweeper] ADC raw=%u, v=%.2fV -> %s\r\n",
                   (unsigned int)data, v,
                   key == KEY_R1 ? "R1" : (key == KEY_R2 ? "R2" : "USER"));
        }
        g_lastRawKey = key;
    }
    return key;
}

/* Debounce / long-press / auto-repeat state machine for one virtual key.
 * `pressed` is the raw reading for the key this instance tracks. */
static uint8_t KeyUpdate(Key *k, uint8_t pressed, uint32_t now)
{
    uint8_t ev = 0;
    uint8_t raw = pressed ? 0 : 1;   /* keep the active-low semantics */

    if (raw != k->lastRaw) {
        k->debounce = 0;
        k->lastRaw = raw;
    } else if (k->debounce < 3) {
        k->debounce++;   /* 3 * 20ms = 60ms debounce */
    }

    if (k->debounce >= 3 && raw != k->level) {
        k->level = raw;
        if (raw == 0) {          /* stable pressed edge (active low) */
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
    if (k->held && k->longFired && (now - k->lastRepMs) >= REPEAT_MS) {
        k->lastRepMs = now;
        ev |= EV_REPEAT;
    }
    return ev;
}

static void HandleKeys(uint32_t now)
{
    uint8_t rawKey = AdcScanKey();
    uint8_t evMove = KeyUpdate(&g_keyMove, rawKey == KEY_R1, now);
    uint8_t evAct  = KeyUpdate(&g_keyAct,  rawKey == KEY_R2, now);
    uint8_t evUser = KeyUpdate(&g_keyUser, rawKey == KEY_USER, now);

    if (g_over) {
        /* any short tap (R1 / R2 / USER) starts a new game */
        if ((evMove & EV_SHORT) || (evAct & EV_SHORT) || (evUser & EV_SHORT)) {
            NewGame();
        }
        return;
    }

    if (evMove & EV_PRESS) {
        MoveRight();
        SoundPlay(g_sndMove, sizeof(g_sndMove) / sizeof(g_sndMove[0]));
        g_dirty = 1;
    }
    if (evMove & (EV_LONG | EV_REPEAT)) {
        MoveDown();
        SoundPlay(g_sndMove, sizeof(g_sndMove) / sizeof(g_sndMove[0]));
        g_dirty = 1;
    }
    if (evAct & EV_SHORT) {
        DoOpen();
        if (g_over == 1) {
            SoundPlay(g_sndBoom, sizeof(g_sndBoom) / sizeof(g_sndBoom[0]));
        } else if (g_over == 2) {
            SoundPlay(g_sndWin, sizeof(g_sndWin) / sizeof(g_sndWin[0]));
        } else {
            SoundPlay(g_sndOpen, sizeof(g_sndOpen) / sizeof(g_sndOpen[0]));
        }
        g_dirty = 1;
    }
    if (evAct & EV_LONG) {
        DoFlag();
        SoundPlay(g_sndFlag, sizeof(g_sndFlag) / sizeof(g_sndFlag[0]));
        g_dirty = 1;
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

/* Flag marker: pole + cloth triangle, drawn in `color`. */
static void DrawFlag(uint8_t px, uint8_t py, SSD1306_COLOR color)
{
    ssd1306_DrawLine(px + 2, py + 1, px + 2, py + 6, color);   /* pole */
    ssd1306_DrawLine(px + 3, py + 1, px + 5, py + 1, color);   /* cloth */
    ssd1306_DrawLine(px + 3, py + 2, px + 4, py + 2, color);
    ssd1306_DrawPixel(px + 3, py + 3, color);
    ssd1306_DrawLine(px + 1, py + 6, px + 4, py + 6, color);   /* base */
}

/* Mine marker: round body with 4 spikes. */
static void DrawMine(uint8_t px, uint8_t py, SSD1306_COLOR color)
{
    ssd1306_DrawCircle(px + 3, py + 3, 2, color);
    ssd1306_DrawPixel(px + 3, py + 0, color);
    ssd1306_DrawPixel(px + 3, py + 6, color);
    ssd1306_DrawPixel(px + 0, py + 3, color);
    ssd1306_DrawPixel(px + 6, py + 3, color);
    ssd1306_DrawPixel(px + 1, py + 1, color);
    ssd1306_DrawPixel(px + 5, py + 1, color);
    ssd1306_DrawPixel(px + 1, py + 5, color);
    ssd1306_DrawPixel(px + 5, py + 5, color);
}

static void DrawCell(uint8_t x, uint8_t y, uint8_t blinkOn)
{
    uint8_t px = GRID_X + x * CELL_PITCH;
    uint8_t py = GRID_Y + y * CELL_PITCH;
    Cell *c = &g_board[y][x];
    uint8_t isCursor = (x == g_curX && y == g_curY);

    /* after game over: reveal mines and mark wrong flags */
    if (g_over && c->mine && !c->flagged) {
        if (c->opened) {
            FillRect(px, py, px + CELL_BOX - 1, py + CELL_BOX - 1, White);
            ssd1306_DrawLine(px + 1, py + 1, px + 5, py + 5, Black);
            ssd1306_DrawLine(px + 5, py + 1, px + 1, py + 5, Black);
        } else {
            DrawMine(px, py, White);
        }
        return;
    }
    if (g_over && c->flagged && !c->mine) {
        FillRect(px, py, px + CELL_BOX - 1, py + CELL_BOX - 1, White);
        ssd1306_DrawLine(px + 1, py + 1, px + 5, py + 5, Black);
        ssd1306_DrawLine(px + 5, py + 1, px + 1, py + 5, Black);
        return;
    }

    if (!c->opened) {
        /* unopened: white button */
        FillRect(px, py, px + CELL_BOX - 1, py + CELL_BOX - 1, White);
        if (c->flagged) {
            DrawFlag(px, py, Black);
        }
        if (isCursor && blinkOn) {
            ssd1306_DrawRectangle(px + 1, py + 1, px + 5, py + 5, Black);
        }
    } else {
        if (c->adj > 0) {
            char ch = (char)('0' + c->adj);
            ssd1306_SetCursor(px, py);
            ssd1306_DrawChar(ch, Font_6x8, White);
        }
        if (isCursor && blinkOn) {
            ssd1306_DrawRectangle(px, py, px + CELL_BOX - 1,
                                  py + CELL_BOX - 1, White);
        }
    }
}

static void DrawStatus(void)
{
    char buf[24];

    snprintf(buf, sizeof(buf), "M:%02d F:%02d", MINE_NUM, g_flags);
    ssd1306_SetCursor(0, 2);
    ssd1306_DrawString(buf, Font_7x10, White);

    if (g_over == 1) {
        ssd1306_SetCursor(96, 2);
        ssd1306_DrawString("BOOM", Font_7x10, White);
    } else if (g_over == 2) {
        ssd1306_SetCursor(96, 2);
        ssd1306_DrawString("WIN!", Font_7x10, White);
    } else {
        snprintf(buf, sizeof(buf), "R%02dC%02d", g_curY + 1, g_curX + 1);
        ssd1306_SetCursor(84, 2);
        ssd1306_DrawString(buf, Font_7x10, White);
    }
    ssd1306_DrawLine(0, GRID_Y - 1, SSD1306_WIDTH - 1, GRID_Y - 1, White);
}

static void Render(uint8_t blinkOn)
{
    ssd1306_Fill(Black);
    DrawStatus();
    for (uint8_t y = 0; y < ROWS; y++) {
        for (uint8_t x = 0; x < COLS; x++) {
            DrawCell(x, y, blinkOn);
        }
    }
    ssd1306_UpdateScreen();
}

/* ASCII board dump on the serial port when a round ends. */
static void PrintBoard(void)
{
    printf("\r\n    ");
    for (uint8_t x = 0; x < COLS; x++) {
        printf("%X", x);
    }
    printf("\r\n");
    for (uint8_t y = 0; y < ROWS; y++) {
        printf("%02d  ", y);
        for (uint8_t x = 0; x < COLS; x++) {
            Cell *c = &g_board[y][x];
            char ch;
            if (c->flagged) {
                ch = 'F';
            } else if (!c->opened) {
                ch = '.';
            } else if (c->mine) {
                ch = '*';
            } else {
                ch = c->adj ? (char)('0' + c->adj) : ' ';
            }
            printf("%c", ch);
        }
        printf("\r\n");
    }
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

/* GPIO5 -> ADC2: the resistor-divider network of R1/R2/USER keys. */
static void InitKeys(void)
{
    IoTGpioInit(HI_IO_NAME_GPIO_5);
    hi_io_set_func(HI_IO_NAME_GPIO_5, HI_IO_FUNC_GPIO_5_GPIO);
    IoTGpioSetDir(HI_IO_NAME_GPIO_5, IOT_GPIO_DIR_IN);

    Key *keys[] = { &g_keyMove, &g_keyAct, &g_keyUser };
    for (uint8_t i = 0; i < 3; i++) {
        keys[i]->level = 1;
        keys[i]->lastRaw = 1;
        keys[i]->debounce = 3;
        keys[i]->held = 0;
        keys[i]->longFired = 0;
    }
}

/* GPIO9 -> PWM0: passive buzzer */
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
    ssd1306_SetCursor(3, 0);            /* 11 chars * 11px = 121px */
    ssd1306_DrawString("MINESWEEPER", Font_11x18, White);
    ssd1306_SetCursor(4, 22);
    ssd1306_DrawString("R1: move cursor", Font_7x10, White);
    ssd1306_SetCursor(4, 34);
    ssd1306_DrawString("R2 tap:  open", Font_7x10, White);
    ssd1306_SetCursor(4, 46);
    ssd1306_DrawString("R2 hold: flag", Font_7x10, White);
    ssd1306_UpdateScreen();
    printf("[Minesweeper] Keys share GPIO5/ADC2 via resistor divider:\r\n");
    printf("[Minesweeper]   R1(S1) ~0.57V, R2(S2) ~0.97V, USER ~0.20V, none ~3.23V.\r\n");
    printf("[Minesweeper] R1: tap = move right, hold = move down;\r\n");
    printf("[Minesweeper] R2: tap = open cell, hold = flag/unflag; buzzer on GPIO9/PWM0.\r\n");
}

static void MinesweeperTask(void)
{
    InitOled();
    InitKeys();
    InitBuzzer();

    srand((unsigned int)HAL_GetTick());
    NewGame();
    ShowWelcome();
    HAL_Delay(WELCOME_SHOW_MS);
    g_dirty = 1;

    uint32_t lastBlink = HAL_GetTick();
    uint8_t blinkOn = 0;
    uint8_t lastOver = 0;

    while (1) {
        uint32_t now = HAL_GetTick();
        HandleKeys(now);
        SoundUpdate(now);

        if (g_over != lastOver) {
            lastOver = g_over;
            if (g_over) {
                PrintBoard();
            }
        }

        if ((now - lastBlink) >= BLINK_MS) {
            lastBlink = now;
            blinkOn = !blinkOn;
            g_dirty = 1;
        }
        if (g_dirty) {
            Render(blinkOn);
            g_dirty = 0;
        }
        osDelay(POLL_TICKS);
    }
}

static void MinesweeperEntry(void)
{
    osThreadAttr_t attr;

    attr.name = "MinesweeperTask";
    attr.attr_bits = 0U;
    attr.cb_mem = NULL;
    attr.cb_size = 0U;
    attr.stack_mem = NULL;
    attr.stack_size = TASK_STACK_SIZE;
    attr.priority = osPriorityNormal;

    if (osThreadNew((osThreadFunc_t)MinesweeperTask, NULL, &attr) == NULL) {
        printf("[Minesweeper] Failed to create MinesweeperTask!\r\n");
    }
}
APP_FEATURE_INIT(MinesweeperEntry);
