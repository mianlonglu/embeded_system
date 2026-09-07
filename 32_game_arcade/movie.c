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

/* movie.c -- badge video player. Decodes per-frame RLE data from
 * frame_data.c into the SSD1306 buffer and streams the matching
 * single-tone melody from sfx_data.c through the buzzer queue.
 *
 * Frame rate   : 12 fps  -> 83 ms/frame
 * Video length : 280 frames (~23.3 s)
 * Audio        : 170 Beep notes, fed into the 16-slot buzzer queue
 *                a few notes at a time so the whole melody plays
 *                without overflowing the queue.
 */

#include <stdint.h>
#include <string.h>

#include "ssd1306.h"
#include "arcade.h"

/* auto-generated data lives in frame_data.c / sfx_data.c (compiled as
 * separate translation units, linked here). */
extern const uint16_t MOVIE_FRAME_COUNT;
extern const uint16_t MOVIE_RLE_MAX;
extern const uint16_t MOVIE_FRAME_BYTES;
extern const uint32_t MOVIE_DATA_LEN;
extern const uint32_t MOVIE_OFFSET[];   /* size = MOVIE_FRAME_COUNT + 1 */
extern const uint8_t  MOVIE_DATA[];     /* size = MOVIE_DATA_LEN        */
extern const uint16_t MOVIE_SFX_COUNT;
extern const Beep     MOVIE_SFX[];      /* size = MOVIE_SFX_COUNT       */

#define MOVIE_FPS         12
#define MOVIE_FRAME_MS    (1000 / MOVIE_FPS)   /* 83 ms */

/* feed this many notes per tick when the queue has room. Keeps the
 * 16-slot buzzer queue topped up without overflowing it. */
#define MOVIE_FEED_PER_TICK 4

static uint16_t m_frame = 0;       /* current video frame index   */
static uint16_t m_sfxIdx = 0;      /* next Beep to enqueue        */
static uint32_t m_nextFrameMs = 0; /* wall-clock ms of next frame */
static uint8_t  m_started = 0;
static uint8_t  m_finished = 0;    /* 1 once last frame shown     */
static uint8_t  m_dirty = 1;

/* RLE scratch buffer reused every render (1 KB). */
static uint8_t m_buf[1024];

/* decode frame f from MOVIE_DATA into m_buf (1024 bytes). */
static void MovieDecodeFrame(uint16_t f)
{
    uint32_t off  = MOVIE_OFFSET[f];
    uint32_t end  = MOVIE_OFFSET[f + 1];
    uint32_t i    = 0;
    while (off + 1 < end && i < sizeof(m_buf)) {
        uint8_t cnt = MOVIE_DATA[off++];
        uint8_t val = MOVIE_DATA[off++];
        uint32_t n = (cnt > sizeof(m_buf) - i) ? (sizeof(m_buf) - i) : cnt;
        memset(&m_buf[i], val, n);
        i += n;
    }
    /* any remaining bytes (short RLE) stay zero -- matches Black */
    if (i < sizeof(m_buf)) {
        memset(&m_buf[i], 0x00, sizeof(m_buf) - i);
    }
}

void MovieInit(void)
{
    m_frame = 0;
    m_sfxIdx = 0;
    m_started = 0;
    m_finished = 0;
    m_dirty = 1;
    LedAllOff();
    /* first frame visible immediately */
    MovieDecodeFrame(0);
    ssd1306_FillBuffer(m_buf, sizeof(m_buf));
    ssd1306_UpdateScreen();
}

void MovieUpdate(uint32_t now, uint8_t *toMenu)
{
    uint8_t evR1, evR2, evU;
    KeysScan(&evR1, &evR2, &evU, now);

    /* USER long-press always returns to the menu. */
    if (evU & EV_LONG) { *toMenu = 1; return; }

    /* once the clip has finished, any short press returns. */
    if (m_finished) {
        if ((evR1 & EV_SHORT) || (evR2 & EV_SHORT) || (evU & EV_SHORT)) {
            *toMenu = 1;
        }
        return;
    }

    if (!m_started) {
        m_started = 1;
        m_nextFrameMs = now + MOVIE_FRAME_MS;
    }

    /* stream audio: while the buzzer queue has room, push a few notes
     * so the long melody plays continuously. */
    for (uint8_t k = 0; k < MOVIE_FEED_PER_TICK; k++) {
        if (m_sfxIdx >= MOVIE_SFX_COUNT) break;
        if (SoundSlots() == 0) break;
        SoundPlay(&MOVIE_SFX[m_sfxIdx], 1);
        m_sfxIdx++;
    }

    /* advance video frame at 12 fps. Uses an absolute deadline so the
     * clip length stays correct even if the odd tick slips. */
    if ((int32_t)(now - m_nextFrameMs) >= 0) {
        m_nextFrameMs += MOVIE_FRAME_MS;
        if (m_frame < MOVIE_FRAME_COUNT - 1) {
            m_frame++;
            m_dirty = 1;
        } else {
            m_finished = 1;   /* hold the last frame on screen */
        }
    }
}

void MovieRender(void)
{
    if (!m_dirty) return;
    m_dirty = 0;
    MovieDecodeFrame(m_frame);
    ssd1306_FillBuffer(m_buf, sizeof(m_buf));
    ssd1306_UpdateScreen();
}
