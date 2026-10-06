/**************************************************************************//**
 * @file     gif_player.cpp
 * @brief    Streaming GIF player built on the AnimatedGIF library.
 *
 *           Pipelined, non-blocking state machine that overlaps the LCD
 *           transfer of frame N with the LZW decode of frame N+1:
 *
 *             SPI flash --(QSPI PDMA)--> LZW decode --> 8-bit index canvas
 *                        --(global-palette LUT)--> RGB565 TX buffer
 *                        --(SPI1 PDMA)--> ILI9341 TFT
 *
 *           Two separate buffers are kept so decoding the next frame does not
 *           disturb the pixels the background PDMA is still streaming:
 *             - g_index  : 8-bit palette-index canvas the library composites
 *                          into. Frame disposal (restore-to-background /
 *                          previous) and inter-frame transparency are handled
 *                          here by the library. A few extra lines of slack are
 *                          needed because, in callback mode, the library cooks
 *                          one scratch line just past the index canvas.
 *             - g_cooked : RGB565 image actually sent to the panel. After the
 *                          previous frame's transfer completes we cook the
 *                          whole index canvas into it (one global-palette LUT
 *                          pass), then start the next transfer.
 *
 *           NOTE: this cook uses a single (global) RGB565 palette captured from
 *           the GIFDRAW callback. It assumes the GIF uses one global palette
 *           for every frame (true for virtually all animated GIFs); a one-shot
 *           warning is printed if a local palette is ever seen.
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
   second. "Busy" is the CPU time spent decoding (gif.playFrame(), i.e. LZW
   decode + the QSPI-flash reads) plus the index->RGB565 cook. The LCD transfer
   runs in the background via PDMA and overlaps the decode, so
   CPU% = (decode + cook) / wall-clock. */
static uint32_t s_statWindowStart = 0;  /* window start, 100us ticks         */
static uint32_t s_statBusyTicks   = 0;  /* decode+cook CPU time this window   */
static uint32_t s_statFrames      = 0;  /* frames shown this window           */

/* 8-bit palette-index canvas the library composites each frame into. In
   callback mode the library also cooks one scratch RGB565 line to
   g_index[W*H ..], so a few lines of slack are appended (matches the library's
   own allocFrameBuf(), which reserves W*(H+3)). 4-byte aligned. */
#define FB_INDEX_SLACK   (CANVAS_W * 3)
static uint8_t  g_index[CANVAS_W * CANVAS_H + FB_INDEX_SLACK]
    __attribute__((aligned(4)));

/* RGB565 image streamed to the panel (separate from g_index so the background
   PDMA transfer of this frame is not corrupted while the next frame decodes
   into g_index). We cook g_index into this via g_pPal565. */
static uint16_t g_cooked[CANVAS_W * CANVAS_H]
    __attribute__((aligned(4)));

/* RGB565 global palette captured from the GIFDRAW callback. Stable across
   frames for a global-palette GIF; used to cook g_index -> g_cooked. */
static const uint16_t *g_pPal565  = NULL;
static int             g_localWarn = 0;   /* one-shot local-palette warning   */

static AnimatedGIF gif;

/* Player state machine (pipelined) :
     ST_PRIME       : decode + cook + start-transfer of the first frame.
     ST_DECODE_NEXT : decode the next frame into g_index while the previous
                      frame streams to the panel in the background.
     ST_WAIT_TX     : once the transfer finishes, cook the freshly decoded
                      frame and start transferring it. */
enum {
    ST_OPEN = 0,
    ST_PRIME,
    ST_DECODE_NEXT,
    ST_WAIT_TX,
    ST_ERROR
};
static int           s_state       = ST_OPEN;
static int           s_delayMs     = 0;
static int           s_pendLoopEnd = 0;   /* frame currently in g_cooked is last */
static int           s_nextLoopEnd = 0;   /* frame just decoded into g_index is last */

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

/* GIFDRAW callback. We do NOT use the per-line cooked pixels it hands us (those
   are unreliable for transparent pixels); installing a callback simply puts the
   library in "index-only framebuffer" mode so it composites the frame into
   g_index without needing a full cooked canvas. We only grab the stable RGB565
   global palette pointer here so we can cook g_index ourselves. */
static void GIF_Draw_CB(GIFDRAW *pDraw)
{
    g_pPal565 = pDraw->pPalette;
    if (!pDraw->ucIsGlobalPalette && !g_localWarn)
    {
        printf("WARN: GIF frame uses a local palette; pipelined cook assumes a "
               "single global palette, colours may be wrong\n");
        g_localWarn = 1;
    }
}

