/**************************************************************************//**
 * @file     main.c
 * @brief    GIF player : read GIF raw data from SPI flash over QSPI0 + PDMA,
 *           decode it line-by-line with the AnimatedGIF library and stream the
 *           rendered 320x240 RGB565 frames to an ILI9341 TFT over SPI1 + PDMA.
 *
 * @copyright SPDX-License-Identifier: Apache-2.0
 * @copyright Copyright (c) 2024 Nuvoton Technology Corp. All rights reserved.
 ******************************************************************************/
#include <stdio.h>
#include "NuMicro.h"
#include "qspi_flash.h"
#include "lcd.h"
#include "gif_player.h"

/* DMA-completion flags owned by the LCD and flash drivers */
extern volatile uint8_t g_LcdDmaDone;
extern volatile uint8_t g_FlashDmaDone;

/* TIMER0 tick counter : one tick every 100 microseconds */
volatile uint32_t g_u32TimerCnt = 0;

/*--------------------------------------------------------------------------*/
/* Interrupt handlers                                                        */
/*--------------------------------------------------------------------------*/
void PDMA0_IRQHandler(void)
{
    uint32_t u32Status = PDMA_GET_INT_STATUS(PDMA0);

    if (u32Status & 0x1)            /* abort */
    {
        PDMA_CLR_ABORT_FLAG(PDMA0, PDMA_GET_ABORT_STS(PDMA0));
    }
    else if (u32Status & 0x2)       /* transfer done */
    {
        if (PDMA_GET_TD_STS(PDMA0) & (1 << 0))   /* channel 0 : LCD TX */
        {
            PDMA_CLR_TD_FLAG(PDMA0, (1 << 0));
            LCD_DmaDoneHook();                   /* chain 2nd half or finish */
        }
        if (PDMA_GET_TD_STS(PDMA0) & (1 << 1))   /* channel 1 : flash RX */
        {
            g_FlashDmaDone = 1;
            PDMA_CLR_TD_FLAG(PDMA0, (1 << 1));
        }
    }
    else if (u32Status & 0x100)     /* channel 0 time-out */
    {
        PDMA_CLR_TMOUT_FLAG(PDMA0, 0);
    }
    else if (u32Status & 0x200)     /* channel 1 time-out */
    {
        PDMA_CLR_TMOUT_FLAG(PDMA0, 1);
    }
}

void TMR0_IRQHandler(void)
{
    if (TIMER_GetIntFlag(TIMER0) == 1)
    {
        TIMER_ClearIntFlag(TIMER0);
        g_u32TimerCnt++;
    }
}

/*--------------------------------------------------------------------------*/
/* HardFault diagnostics : dump the stacked registers + fault status so we   */
/* can locate the crash inside the GIF decoder over UART.                    */
/*                                                                            */
/* IMPORTANT: this path must NOT use printf(). printf pulls in a lot of stack */
/* and library state; if the fault was caused by a stack overflow, calling    */
/* printf from the handler faults again and the core escalates to lockup -> a */
/* silent reset (exactly the "nothing prints, board resets" symptom). Instead */
/* we bit-bang characters straight into the UART0 FIFO, and the assembly entry*/
/* resets MSP to the top of SRAM first so there is always room to work.       */
/*--------------------------------------------------------------------------*/
static void HF_putc(char c)
{
    while (UART0->FIFOSTS & UART_FIFOSTS_TXFULL_Msk) {}
    UART0->DAT = (uint32_t)(uint8_t)c;
}

static void HF_puts(const char *s)
{
    while (*s)
        HF_putc(*s++);
}

static void HF_puthex(uint32_t v)
{
    static const char hx[] = "0123456789ABCDEF";
    int i;
    HF_putc('0');
    HF_putc('x');
    for (i = 28; i >= 0; i -= 4)
        HF_putc(hx[(v >> i) & 0xF]);
}

/* Called from the naked HardFault_Handler. r0 = original stack pointer at the
   time of the fault (the frame the CPU pushed: R0 R1 R2 R3 R12 LR PC xPSR). */
