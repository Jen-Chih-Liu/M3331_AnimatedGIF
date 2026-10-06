/**************************************************************************//**
 * @file     qspi_flash.c
 * @brief    QSPI0 + PDMA SPI-NOR flash reader with a 2 KB block cache.
 *           The GIF decoder issues random-access reads through FlashCache_Read();
 *           each cache miss performs a 0xEB Quad I/O Fast Read of one 2 KB block
 *           over QSPI0 using PDMA channel 1 (32-bit, RX).
 *
 * @copyright SPDX-License-Identifier: Apache-2.0
 ******************************************************************************/
#include <string.h>
#include "NuMicro.h"
#include "qspi_flash.h"

/* PDMA0 channel used for the flash RX path */
#define PDMA_FLASH_CH           1

/* Set to 1 by PDMA0_IRQHandler (main.c) when the flash RX transfer completes */
volatile uint8_t g_FlashDmaDone = 0;

/* One cache line and the flash address it currently holds (0xFFFFFFFF = empty) */
static uint8_t  s_cache[FLASH_CACHE_BLOCK];
static uint32_t s_cacheAddr = 0xFFFFFFFFu;

/*--------------------------------------------------------------------------*/
/* Low level helpers                                                        */
/*--------------------------------------------------------------------------*/
static void Wait_QSPI_Busy(void)
{
    uint32_t u32TimeOut = SystemCoreClock; /* ~1 second guard */

    while (QSPI_IS_BUSY(QSPI_FLASH_PORT))
    {
        if (--u32TimeOut == 0)
            break;
    }
}

/* Switch PC4/PC5 (flash IO2/IO3) to GPIO driven high (single-line command phase). */
static void D2D3_SwitchToNormalMode(void)
{
    SYS->GPC_MFP1 = SYS->GPC_MFP1 & ~(SYS_GPC_MFP1_PC4MFP_Msk | SYS_GPC_MFP1_PC5MFP_Msk);
    GPIO_SetMode(PC, BIT4, GPIO_MODE_OUTPUT);
    GPIO_SetMode(PC, BIT5, GPIO_MODE_OUTPUT);
    PC4 = 1;
    PC5 = 1;
}

/* Switch PC4/PC5 back to QSPI0 MOSI1/MISO1 for the quad data phase. */
static void D2D3_SwitchToQuadMode(void)
{
    GPIO_SetSlewCtl(PC, BIT4 | BIT5, GPIO_SLEWCTL_FAST);
    SET_QSPI0_MOSI1_PC4();
    SET_QSPI0_MISO1_PC5();
}

static uint8_t SpiFlash_ReadStatusReg(uint8_t u8Cmd)
{
    uint8_t u8Val;

    QSPI_ClearRxFIFO(QSPI_FLASH_PORT);
    QSPI_SET_SS_LOW(QSPI_FLASH_PORT);
    QSPI_WRITE_TX(QSPI_FLASH_PORT, u8Cmd);
    QSPI_WRITE_TX(QSPI_FLASH_PORT, 0x00);
    Wait_QSPI_Busy();
    QSPI_SET_SS_HIGH(QSPI_FLASH_PORT);

    (void)QSPI_READ_RX(QSPI_FLASH_PORT);           /* discard byte shifted during command */
    u8Val = (uint8_t)QSPI_READ_RX(QSPI_FLASH_PORT);
    return u8Val;
}

static int32_t SpiFlash_WaitReady(void)
{
    uint32_t u32TimeOut = SystemCoreClock;

    while (SpiFlash_ReadStatusReg(0x05) & 0x01) /* BUSY bit */
    {
        if (--u32TimeOut == 0)
            return -1;
    }
    return 0;
}