/*--------------------------------------------------------------------------*/
/* Helpers                                                                   */
/*--------------------------------------------------------------------------*/
static int GIF_OpenFile(void)
{
    gif.begin(GIF_PALETTE_RGB565_LE);
    FlashCache_Reset();

    /* Install GIF_Draw_CB so the library runs in index-only framebuffer mode:
       it composites each frame into g_index (handling disposal / transparency)
       but does not need a full cooked canvas appended to it. */
    if (gif.open((const char *)"flash",
                 GIF_Open_CB, GIF_Close_CB,
                 GIF_Read_CB, GIF_Seek_CB, GIF_Draw_CB) == 0)
    {
        printf("gif.open FAILED, lastError=%d\n", gif.getLastError());
        return 0;
    }

    printf("gif.open OK, canvas=%dx%d\n",
           gif.getCanvasWidth(), gif.getCanvasHeight());

    gif.setFrameBuf(g_index);
    gif.setDrawType(GIF_DRAW_COOKED);
    return 1;
}

/* Cook the composited 8-bit index canvas into the RGB565 transfer buffer using
   the global palette. Runs after the previous frame's transfer has finished, so
   g_cooked is free to overwrite. */
static void GIF_CookCanvas(void)
{
    const uint16_t *pal = g_pPal565;
    const uint8_t  *s   = g_index;
    uint16_t       *d   = g_cooked;
    int             n   = CANVAS_W * CANVAS_H;

    if (pal == NULL)                        /* no frame decoded yet */
        return;
    for (int i = 0; i < n; i++)
        d[i] = pal[s[i]];
}

/* Stream the RGB565 transfer buffer to the panel : one full-screen window,
   GRAM auto-increments across the whole 320x240 frame (background PDMA). */
static void GIF_StartBlit(void)
{
    LCD_SetWindow(0, 0, CANVAS_W - 1, CANVAS_H - 1);
    LCD_SendFrameStart(g_cooked, (uint32_t)CANVAS_W * CANVAS_H);
}

/* Once per second, report frame rate and CPU usage over the elapsed window. */
static void GIF_ReportStats(void)
{
    uint32_t elapsed = (uint32_t)(g_u32TimerCnt - s_statWindowStart);
    if (elapsed >= STAT_TICKS_PER_SEC)
    {
        uint32_t fps = (s_statFrames * STAT_TICKS_PER_SEC) / elapsed;
        uint32_t cpu = (s_statBusyTicks * 100u) / elapsed;
        printf("FPS=%lu  CPU=%lu%%\n", (unsigned long)fps, (unsigned long)cpu);
        s_statWindowStart = g_u32TimerCnt;
        s_statBusyTicks   = 0;
        s_statFrames      = 0;
    }
}

/*--------------------------------------------------------------------------*/
/* Public API                                                                */
/*--------------------------------------------------------------------------*/

extern "C" void GIF_Player_Init(void)
{
    memset(g_index, 0, sizeof(g_index));
    s_state           = ST_OPEN;
    s_pendLoopEnd     = 0;
    s_nextLoopEnd     = 0;
    g_pPal565         = NULL;
    g_localWarn       = 0;
    s_statWindowStart = g_u32TimerCnt;
    s_statBusyTicks   = 0;
    s_statFrames      = 0;
}

extern "C" void GIF_Player_Task(void)
{
    switch (s_state)
    {
    case ST_OPEN:
        s_state = GIF_OpenFile() ? ST_PRIME : ST_ERROR;
        break;

    case ST_PRIME:
    {
        /* Decode, cook and start transferring the very first frame. This primes
           the pipeline: from here on a transfer is always in flight while the
           next frame decodes. */
        uint32_t t0 = g_u32TimerCnt;
        int rc = gif.playFrame(false, &s_delayMs, NULL);
        GIF_CookCanvas();
        s_statBusyTicks += (uint32_t)(g_u32TimerCnt - t0);
        s_statFrames++;

        s_pendLoopEnd = (rc <= 0);
        GIF_StartBlit();
        s_state = ST_DECODE_NEXT;
        break;
    }

    case ST_DECODE_NEXT:
    {
        /* Decode the next frame into g_index while the current frame streams to
           the panel in the background (PDMA). This decode is the overlap. */
        uint32_t t0;
        int rc;

        if (s_pendLoopEnd)                  /* current frame was the last one */
        {
            gif.reset();                    /* restart from frame 0           */
            s_pendLoopEnd = 0;
        }

        t0 = g_u32TimerCnt;
        rc = gif.playFrame(false, &s_delayMs, NULL);
        s_statBusyTicks += (uint32_t)(g_u32TimerCnt - t0);

        s_nextLoopEnd = (rc <= 0);
        s_state = ST_WAIT_TX;
        break;
    }

    case ST_WAIT_TX:
        if (LCD_SendFrameDone())            /* previous transfer finished     */
        {
            uint32_t t0 = g_u32TimerCnt;
            GIF_CookCanvas();               /* cook the freshly decoded frame */
            s_statBusyTicks += (uint32_t)(g_u32TimerCnt - t0);
            s_statFrames++;

            GIF_StartBlit();                /* start streaming it             */
            s_pendLoopEnd = s_nextLoopEnd;
            GIF_ReportStats();              /* prints while the PDMA runs     */
            s_state = ST_DECODE_NEXT;
        }
        break;

    case ST_ERROR:
    default:
        s_state = ST_OPEN;                  /* retry opening the GIF          */
        break;
    }
}
