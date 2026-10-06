/**************************************************************************//**
 * @file     gif_player.h
 * @brief    Streaming GIF player glue around the AnimatedGIF library.
 *           Decodes a GIF stored in SPI flash line-by-line (COOKED RGB565)
 *           into a full 320x240 frame buffer and streams it to the TFT.
 *
 * @copyright SPDX-License-Identifier: Apache-2.0
 ******************************************************************************/
#ifndef __GIF_PLAYER_H__
#define __GIF_PLAYER_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Byte size of the GIF stored in flash. Adjust to match the file written by
   the HID SPI-flash tool. A value larger than the real file still works: the
   player detects the GIF trailer and loops. */
#ifndef GIF_FILE_SIZE
#define GIF_FILE_SIZE   (2477900)
#endif

/** @brief Reset the player state and clear the frame buffer. */
void GIF_Player_Init(void);
/**
 * @brief Advance the non-blocking GIF player state machine by one step.
 *        Call repeatedly from the main super-loop.
 */
void GIF_Player_Task(void);
#ifdef __cplusplus
}
#endif

#endif /* __GIF_PLAYER_H__ */