void HardFault_Dump(uint32_t *pu32Stack)
{
    uint32_t sp = (uint32_t)pu32Stack;

    HF_puts("\r\n==== HARD FAULT ====\r\n");

    HF_puts("CFSR =");  HF_puthex(SCB->CFSR);
    HF_puts(" HFSR ="); HF_puthex(SCB->HFSR);
    HF_puts("\r\nMMFAR="); HF_puthex(SCB->MMFAR);
    HF_puts(" BFAR ="); HF_puthex(SCB->BFAR);
    HF_puts("\r\n");

    /* Only read the stacked frame if the original SP looks like valid SRAM,
       so an overflowed/garbage SP cannot fault us again. */
    if (sp >= 0x20000000u && sp <= (0x20060000u - 32u))
    {
        HF_puts("R0 =");  HF_puthex(pu32Stack[0]);
        HF_puts(" R1 ="); HF_puthex(pu32Stack[1]);
        HF_puts(" R2 ="); HF_puthex(pu32Stack[2]);
        HF_puts(" R3 ="); HF_puthex(pu32Stack[3]);
        HF_puts("\r\nR12="); HF_puthex(pu32Stack[4]);
        HF_puts(" LR ="); HF_puthex(pu32Stack[5]);
        HF_puts(" PC ="); HF_puthex(pu32Stack[6]);   /* faulting instruction */
        HF_puts(" PSR="); HF_puthex(pu32Stack[7]);
        HF_puts("\r\n");
    }
    else
    {
        HF_puts("SP out of range (stack overflow?) SP=");
        HF_puthex(sp);
        HF_puts("\r\n");
    }

    /* Decode the most common causes (CFSR bit meanings). */
    if (SCB->CFSR & (1u << 25)) HF_puts("  DIVBYZERO\r\n");
    if (SCB->CFSR & (1u << 24)) HF_puts("  UNALIGNED\r\n");
    if (SCB->CFSR & (1u << 20)) HF_puts("  STKOF (stack overflow)\r\n");
    if (SCB->CFSR & (1u << 19)) HF_puts("  NOCP\r\n");
    if (SCB->CFSR & (1u << 18)) HF_puts("  INVPC\r\n");
    if (SCB->CFSR & (1u << 17)) HF_puts("  INVSTATE\r\n");
    if (SCB->CFSR & (1u << 16)) HF_puts("  UNDEFINSTR\r\n");
    if (SCB->CFSR & (1u << 15)) HF_puts("  BFARVALID\r\n");
    if (SCB->CFSR & (1u << 10)) HF_puts("  PRECISERR (bus fault @BFAR)\r\n");
    if (SCB->CFSR & (1u << 9))  HF_puts("  IMPRECISERR (async bus fault)\r\n");
    if (SCB->CFSR & (1u << 7))  HF_puts("  MMARVALID\r\n");
    if (SCB->CFSR & (1u << 1))  HF_puts("  DACCVIOL (data access violation)\r\n");
    if (SCB->CFSR & (1u << 0))  HF_puts("  IACCVIOL (instr access violation)\r\n");
    HF_puts("====================\r\n");

    while (1) {}   /* halt here : inspect PC/LR to locate the faulting code */
}

/* Naked entry : capture the faulting SP in r0, then reset MSP to the top of
   SRAM so the dump code always has a clean stack to run on. */
__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile(
        "tst lr, #4            \n" /* EXC_RETURN bit 2 : 0=MSP, 1=PSP */
        "ite eq               \n"
        "mrseq r0, msp        \n"
        "mrsne r0, psp        \n"
        "movw r1, #0x0000     \n" /* r1 = 0x20060000 (top of 384 KB SRAM) */
        "movt r1, #0x2006     \n"
        "mov sp, r1           \n" /* safe stack for the dumper */
        "b HardFault_Dump     \n"
    );
}


unsigned long millis(void)
{
    return (unsigned long)(g_u32TimerCnt / 10u);   /* 100us ticks -> ms */
}

void delay(unsigned long u32Ms)
{
    unsigned long u32Start = g_u32TimerCnt;

    while ((g_u32TimerCnt - u32Start) < (u32Ms * 10u)) {}
}

