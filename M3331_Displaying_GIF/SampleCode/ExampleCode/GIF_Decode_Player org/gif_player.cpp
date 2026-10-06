/**************************************************************************//**
 * @file     gif_player.cpp
 * @brief    Streaming GIF player built on the AnimatedGIF library.
 *
 *           Pipeline (non-blocking state machine):
 *             SPI flash --(QSPI, 2KB cache)--> LZW decode (line by line)
 *                       --(COOKED RGB565)--> 320x240 frame buffer
 *                       --(SPI1 FIFO)--> ILI9341 TFT
 *
 *           Frame-buffer-only COOKED mode is used (no GIFDRAW callback): the
 *           AnimatedGIF library composites each frame into its own canvas, so
 *           frame disposal (restore-to-background / restore-to-previous) and
 *           inter-frame transparency are handled correctly. We then blit the
 *           library's cooked RGB565 canvas straight to the panel.
 *
 *           A single buffer is kept:
 *             - g_libFB  : [0 .. W*H-1]      8-bit index canvas (library state)
 *                          [W*H .. +W*H*2-1] cooked RGB565 canvas (what we blit)
 *                          + a few extra cooked-scratch lines.
 *             g_cooked points at the RGB565 region.
 *
 * @copyright SPDX-License-Identifier: Apache-2.0
 ******************************************************************************/
#include <string.h>
#include <stdio.h>
#include "NuMicro.h"
#include "AnimatedGIF.h"
#include "gif_player.h"
#include "qspi_flash.h"
#include "lcd.h"

#define CANVAS_W        LCD_WIDTH       /* 320 */
#define CANVAS_H        LCD_HEIGHT      /* 240 */

/* millis() is provided by main.c (see gif_compat.h) */
extern "C" unsigned long millis(void);

/* Free-running 100us tick counter (TIMER0) from main.c, used to measure the
   decode (CPU-busy) time versus wall-clock time for FPS / CPU-usage stats. */
extern "C" volatile uint32_t g_u32TimerCnt;
#define STAT_TICKS_PER_SEC  10000u     /* 100us ticks in one second */

/* Performance stats, accumulated over a ~1 second window and printed once per
   second. "Busy" is the time spent inside gif.playFrame() (LZW decode + the
   blocking QSPI-flash reads); the LCD transfer and inter-frame delay run in the
   background / idle, so CPU% = busy / wall-clock. */
static uint32_t s_statWindowStart = 0;  /* window start, 100us ticks         */
static uint32_t s_statBusyTicks   = 0;  /* decode CPU time this window        */
static uint32_t s_statFrames      = 0;  /* frames decoded this window         */

/* AnimatedGIF COOKED frame-buffer-only canvas (NO draw callback).
   When no GIFDRAW callback is installed the library composites each frame into
   this buffer itself, correctly handling frame disposal and inter-frame
   transparency (a transparent pixel keeps the value already in the canvas).
   Required layout for COOKED + RGB565:
     [0 .. W*H-1]            : 8-bit palette-index canvas
     [W*H .. W*H + W*H*2 -1] : RGB565 cooked canvas  <- this is what we blit
   A few extra lines of slack are appended because the library can write a
   little past the last line. 4-byte aligned so the RGB565 region stays
   halfword aligned for the library's 16-bit stores. */
#define FB_INDEX_BYTES   (CANVAS_W * CANVAS_H)
#define FB_RGB565_BYTES  (CANVAS_W * CANVAS_H * 2)
static uint8_t g_libFB[FB_INDEX_BYTES + FB_RGB565_BYTES + CANVAS_W * 2 * 4]
    __attribute__((aligned(4)));

/* The cooked RGB565 canvas streamed to the panel starts right after the
   8-bit index canvas. */
static uint16_t * const g_cooked = (uint16_t *)&g_libFB[FB_INDEX_BYTES];

static AnimatedGIF gif;

/* Player state machine */
enum {
    ST_OPEN = 0,
    ST_DECODE,
    ST_BLIT,
    ST_WAIT,
    ST_ERROR
};
static int           s_state     = ST_OPEN;
static int           s_delayMs   = 0;
static unsigned long s_waitStart = 0;
static int           s_loopEnd   = 0;

/*--------------------------------------------------------------------------*/
/* AnimatedGIF file callbacks (GIF data lives in SPI flash)                  */
/*--------------------------------------------------------------------------*/
static void *GIF_Open_CB(const char *szName, int32_t *pFileSize)
{
    (void)szName;
    *pFileSize = (int32_t)GIF_FILE_SIZE;
    return (void *)1;                       /* non-NULL dummy handle */
}

static void GIF_Close_CB(void *pHandle)
{
    (void)pHandle;
}

