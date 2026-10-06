/**************************************************************************//**
 * @file     spi_flash.c
 * @brief    SPI NOR flash driver (single-line) over QSPI0 using PC0~PC5 pins.
 *
 * SPDX-License-Identifier: Apache-2.0
 ******************************************************************************/
#include <stdio.h>
#include "NuMicro.h"
#include "spi_flash.h"

/* Wait until QSPI transfer engine is idle (with time-out). */
__STATIC_INLINE void SpiFlash_WaitBusy(void)
{
    uint32_t u32TimeOutCnt = SystemCoreClock; /* about 1 second */

    while (QSPI_IS_BUSY(SPI_FLASH_PORT))
    {
        if (--u32TimeOutCnt == 0)
        {
            printf("Wait for QSPI time-out!\n");
            break;
        }
    }
}

void SpiFlash_Init(void)
{
    /* Configure QSPI0 as a master, MSB first, 8-bit transaction, SPI Mode-0. */
    QSPI_Open(SPI_FLASH_PORT, QSPI_MASTER, QSPI_MODE_0, 8, FLASH_QSPI_CLK_FREQ);

    /* Disable auto SS so we can control /CS manually around each command. */
    QSPI_DisableAutoSS(SPI_FLASH_PORT);

    /* Set TX/RX FIFO threshold. */
    QSPI_SetFIFO(SPI_FLASH_PORT, 7, 7);

    QSPI_ClearRxFIFO(SPI_FLASH_PORT);
}

uint32_t SpiFlash_ReadJedecID(void)
{
    uint8_t u8RxData[4] = {0}, u8IDCnt = 0;

    QSPI_ClearRxFIFO(SPI_FLASH_PORT);

    /* /CS: active */
    QSPI_SET_SS_LOW(SPI_FLASH_PORT);

    /* Command 0x9F: Read JEDEC ID */
    QSPI_WRITE_TX(SPI_FLASH_PORT, 0x9F);

    /* Clock out 3 dummy bytes to shift in the 24-bit ID */
    QSPI_WRITE_TX(SPI_FLASH_PORT, 0x00);
    QSPI_WRITE_TX(SPI_FLASH_PORT, 0x00);
    QSPI_WRITE_TX(SPI_FLASH_PORT, 0x00);

    SpiFlash_WaitBusy();

    /* /CS: de-active */
    QSPI_SET_SS_HIGH(SPI_FLASH_PORT);

    while (!QSPI_GET_RX_FIFO_EMPTY_FLAG(SPI_FLASH_PORT) && (u8IDCnt < 4))
        u8RxData[u8IDCnt++] = (uint8_t)QSPI_READ_RX(SPI_FLASH_PORT);

    /* u8RxData[0] is the dummy byte shifted in during the command byte. */
    return (uint32_t)((u8RxData[1] << 16) | (u8RxData[2] << 8) | u8RxData[3]);
}

/* Issue Write Enable (0x06). */
static void SpiFlash_WriteEnable(void)
{
    QSPI_SET_SS_LOW(SPI_FLASH_PORT);
    QSPI_WRITE_TX(SPI_FLASH_PORT, 0x06);
    SpiFlash_WaitBusy();
    QSPI_SET_SS_HIGH(SPI_FLASH_PORT);
}

/* Read Status Register-1 (0x05). */
static uint8_t SpiFlash_ReadStatusReg(void)
{
    uint8_t u8Val;

    QSPI_ClearRxFIFO(SPI_FLASH_PORT);

    QSPI_SET_SS_LOW(SPI_FLASH_PORT);
    QSPI_WRITE_TX(SPI_FLASH_PORT, 0x05);
    QSPI_WRITE_TX(SPI_FLASH_PORT, 0x00);
    SpiFlash_WaitBusy();
    QSPI_SET_SS_HIGH(SPI_FLASH_PORT);

    /* First RX byte is dummy (clocked in during the command byte). */
    (void)QSPI_READ_RX(SPI_FLASH_PORT);
    u8Val = (uint8_t)QSPI_READ_RX(SPI_FLASH_PORT);

    return u8Val;
}