static void SpiFlash_WriteStatusReg(uint8_t u8Value1, uint8_t u8Value2)
{
    /* Write Enable (0x06) */
    QSPI_SET_SS_LOW(QSPI_FLASH_PORT);
    QSPI_WRITE_TX(QSPI_FLASH_PORT, 0x06);
    Wait_QSPI_Busy();
    QSPI_SET_SS_HIGH(QSPI_FLASH_PORT);

    /* Write Status Register (0x01) */
    QSPI_SET_SS_LOW(QSPI_FLASH_PORT);
    QSPI_WRITE_TX(QSPI_FLASH_PORT, 0x01);
    QSPI_WRITE_TX(QSPI_FLASH_PORT, u8Value1);
    QSPI_WRITE_TX(QSPI_FLASH_PORT, u8Value2);
    Wait_QSPI_Busy();
    QSPI_SET_SS_HIGH(QSPI_FLASH_PORT);
}

/* Set the QE bit (Status Register-2 bit 1) so 0xEB quad read works. */
static void SpiFlash_EnableQuad(void)
{
    uint8_t u8Status1 = SpiFlash_ReadStatusReg(0x05);
    uint8_t u8Status2 = SpiFlash_ReadStatusReg(0x35);

    if ((u8Status2 & 0x02) == 0)
    {
        u8Status2 |= 0x02;
        SpiFlash_WriteStatusReg(u8Status1, u8Status2);
        SpiFlash_WaitReady();
    }
}

/*--------------------------------------------------------------------------*/
/* Public API                                                               */
/*--------------------------------------------------------------------------*/
void QSPI_Flash_Init(void)
{
    QSPI_FLASH_PORT->CTL &= (~QSPI_CTL_SPIEN_Msk);
    while ((QSPI_FLASH_PORT->STATUS & QSPI_STATUS_SPIENSTS_Msk) != 0) {}

    /* Master, mode 0, 8-bit, SPI clock = FLASH_QSPI_CLK_FREQ */
    QSPI_Open(QSPI_FLASH_PORT, QSPI_MASTER, QSPI_MODE_0, 8, FLASH_QSPI_CLK_FREQ);

    /* Control /SS manually */
    QSPI_DisableAutoSS(QSPI_FLASH_PORT);
    QSPI_SetFIFO(QSPI_FLASH_PORT, 7, 7);
    NVIC_DisableIRQ(QSPI0_IRQn);
    QSPI_FLASH_PORT->CTL |= (QSPI_CTL_SPIEN_Msk);

    /* Drive IO2/IO3 high and enable the flash quad mode once */
    D2D3_SwitchToNormalMode();
    SpiFlash_EnableQuad();

    s_cacheAddr = 0xFFFFFFFFu;
}

void FlashCache_Reset(void)
{
    s_cacheAddr = 0xFFFFFFFFu;
}