static int32_t GIF_Read_CB(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen)
{
    int32_t iRead = iLen;

    if (pFile->iPos + iRead > pFile->iSize)
        iRead = pFile->iSize - pFile->iPos;
    if (iRead <= 0)
        return 0;
#if 0
    {
        static int s_dbgReads = 0;
        if (s_dbgReads < 60)
        {
             printf("rd #%d pos=%d len=%d\n", s_dbgReads, (int)pFile->iPos, (int)iRead);
            s_dbgReads++;
        }
    }
#endif
    FlashCache_Read(GIF_FLASH_BASE_ADDR + (uint32_t)pFile->iPos, pBuf, (uint32_t)iRead);
    pFile->iPos += iRead;
    return iRead;
}

static int32_t GIF_Seek_CB(GIFFILE *pFile, int32_t iPosition)
{
    if (iPosition < 0)
        iPosition = 0;
    else if (iPosition >= pFile->iSize)
        iPosition = pFile->iSize - 1;
    pFile->iPos = iPosition;
    return iPosition;
}

/*--------------------------------------------------------------------------*/
/* Helpers                                                                   */
/*--------------------------------------------------------------------------*/
static int GIF_OpenFile(void)
{
    gif.begin(GIF_PALETTE_RGB565_LE);
    FlashCache_Reset();

    /* No GIFDRAW callback (last arg NULL) : the library composites each frame
       into g_libFB itself, so frame disposal and inter-frame transparency are
       handled correctly. We then blit the cooked RGB565 canvas ourselves. */
    if (gif.open((const char *)"flash",
                 GIF_Open_CB, GIF_Close_CB,
                 GIF_Read_CB, GIF_Seek_CB, NULL) == 0)
    {
        printf("gif.open FAILED, lastError=%d\n", gif.getLastError());
        return 0;
    }

    printf("gif.open OK, canvas=%dx%d\n",
           gif.getCanvasWidth(), gif.getCanvasHeight());

    /* Keep full-canvas state in the index buffer, let the library cook the
       RGB565 output straight into the canvas (no per-line callback). */
    gif.setFrameBuf(g_libFB);
    gif.setDrawType(GIF_DRAW_COOKED);
    return 1;
}

/* Stream the library's cooked RGB565 canvas to the panel : one full-screen
   window, GRAM auto-increments across the whole 320x240 frame. */
static void GIF_StartBlit(void)
{
    LCD_SetWindow(0, 0, CANVAS_W - 1, CANVAS_H - 1);
    LCD_SendFrameStart(g_cooked, (uint32_t)CANVAS_W * CANVAS_H);
}

/*--------------------------------------------------------------------------*/
/* Public API                                                                */
/*--------------------------------------------------------------------------*/

extern "C" void GIF_Player_Init(void)
{
    memset(g_libFB, 0, sizeof(g_libFB));
    s_state = ST_OPEN;
    s_statWindowStart = g_u32TimerCnt;
    s_statBusyTicks   = 0;
    s_statFrames      = 0;
}

extern "C" void GIF_Player_Task(void)
{
    switch (s_state)
    {
    case ST_OPEN:
        s_state = GIF_OpenFile() ? ST_DECODE : ST_ERROR;
        break;

    case ST_DECODE:
    {
        int rc;
        uint32_t tDecStart = g_u32TimerCnt;

        rc = gif.playFrame(false, &s_delayMs, NULL);

        /* Accumulate decode (CPU-busy) time and frame count for the stats. */
        s_statBusyTicks += (uint32_t)(g_u32TimerCnt - tDecStart);
        s_statFrames++;

        /* Once per second, report frame rate and CPU usage. */
        {
            uint32_t elapsed = (uint32_t)(g_u32TimerCnt - s_statWindowStart);
            if (elapsed >= STAT_TICKS_PER_SEC)
            {
                uint32_t fps = (s_statFrames * STAT_TICKS_PER_SEC) / elapsed;
                uint32_t cpu = (s_statBusyTicks * 100u) / elapsed;
                printf("FPS=%lu  CPU=%lu%%\n",
                       (unsigned long)fps, (unsigned long)cpu);
                s_statWindowStart = g_u32TimerCnt;
                s_statBusyTicks   = 0;
                s_statFrames      = 0;
            }
        }

        /* The library composited this frame into the persistent canvas, so the
           whole canvas is always a complete, up-to-date frame : blit it. */
        s_loopEnd = (rc <= 0);              /* last frame of the animation ? */
        GIF_StartBlit();
        s_state = ST_BLIT;
        break;
    }

    case ST_BLIT:
        if (LCD_SendFrameDone())
        {
            if (s_loopEnd)
                gif.reset();
            /* No inter-frame delay : decode the next frame immediately so the
               player runs as fast as the decode + LCD transfer allow. */
            s_state = ST_DECODE;
        }
        break;

    case ST_ERROR:
    default:
        s_state = ST_OPEN;                  /* retry opening the GIF */
        break;
    }
}