/* Read Status Register-2 (0x35). */
static uint8_t SpiFlash_ReadStatusReg2(void)
{
    uint8_t u8Val;

    QSPI_ClearRxFIFO(SPI_FLASH_PORT);

    QSPI_SET_SS_LOW(SPI_FLASH_PORT);
    QSPI_WRITE_TX(SPI_FLASH_PORT, 0x35);
    QSPI_WRITE_TX(SPI_FLASH_PORT, 0x00);
    SpiFlash_WaitBusy();
    QSPI_SET_SS_HIGH(SPI_FLASH_PORT);

    /* First RX byte is dummy (clocked in during the command byte). */
    (void)QSPI_READ_RX(SPI_FLASH_PORT);
    u8Val = (uint8_t)QSPI_READ_RX(SPI_FLASH_PORT);

    return u8Val;
}

/* Write Status Register (0x01): writes SR1 (and SR2 when supported) in one
 * command. Most Winbond/GigaDevice/etc. parts accept the 2-byte form, which
 * also keeps SR2 (e.g. the CMP/QE bits) consistent. */
static void SpiFlash_WriteStatusReg(uint8_t u8Sr1, uint8_t u8Sr2)
{
    SpiFlash_WriteEnable();

    QSPI_SET_SS_LOW(SPI_FLASH_PORT);
    QSPI_WRITE_TX(SPI_FLASH_PORT, 0x01);
    QSPI_WRITE_TX(SPI_FLASH_PORT, u8Sr1);
    QSPI_WRITE_TX(SPI_FLASH_PORT, u8Sr2);
    SpiFlash_WaitBusy();
    QSPI_SET_SS_HIGH(SPI_FLASH_PORT);

    QSPI_ClearRxFIFO(SPI_FLASH_PORT);

    SpiFlash_WaitReady();
}

int32_t SpiFlash_WaitReady(void)
{
    volatile uint8_t u8Status;
    uint32_t u32TimeOutCnt = SystemCoreClock; /* about 1 second */

    do
    {
        if (--u32TimeOutCnt == 0)
        {
            printf("SpiFlash_WaitReady time-out!\n");
            return -1;
        }

        u8Status = SpiFlash_ReadStatusReg() & 0x01; /* BUSY bit */
    }
    while (u8Status != 0);

    return 0;
}

int32_t SpiFlash_ClearWriteProtect(void)
{
    uint8_t u8Sr1, u8Sr2;

    /* Make sure the device is idle before touching the status registers. */
    if (SpiFlash_WaitReady() != 0)
        return -1;

    u8Sr1 = SpiFlash_ReadStatusReg();
    u8Sr2 = SpiFlash_ReadStatusReg2();

    /* SR1 protection-related bits:
     *   bit7 SRP0, bit6 SEC(/BP3), bit5 TB, bit4 BP2, bit3 BP1, bit2 BP0.
     * SR2 protection-related bits:
     *   bit6 CMP (complements the SR1 block-protect range),
     *   bit0 SRP1 (together with SRP0 selects the lock mode).
     * Keep every other bit (e.g. QE) untouched. */
    if (((u8Sr1 & 0xFC) == 0) && ((u8Sr2 & 0x41) == 0))
        return 0; /* Nothing is protected. */

    printf("Flash write-protected (SR1=0x%02X SR2=0x%02X), clearing...\n",
           u8Sr1, u8Sr2);

    u8Sr1 &= (uint8_t)~0xFC; /* Clear SRP0, SEC, TB, BP2..BP0 */
    u8Sr2 &= (uint8_t)~0x41; /* Clear CMP and SRP1           */

    SpiFlash_WriteStatusReg(u8Sr1, u8Sr2);

    /* Verify the protection bits really cleared. */
    u8Sr1 = SpiFlash_ReadStatusReg();
    u8Sr2 = SpiFlash_ReadStatusReg2();

    if (((u8Sr1 & 0xFC) != 0) || ((u8Sr2 & 0x41) != 0))
    {
        printf("Failed to clear write protect (SR1=0x%02X SR2=0x%02X).\n",
               u8Sr1, u8Sr2);
        return -1;
    }

    printf("Write protect cleared.\n");
    return 0;
}

