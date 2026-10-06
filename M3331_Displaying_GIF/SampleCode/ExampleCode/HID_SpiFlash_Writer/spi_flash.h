/**************************************************************************//**
 * @file     spi_flash.h
 * @brief    SPI NOR flash driver (single-line) over QSPI0 using PC0~PC5 pins
 *           for the HID -> SPI flash programming utility.
 *
 * Pin mapping (QSPI0, same as Display_Animation example):
 *   PC0 = QSPI0_MOSI0, PC1 = QSPI0_MISO0, PC2 = QSPI0_CLK, PC3 = QSPI0_SS
 *   PC4 = QSPI0_MOSI1(D2), PC5 = QSPI0_MISO1(D3)  (reserved, quad not used here)
 *
 ******************************************************************************/
#ifndef __SPI_FLASH_H__
#define __SPI_FLASH_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* QSPI port used for the SPI flash */
#define SPI_FLASH_PORT          QSPI0

/* SPI clock frequency for the flash (Hz) */
#define FLASH_QSPI_CLK_FREQ     48000000

/* Flash geometry */
#define FLASH_PAGE_SIZE         256     /* Program page size (bytes)  */
#define FLASH_SECTOR_SIZE       4096    /* Erase sector size (bytes)  */

/**
 * @brief  Initialize QSPI0 and its PC0~PC5 pin multiplexing for the SPI flash.
 *         Must be called after the QSPI0 module clock has been enabled and
 *         the pin mux configured in SYS_Init().
 */
void SpiFlash_Init(void);

/**
 * @brief  Read the 24-bit JEDEC ID (manufacturer + device).
 * @return 24-bit JEDEC ID.
 */
uint32_t SpiFlash_ReadJedecID(void);

/**
 * @brief  Wait until the flash BUSY bit is cleared.
 * @return 0 on success, -1 on time-out.
 */
int32_t SpiFlash_WaitReady(void);

/**
 * @brief  Remove any block / status-register write protection so the flash
 *         can be erased and programmed. Clears the BP0..BP2, TB, SEC and SRP0
 *         bits in Status Register-1 and the CMP / SRP1 bits in Status
 *         Register-2 when they are set. Other bits (e.g. QE) are preserved.
 * @return 0 if the flash is unprotected (already or after clearing),
 *         -1 on time-out or if the protection could not be cleared.
 */
int32_t SpiFlash_ClearWriteProtect(void);

/**
 * @brief  Erase one 4 KB sector.
 * @param  u32Addr  Byte address inside the sector (will be 4 KB aligned).
 */
void SpiFlash_SectorErase4K(uint32_t u32Addr);

/**
 * @brief  Erase the whole chip (bulk erase).
 */
void SpiFlash_ChipErase(void);

/**
 * @brief  Program up to 256 bytes within a single page.
 * @param  u32Addr  Start byte address (should be 256-byte aligned for best use).
 * @param  pu8Data  Pointer to data buffer.
 * @param  u32Len   Number of bytes to program (1..256).
 */
void SpiFlash_PageProgram256(uint32_t u32Addr, uint8_t *pu8Data, uint32_t u32Len);

/**
 * @brief  Read data from flash (standard 0x03 read).
 * @param  u32Addr  Start byte address.
 * @param  pu8Data  Destination buffer.
 * @param  u32Len   Number of bytes to read.
 */
void SpiFlash_Read(uint32_t u32Addr, uint8_t *pu8Data, uint32_t u32Len);

#ifdef __cplusplus
}
#endif

#endif /* __SPI_FLASH_H__ */
