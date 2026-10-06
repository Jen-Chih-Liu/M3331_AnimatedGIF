/**************************************************************************//**
 * @file     gif_compat.h
 * @brief    Minimal Arduino timing shim so the (unmodified) AnimatedGIF C++
 *           sources compile and link on bare-metal. This header is force-
 *           included into every translation unit via the compiler
 *           "-include ..\gif_compat.h" option (see the Keil project).
 *
 *           millis()/delay() are implemented in main.c on top of TIMER0.
 *
 * @copyright SPDX-License-Identifier: Apache-2.0
 ******************************************************************************/
#ifndef __GIF_COMPAT_H__
#define __GIF_COMPAT_H__

#ifdef __cplusplus
extern "C" {
#endif

unsigned long millis(void);
void          delay(unsigned long u32Ms);

#ifdef __cplusplus
}
#endif

#endif /* __GIF_COMPAT_H__ */
