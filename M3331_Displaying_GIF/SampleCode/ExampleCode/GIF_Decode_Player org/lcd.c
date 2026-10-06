/**************************************************************************//**
 * @file     lcd.c
 * @brief    ILI9341 TFT driver over SPI1. Register access uses 8-bit transfers;
 *           pixel data is streamed as 16-bit words through PDMA0 channel 0.
 *
 * @copyright SPDX-License-Identifier: Apache-2.0
 ******************************************************************************/
#include "NuMicro.h"
#include "lcd.h"

/* PDMA0 channel used for the LCD TX path */
#define PDMA_LCD_CH         0

/* Set to 1 by PDMA0_IRQHandler (main.c) when the LCD TX transfer completes.
   Starts "idle" so the first band can be launched. */
volatile uint8_t g_LcdDmaDone = 1;

#define LCD_DC_CLR()        (PH10 = 0)
#define LCD_DC_SET()        (PH10 = 1)

/*--------------------------------------------------------------------------*/
/* Low level register / data writes (current SPI data width)                */
/*--------------------------------------------------------------------------*/
static void LCD_Write_Reg(uint8_t u8Cmd)
{
    LCD_DC_CLR();                               /* command */
    SPI_WRITE_TX(SPI_LCD_PORT, u8Cmd);
    while (SPI_IS_BUSY(SPI_LCD_PORT)) {}
    LCD_DC_SET();                               /* back to data */
}

static void LCD_Write_Data(uint16_t u16Dat)
{
    SPI_WRITE_TX(SPI_LCD_PORT, u16Dat);
    while (SPI_IS_BUSY(SPI_LCD_PORT)) {}
}

/* Reconfigure the SPI1 transfer width (8 for commands/addresses, 16 for pixels) */
static void LCD_SetBusWidth(uint32_t u32Bits)
{
    SPI_Close(SPI_LCD_PORT);
    while ((SPI_LCD_PORT->STATUS & SPI_STATUS_SPIENSTS_Msk) != 0) {}
    SPI_SET_DATA_WIDTH(SPI_LCD_PORT, u32Bits);
    SPI_LCD_PORT->CTL |= (SPI_CTL_SPIEN_Msk);
}

/*--------------------------------------------------------------------------*/
/* Public API                                                               */
/*--------------------------------------------------------------------------*/
void SPI_LCD_Init(void)
{
    SPI_Close(SPI_LCD_PORT);
    while ((SPI_LCD_PORT->STATUS & SPI_STATUS_SPIENSTS_Msk) != 0) {}

    /* Master, mode 0, 8-bit, SPI clock = LCD_SPI_CLK_FREQ */
    SPI_Open(SPI_LCD_PORT, SPI_MASTER, SPI_MODE_0, 8, LCD_SPI_CLK_FREQ);
    /* Clear the suspend interval so consecutive words are shifted out
       back-to-back (matches the Nuvoton emWin ILI9341 reference driver). */
    SPI_LCD_PORT->CTL &= ~SPI_CTL_SUSPITV_Msk;
    SPI_EnableAutoSS(SPI_LCD_PORT, SPI_SS, SPI_SS_ACTIVE_LOW);
    SPI_SetFIFO(SPI_LCD_PORT, 3, 3);
    NVIC_DisableIRQ(SPI1_IRQn);
    SPI_LCD_PORT->CTL |= (SPI_CTL_SPIEN_Msk);
}