/* Read exactly FLASH_CACHE_BLOCK bytes starting at @p u32Addr into @p pu8Dst. */
static void QSPI_Flash_ReadBlock(uint32_t u32Addr, uint8_t *pu8Dst)
{
    uint32_t u32Words = FLASH_CACHE_BLOCK >> 2; /* 32-bit PDMA transfers */

    /* ---- Command / address phase : single line, 8-bit ---- */
    QSPI_Close(QSPI_FLASH_PORT);
    while ((QSPI_FLASH_PORT->STATUS & QSPI_STATUS_SPIENSTS_Msk) != 0) {}
    QSPI_DISABLE_QUAD_MODE(QSPI_FLASH_PORT);
    QSPI_SET_DATA_WIDTH(QSPI_FLASH_PORT, 8);
    QSPI_FLASH_PORT->CTL |= (QSPI_CTL_SPIEN_Msk);

    QSPI_SET_SS_LOW(QSPI_FLASH_PORT);

    /* 0xEB : Quad I/O Fast Read */
    QSPI_WRITE_TX(QSPI_FLASH_PORT, 0xEB);
    Wait_QSPI_Busy();

    /* Address + mode/dummy bytes are sent on all four lines */
    D2D3_SwitchToQuadMode();
    QSPI_ENABLE_QUAD_OUTPUT_MODE(QSPI_FLASH_PORT);
    QSPI_WRITE_TX(QSPI_FLASH_PORT, (u32Addr >> 16) & 0xFF);
    QSPI_WRITE_TX(QSPI_FLASH_PORT, (u32Addr >> 8)  & 0xFF);
    QSPI_WRITE_TX(QSPI_FLASH_PORT,  u32Addr        & 0xFF);
    QSPI_WRITE_TX(QSPI_FLASH_PORT, 0x00); /* mode byte  */
    QSPI_WRITE_TX(QSPI_FLASH_PORT, 0x00); /* dummy      */
    QSPI_WRITE_TX(QSPI_FLASH_PORT, 0x00); /* dummy      */
    Wait_QSPI_Busy();
    QSPI_ENABLE_QUAD_INPUT_MODE(QSPI_FLASH_PORT);

    /* ---- Data phase : switch controller to 32-bit and stream the whole block
            into SRAM with QSPI0 RX PDMA (channel 1). Triggering RX PDMA makes
            the master auto-generate the quad clocks; no TX writes are needed.
            The first received byte lands in the MSB of each 32-bit word, so a
            __REV() pass restores flash byte order (matching the old path). ---- */
    QSPI_Close(QSPI_FLASH_PORT);
    while ((QSPI_FLASH_PORT->STATUS & QSPI_STATUS_SPIENSTS_Msk) != 0) {}
    QSPI_SET_DATA_WIDTH(QSPI_FLASH_PORT, 32);
    QSPI_FLASH_PORT->CTL |= (QSPI_CTL_SPIEN_Msk);

    QSPI_ClearRxFIFO(QSPI_FLASH_PORT);

    PDMA_Open(PDMA0, (1 << PDMA_FLASH_CH));
    PDMA_SetTransferCnt(PDMA0, PDMA_FLASH_CH, PDMA_WIDTH_32, u32Words);
    PDMA_SetTransferAddr(PDMA0, PDMA_FLASH_CH, (uint32_t)&QSPI_FLASH_PORT->RX,
                         PDMA_SAR_FIX, (uint32_t)pu8Dst, PDMA_DAR_INC);
    PDMA_SetTransferMode(PDMA0, PDMA_FLASH_CH, PDMA_QSPI0_RX, FALSE, 0);
    PDMA_SetBurstType(PDMA0, PDMA_FLASH_CH, PDMA_REQ_SINGLE, 0);
    PDMA_EnableInt(PDMA0, PDMA_FLASH_CH, PDMA_INT_TRANS_DONE);
    NVIC_EnableIRQ(PDMA0_IRQn);

    g_FlashDmaDone = 0;
    QSPI_TRIGGER_RX_PDMA(QSPI_FLASH_PORT);
    while (g_FlashDmaDone == 0) {}
    QSPI_DISABLE_RX_PDMA(QSPI_FLASH_PORT);

    /* Restore flash byte order (RX word has the first byte in the MSB). */
    {
        uint32_t *pu32 = (uint32_t *)pu8Dst;
        uint32_t  i;
        for (i = 0; i < u32Words; i++)
            pu32[i] = __REV(pu32[i]);
    }

    /* ---- End the read : raise /CS and return IO2/IO3 to idle-high ---- */
    QSPI_DISABLE_QUAD_MODE(QSPI_FLASH_PORT);
    QSPI_SET_SS_HIGH(QSPI_FLASH_PORT);
    D2D3_SwitchToNormalMode();
}

void FlashCache_Read(uint32_t u32Addr, uint8_t *pu8Dst, uint32_t u32Len)
{
    while (u32Len)
    {
        uint32_t u32Block = u32Addr & ~(FLASH_CACHE_BLOCK - 1u);
        uint32_t u32Off   = u32Addr - u32Block;
        uint32_t u32Chunk = FLASH_CACHE_BLOCK - u32Off;

        if (u32Block != s_cacheAddr)
        {
            QSPI_Flash_ReadBlock(u32Block, s_cache);
            s_cacheAddr = u32Block;
        }

        if (u32Chunk > u32Len)
            u32Chunk = u32Len;

        memcpy(pu8Dst, &s_cache[u32Off], u32Chunk);

        pu8Dst += u32Chunk;
        u32Addr += u32Chunk;
        u32Len  -= u32Chunk;
    }
}