/*--------------------------------------------------------------------------*/
/* System init                                                               */
/*--------------------------------------------------------------------------*/
void SYS_Init(void)
{
    /* Set PCLK0 and PCLK1 to HCLK */
    CLK->PCLKDIV = (CLK_PCLKDIV_APB0DIV_DIV1 | CLK_PCLKDIV_APB1DIV_DIV1);

    /* Set core clock */
    CLK_SetCoreClock(FREQ_180MHZ);

    /* Enable all GPIO port clocks */
    CLK->AHBCLK0 |= CLK_AHBCLK0_GPACKEN_Msk | CLK_AHBCLK0_GPBCKEN_Msk | CLK_AHBCLK0_GPCCKEN_Msk | CLK_AHBCLK0_GPDCKEN_Msk |
                    CLK_AHBCLK0_GPECKEN_Msk | CLK_AHBCLK0_GPFCKEN_Msk | CLK_AHBCLK0_GPGCKEN_Msk | CLK_AHBCLK0_GPHCKEN_Msk;

    /* UART0 clock : HIRC */
    CLK_EnableModuleClock(UART0_MODULE);
    CLK_SetModuleClock(UART0_MODULE, CLK_CLKSEL1_UART0SEL_HIRC, CLK_CLKDIV0_UART0(1));

    /* TIMER0 clock : HIRC */
    CLK_EnableModuleClock(TMR0_MODULE);
    CLK_SetModuleClock(TMR0_MODULE, CLK_CLKSEL1_TMR0SEL_HIRC, 0);

    /* QSPI0 (flash) and SPI1 (LCD) clocks : PCLK0 */
    CLK_EnableModuleClock(QSPI0_MODULE);
    CLK_EnableModuleClock(SPI1_MODULE);
    CLK_SetModuleClock(QSPI0_MODULE, CLK_CLKSEL2_QSPI0SEL_PCLK0, MODULE_NoMsk);
    CLK_SetModuleClock(SPI1_MODULE, CLK_CLKSEL2_SPI1SEL_PCLK0, MODULE_NoMsk);

    /* PDMA0 clock */
    CLK_EnableModuleClock(PDMA0_MODULE);

    /* UART0 : PB12 (RXD) / PB13 (TXD) */
    SET_UART0_RXD_PB12();
    SET_UART0_TXD_PB13();

    /* QSPI0 : PC3 SS, PC2 CLK, PC1 MISO0, PC0 MOSI0 (D2/D3 on PC4/PC5 set on demand) */
    SET_QSPI0_SS_PC3();
    SET_QSPI0_CLK_PC2();
    SET_QSPI0_MISO0_PC1();
    SET_QSPI0_MOSI0_PC0();
    PC->SMTEN |= GPIO_SMTEN_SMTEN2_Msk;                  /* CLK schmitt trigger */
    GPIO_SetSlewCtl(PC, BIT0 | BIT1 | BIT2 | BIT3, GPIO_SLEWCTL_FAST);

    /* SPI1 : PH9 SS, PH8 CLK, PE1 MISO, PE0 MOSI */
    SET_SPI1_SS_PH9();
    SET_SPI1_CLK_PH8();
    SET_SPI1_MISO_PE1();
    SET_SPI1_MOSI_PE0();
    GPIO_SetSlewCtl(PH, BIT8 | BIT9, GPIO_SLEWCTL_FAST);
    GPIO_SetSlewCtl(PE, BIT0 | BIT1, GPIO_SLEWCTL_FAST);
}

void Timer0_Init(void)
{
    /* Periodic mode, 100us per interrupt tick */
    TIMER_Open(TIMER0, TIMER_PERIODIC_MODE, 10000);
    TIMER_EnableInt(TIMER0);
    TIMER_Start(TIMER0);

    g_u32TimerCnt = 0;
    NVIC_EnableIRQ(TMR0_IRQn);
}

/*--------------------------------------------------------------------------*/
/* Application                                                               */
/*--------------------------------------------------------------------------*/
int main(void)
{
    SYS_UnlockReg();
    SYS_Init();

    /* LCD control pins : PD14 RST, PH10 DC, PH11 EN */
    GPIO_SetMode(PD, BIT14, GPIO_MODE_OUTPUT);
    GPIO_SetMode(PH, BIT10 | BIT11, GPIO_MODE_OUTPUT);

    UART_Open(UART0, 115200);

    /* ---- Report WHY the chip (re)started. A silent reboot with no HardFault
            dump is a hardware reset: watchdog, brown-out/low-voltage, CPU
            lockup (double fault) or a pin/power reset. RSTSTS latches the
            cause; print it once then clear so the next reset is unambiguous. */
    {
        uint32_t u32Rst = SYS->RSTSTS;
        printf("\n==== BOOT ==== RSTSTS=0x%08X :", u32Rst);
        if (u32Rst & SYS_RSTSTS_PORF_Msk)    printf(" POR");
        if (u32Rst & SYS_RSTSTS_PINRF_Msk)   printf(" PIN");
        if (u32Rst & SYS_RSTSTS_WDTRF_Msk)   printf(" WDT");
        if (u32Rst & SYS_RSTSTS_LVRF_Msk)    printf(" LVR");
        if (u32Rst & SYS_RSTSTS_BODRF_Msk)   printf(" BOD");
        if (u32Rst & SYS_RSTSTS_CPURF_Msk)   printf(" CPU");
        if (u32Rst & SYS_RSTSTS_CPULKRF_Msk) printf(" LOCKUP");
        printf("\n");
        SYS->RSTSTS = u32Rst;   /* write-1-clear all latched flags */
    }

    Timer0_Init();

    QSPI_Flash_Init();      /* flash reader (QSPI0) */
    SPI_LCD_Init();         /* LCD controller (SPI1) */
    LCD_Init();             /* ILI9341 power-on sequence */

    LCD_FillColor(0x0000);  /* clear screen to black */

    NVIC_SetPriority(PDMA0_IRQn, 0);
    NVIC_SetPriority(TMR0_IRQn, 1);

    GIF_Player_Init();
    printf("GIF decode player started.\n");

    while (1)
    {
        /* Non-blocking GIF decode + display pipeline */
        GIF_Player_Task();

        /* Application_Poll();  // place UI / character overlay work here */
    }
}