void LCD_Init(void)
{
    LCD_RST = 1;
    LCD_EN  = 0;

    /* Hardware reset */
    LCD_RST = 1;
    CLK_SysTickLongDelay(5000);
    LCD_RST = 0;
    CLK_SysTickLongDelay(20000);
    LCD_RST = 1;
    CLK_SysTickLongDelay(40000);

    LCD_Write_Reg(0xCB);
    LCD_Write_Data(0x39); LCD_Write_Data(0x2C); LCD_Write_Data(0x00);
    LCD_Write_Data(0x34); LCD_Write_Data(0x02);

    LCD_Write_Reg(0xCF);
    LCD_Write_Data(0x00); LCD_Write_Data(0xC1); LCD_Write_Data(0x30);

    LCD_Write_Reg(0xE8);
    LCD_Write_Data(0x85); LCD_Write_Data(0x00); LCD_Write_Data(0x78);

    LCD_Write_Reg(0xEA);
    LCD_Write_Data(0x00); LCD_Write_Data(0x00);

    LCD_Write_Reg(0xED);
    LCD_Write_Data(0x64); LCD_Write_Data(0x03); LCD_Write_Data(0x12);
    LCD_Write_Data(0x81);

    LCD_Write_Reg(0xF7);
    LCD_Write_Data(0x20);

    LCD_Write_Reg(0xC0);
    LCD_Write_Data(0x23);

    LCD_Write_Reg(0xC1);
    LCD_Write_Data(0x10);

    LCD_Write_Reg(0xC5);
    LCD_Write_Data(0x3e); LCD_Write_Data(0x28);

    LCD_Write_Reg(0xC7);
    LCD_Write_Data(0x86);

    /* MADCTL : 0xE8 -> 320x240 landscape (BGR) */
    LCD_Write_Reg(0x36);
    LCD_Write_Data(0xE8);

    LCD_Write_Reg(0x3A);
    LCD_Write_Data(0x55);                        /* 16 bit/pixel */

    LCD_Write_Reg(0xB1);
    LCD_Write_Data(0x00); LCD_Write_Data(0x18);

    LCD_Write_Reg(0xB6);
    LCD_Write_Data(0x08); LCD_Write_Data(0x82); LCD_Write_Data(0x27);

    LCD_Write_Reg(0xF2);
    LCD_Write_Data(0x00);

    LCD_Write_Reg(0x26);
    LCD_Write_Data(0x01);

    LCD_Write_Reg(0xE0);
    LCD_Write_Data(0x0F); LCD_Write_Data(0x31); LCD_Write_Data(0x2B);
    LCD_Write_Data(0x0C); LCD_Write_Data(0x0E); LCD_Write_Data(0x08);
    LCD_Write_Data(0x4E); LCD_Write_Data(0xF1); LCD_Write_Data(0x37);
    LCD_Write_Data(0x07); LCD_Write_Data(0x10); LCD_Write_Data(0x03);
    LCD_Write_Data(0x0E); LCD_Write_Data(0x09); LCD_Write_Data(0x00);

    LCD_Write_Reg(0xE1);
    LCD_Write_Data(0x00); LCD_Write_Data(0x0E); LCD_Write_Data(0x14);
    LCD_Write_Data(0x03); LCD_Write_Data(0x11); LCD_Write_Data(0x07);
    LCD_Write_Data(0x31); LCD_Write_Data(0xC1); LCD_Write_Data(0x48);
    LCD_Write_Data(0x08); LCD_Write_Data(0x0F); LCD_Write_Data(0x0C);
    LCD_Write_Data(0x31); LCD_Write_Data(0x36); LCD_Write_Data(0x0F);

    LCD_Write_Reg(0x11);                          /* sleep out */
    CLK_SysTickLongDelay(120000);
    LCD_Write_Reg(0x29);                          /* display on */

    LCD_EN = 1;
}

static void Address_Set(uint16_t x1, uint16_t x2, uint16_t y1, uint16_t y2)
{
    LCD_Write_Reg(0x2A);                          /* column */
    LCD_Write_Data(x1 >> 8); LCD_Write_Data(x1 & 0xFF);
    LCD_Write_Data(x2 >> 8); LCD_Write_Data(x2 & 0xFF);
    LCD_Write_Reg(0x2B);                          /* page */
    LCD_Write_Data(y1 >> 8); LCD_Write_Data(y1 & 0xFF);
    LCD_Write_Data(y2 >> 8); LCD_Write_Data(y2 & 0xFF);
    LCD_Write_Reg(0x2C);                          /* memory write */
}

void LCD_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    LCD_SetBusWidth(8);
    Address_Set(x0, x1, y0, y1);
    LCD_SetBusWidth(16);
}