void SpiFlash_SectorErase4K(uint32_t u32Addr)
{
    /* Align the address to the 4 KB sector boundary. */
    u32Addr &= ~(uint32_t)(FLASH_SECTOR_SIZE - 1);

    SpiFlash_WriteEnable();

    QSPI_SET_SS_LOW(SPI_FLASH_PORT);

    /* Command 0x20: Sector Erase (4 KB) */
    QSPI_WRITE_TX(SPI_FLASH_PORT, 0x20);
    QSPI_WRITE_TX(SPI_FLASH_PORT, (u32Addr >> 16) & 0xFF);
    QSPI_WRITE_TX(SPI_FLASH_PORT, (u32Addr >> 8)  & 0xFF);
    QSPI_WRITE_TX(SPI_FLASH_PORT,  u32Addr        & 0xFF);

    SpiFlash_WaitBusy();
    QSPI_SET_SS_HIGH(SPI_FLASH_PORT);

    QSPI_ClearRxFIFO(SPI_FLASH_PORT);

    SpiFlash_WaitReady();
}

void SpiFlash_ChipErase(void)
{
    SpiFlash_WriteEnable();

    QSPI_SET_SS_LOW(SPI_FLASH_PORT);
    QSPI_WRITE_TX(SPI_FLASH_PORT, 0xC7); /* Chip Erase */
    SpiFlash_WaitBusy();
    QSPI_SET_SS_HIGH(SPI_FLASH_PORT);

    QSPI_ClearRxFIFO(SPI_FLASH_PORT);

    SpiFlash_WaitReady();
}

void SpiFlash_PageProgram256(uint32_t u32Addr, uint8_t *pu8Data, uint32_t u32Len)
{
    uint32_t u32Cnt = 0;

    if (u32Len == 0) return;
    if (u32Len > FLASH_PAGE_SIZE) u32Len = FLASH_PAGE_SIZE;

    SpiFlash_WriteEnable();

    QSPI_SET_SS_LOW(SPI_FLASH_PORT);

    /* Command 0x02: Page Program */
    QSPI_WRITE_TX(SPI_FLASH_PORT, 0x02);
    QSPI_WRITE_TX(SPI_FLASH_PORT, (u32Addr >> 16) & 0xFF);
    QSPI_WRITE_TX(SPI_FLASH_PORT, (u32Addr >> 8)  & 0xFF);
    QSPI_WRITE_TX(SPI_FLASH_PORT,  u32Addr        & 0xFF);

    while (u32Cnt < u32Len)
    {
        if (!QSPI_GET_TX_FIFO_FULL_FLAG(SPI_FLASH_PORT))
            QSPI_WRITE_TX(SPI_FLASH_PORT, pu8Data[u32Cnt++]);
    }

    SpiFlash_WaitBusy();
    QSPI_SET_SS_HIGH(SPI_FLASH_PORT);

    QSPI_ClearRxFIFO(SPI_FLASH_PORT);

    SpiFlash_WaitReady();
}

void SpiFlash_Read(uint32_t u32Addr, uint8_t *pu8Data, uint32_t u32Len)
{
    uint32_t u32Cnt = 0;

    if (u32Len == 0) return;

    QSPI_ClearRxFIFO(SPI_FLASH_PORT);

    QSPI_SET_SS_LOW(SPI_FLASH_PORT);

    /* Command 0x03: Read Data */
    QSPI_WRITE_TX(SPI_FLASH_PORT, 0x03);
    QSPI_WRITE_TX(SPI_FLASH_PORT, (u32Addr >> 16) & 0xFF);
    QSPI_WRITE_TX(SPI_FLASH_PORT, (u32Addr >> 8)  & 0xFF);
    QSPI_WRITE_TX(SPI_FLASH_PORT,  u32Addr        & 0xFF);

    /* Drain the 4 bytes clocked in during command + address phase. */
    SpiFlash_WaitBusy();
    while (!QSPI_GET_RX_FIFO_EMPTY_FLAG(SPI_FLASH_PORT))
        (void)QSPI_READ_RX(SPI_FLASH_PORT);

    while (u32Cnt < u32Len)
    {
        /* Clock out a dummy byte to shift in one data byte. */
        if (!QSPI_GET_TX_FIFO_FULL_FLAG(SPI_FLASH_PORT))
            QSPI_WRITE_TX(SPI_FLASH_PORT, 0x00);

        if (!QSPI_GET_RX_FIFO_EMPTY_FLAG(SPI_FLASH_PORT))
            pu8Data[u32Cnt++] = (uint8_t)QSPI_READ_RX(SPI_FLASH_PORT);
    }

    SpiFlash_WaitBusy();
    QSPI_SET_SS_HIGH(SPI_FLASH_PORT);

    QSPI_ClearRxFIFO(SPI_FLASH_PORT);
}
