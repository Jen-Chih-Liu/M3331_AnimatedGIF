/**************************************************************************//**
 * @file     qspi_flash.h
 * @brief    QSPI0 + PDMA SPI-NOR flash reader with a small block cache.
 *           Reads GIF raw data from the on-board SPI flash using the 0xEB
 *           Quad I/O Fast Read command (same wiring as the Display_Animation
 *           example : PC0~PC5).
 *
 * @copyright SPDX-License-Identifier: Apache-2.0
 ******************************************************************************/
#ifndef __QSPI_FLASH_H__
#define __QSPI_FLASH_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* QSPI controller connected to the SPI flash */
#define QSPI_FLASH_PORT         QSPI0
/* QSPI clock used for the flash reads (Hz) */
#define FLASH_QSPI_CLK_FREQ     60000000

/* Size of one cache line (must be a multiple of 4 bytes) */
#define FLASH_CACHE_BLOCK       2048u

/* Flash byte address where the GIF raw data starts */
#define GIF_FLASH_BASE_ADDR     0x00000000u

/**
 * @brief Initialise QSPI0 for the SPI flash and enable the flash Quad mode.
 *        The QSPI0 pin mux and module clock must already be set up in SYS_Init().
 */
void QSPI_Flash_Init(void);

/**
 * @brief Invalidate the block cache. Call before starting a new read sequence
 *        (e.g. before (re)opening the GIF file) so stale data is never used.
 */
void FlashCache_Reset(void);

/**
 * @brief Read @p u32Len bytes starting at flash byte address @p u32Addr.
 *        Served from a 2 KB block cache; cache misses trigger a PDMA quad read.
 */
void FlashCache_Read(uint32_t u32Addr, uint8_t *pu8Dst, uint32_t u32Len);

#ifdef __cplusplus
}
#endif

#endif /* __QSPI_FLASH_H__ */
