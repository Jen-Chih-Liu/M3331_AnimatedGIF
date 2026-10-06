/**************************************************************************//**
 * @file     lcd.h
 * @brief    ILI9341 TFT driver over SPI1 with PDMA (channel 0) pixel streaming.
 *
 * @copyright SPDX-License-Identifier: Apache-2.0
 ******************************************************************************/
#ifndef __LCD_H__
#define __LCD_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SPI controller connected to the LCD */
#define SPI_LCD_PORT        SPI1
/* 60 MHz was marginal and produced progressive line skew on detailed content
   (solid fills hid the dropped bits). 48 MHz trades some margin for a higher
   frame rate; drop back to 30 MHz if line skew / tearing reappears. */
#define LCD_SPI_CLK_FREQ    48000000

/* Panel geometry (landscape) */
#define LCD_WIDTH           320
#define LCD_HEIGHT          240

/* Control pins */
#define LCD_RST             PD14
#define LCD_DC              PH10
#define LCD_EN              PH11

/**
 * @brief Initialise SPI1 for the LCD. QSPI/SPI pin mux and module clock must
 *        already be configured in SYS_Init().
 */
void SPI_LCD_Init(void);

/** @brief Run the ILI9341 power-on / gamma register initialisation sequence. */
void LCD_Init(void);

/** @brief Fill the whole panel with a single RGB565 colour (blocking). */
void LCD_FillColor(uint16_t u16Color);

/**
 * @brief Set the GRAM access window to (x0,y0)-(x1,y1) and issue the memory
 *        write command (0x2C). Leaves SPI1 in 16-bit mode ready for pixel data.
 */
void LCD_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

/**
 * @brief Start a non-blocking PDMA transfer of @p u16Count RGB565 pixels from
 *        @p pBuf to the LCD. Poll LCD_SendBandDone() for completion.
 */
void LCD_SendBandStart(uint16_t *pBuf, uint16_t u16Count);

/** @brief Returns non-zero once the last LCD_SendBandStart() transfer finished. */
int  LCD_SendBandDone(void);

/**
 * @brief Start streaming an ENTIRE frame (@p u32PixelCount RGB565 pixels from
 *        @p pBuf) to the panel. The frame is sent as two PDMA halves chained in
 *        the transfer-done interrupt (a single PDMA transfer is capped at 65535
 *        items). The GRAM window must already be set (LCD_SetWindow); it is left
 *        untouched between halves so the stream is seamless. Poll
 *        LCD_SendFrameDone() for completion.
 */
void LCD_SendFrameStart(uint16_t *pBuf, uint32_t u32PixelCount);

/** @brief Returns non-zero once the last LCD_SendFrameStart() transfer finished. */
int  LCD_SendFrameDone(void);

/**
 * @brief Called from the PDMA0 channel-0 transfer-done interrupt (in main.c).
 *        Chains the pending second half of a whole-frame transfer, or marks the
 *        frame complete. Returns non-zero once the whole frame has finished.
 */
int  LCD_DmaDoneHook(void);

#ifdef __cplusplus
}
#endif

#endif /* __LCD_H__ */