void LCD_FillColor(uint16_t u16Color)
{
    uint32_t i;

    LCD_SetWindow(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    for (i = 0; i < (uint32_t)LCD_WIDTH * LCD_HEIGHT; i++)
        LCD_Write_Data(u16Color);
}

void LCD_SendBandStart(uint16_t *pBuf, uint16_t u16Count)
{
    g_LcdDmaDone = 0;

    PDMA_Open(PDMA0, (1 << PDMA_LCD_CH));
    PDMA_SetTransferCnt(PDMA0, PDMA_LCD_CH, PDMA_WIDTH_16, u16Count);
    PDMA_SetTransferAddr(PDMA0, PDMA_LCD_CH, (uint32_t)pBuf, PDMA_SAR_INC,
                         (uint32_t)&SPI_LCD_PORT->TX, PDMA_DAR_FIX);
    PDMA_SetTransferMode(PDMA0, PDMA_LCD_CH, PDMA_SPI1_TX, FALSE, 0);
    PDMA_SetBurstType(PDMA0, PDMA_LCD_CH, PDMA_REQ_SINGLE, PDMA_BURST_4);
    PDMA_EnableInt(PDMA0, PDMA_LCD_CH, PDMA_INT_TRANS_DONE);
    NVIC_EnableIRQ(PDMA0_IRQn);

    SPI_TRIGGER_TX_PDMA(SPI_LCD_PORT);
}

int LCD_SendBandDone(void)
{
    return (int)g_LcdDmaDone;
}

/*--------------------------------------------------------------------------*/
/* Whole-frame transfer : two PDMA halves chained in the TX done IRQ          */
/*--------------------------------------------------------------------------*/
/* A single PDMA transfer is capped at 65535 items, but a 320x240 frame is
   76800 pixels. The frame is therefore streamed as two back-to-back PDMA
   halves: the first half is kicked by LCD_SendFrameStart(), and when its
   transfer-done interrupt fires, LCD_DmaDoneHook() (called from the PDMA0
   handler in main.c) re-triggers the second half. The GRAM window is set once
   by the caller and auto-increments across both halves, so the stream is
   seamless. g_LcdDmaDone is only raised after the final half completes. */
static uint16_t *s_frameNext    = 0;   /* start of the pending 2nd half      */
static uint32_t  s_frameNextCnt = 0;   /* pixel count of the pending 2nd half */

static void LCD_PdmaKick(uint16_t *pBuf, uint32_t u32Count)
{
    SPI_DISABLE_TX_PDMA(SPI_LCD_PORT);

    PDMA_Open(PDMA0, (1 << PDMA_LCD_CH));
    PDMA_SetTransferCnt(PDMA0, PDMA_LCD_CH, PDMA_WIDTH_16, u32Count);
    PDMA_SetTransferAddr(PDMA0, PDMA_LCD_CH, (uint32_t)pBuf, PDMA_SAR_INC,
                         (uint32_t)&SPI_LCD_PORT->TX, PDMA_DAR_FIX);
    PDMA_SetTransferMode(PDMA0, PDMA_LCD_CH, PDMA_SPI1_TX, FALSE, 0);
    PDMA_SetBurstType(PDMA0, PDMA_LCD_CH, PDMA_REQ_SINGLE, PDMA_BURST_4);
    PDMA_EnableInt(PDMA0, PDMA_LCD_CH, PDMA_INT_TRANS_DONE);
    NVIC_EnableIRQ(PDMA0_IRQn);

    SPI_TRIGGER_TX_PDMA(SPI_LCD_PORT);
}

void LCD_SendFrameStart(uint16_t *pBuf, uint32_t u32PixelCount)
{
    uint32_t u32First;

    g_LcdDmaDone = 0;

    if (u32PixelCount > 65535u)
    {
        u32First        = u32PixelCount / 2u;      /* split into two halves */
        s_frameNext     = pBuf + u32First;
        s_frameNextCnt  = u32PixelCount - u32First;
    }
    else
    {
        u32First        = u32PixelCount;           /* fits in one transfer  */
        s_frameNextCnt  = 0;
    }

    LCD_PdmaKick(pBuf, u32First);
}

/* Called from PDMA0_IRQHandler (main.c) on a channel-0 transfer-done. Kicks the
   pending second half if there is one; otherwise marks the frame complete.
   Returns non-zero once the whole frame has finished. */
int LCD_DmaDoneHook(void)
{
    if (s_frameNextCnt != 0u)
    {
        uint16_t *pNext = s_frameNext;
        uint32_t  cnt   = s_frameNextCnt;
        s_frameNextCnt  = 0;
        LCD_PdmaKick(pNext, cnt);       /* stream the 2nd half */
        return 0;
    }

    SPI_DISABLE_TX_PDMA(SPI_LCD_PORT);
    g_LcdDmaDone = 1;
    return 1;
}

int LCD_SendFrameDone(void)
{
    return (int)g_LcdDmaDone;
}
