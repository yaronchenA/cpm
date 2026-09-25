/*
 * CPM prototype — board bring-up blink (NUCLEO-F302R8).
 *
 * Blinks the user LED LD2 (PB13 on this Nucleo) at 1 Hz using a SysTick
 * millisecond tick. Bare-metal register access, no HAL: the only goal is to
 * prove the toolchain, flashing, and the board itself work (prototype_plan.md M0).
 *
 * Clock: default 8 MHz HSI after reset, no PLL setup.
 */

#include <stdint.h>

#define REG32(addr) (*(volatile uint32_t *)(addr))

/* RCC */
#define RCC_BASE        0x40021000u
#define RCC_AHBENR      REG32(RCC_BASE + 0x14u)
#define RCC_AHBENR_IOPBEN (1u << 18)

/* GPIOB */
#define GPIOB_BASE      0x48000400u
#define GPIOB_MODER     REG32(GPIOB_BASE + 0x00u)
#define GPIOB_BSRR      REG32(GPIOB_BASE + 0x18u)
#define GPIOB_ODR       REG32(GPIOB_BASE + 0x14u)

/* SysTick */
#define SYST_CSR        REG32(0xE000E010u)
#define SYST_RVR        REG32(0xE000E014u)
#define SYST_CVR        REG32(0xE000E018u)

#define LED_PIN         13u
#define CPU_HZ          8000000u

static volatile uint32_t g_ms_ticks;

void SysTick_Handler(void)
{
    g_ms_ticks++;
}

static void delay_ms(uint32_t ms)
{
    uint32_t start = g_ms_ticks;
    while ((g_ms_ticks - start) < ms) {
        /* wait */
    }
}

int main(void)
{
    /* Enable GPIOB clock, set PB13 as push-pull output. */
    RCC_AHBENR |= RCC_AHBENR_IOPBEN;
    GPIOB_MODER = (GPIOB_MODER & ~(3u << (LED_PIN * 2))) | (1u << (LED_PIN * 2));

    /* 1 ms SysTick from the core clock, interrupt enabled. */
    SYST_RVR = (CPU_HZ / 1000u) - 1u;
    SYST_CVR = 0;
    SYST_CSR = 0x7u;

    for (;;) {
        if (GPIOB_ODR & (1u << LED_PIN)) {
            GPIOB_BSRR = 1u << (LED_PIN + 16); /* reset -> LED off */
        } else {
            GPIOB_BSRR = 1u << LED_PIN;        /* set   -> LED on  */
        }
        delay_ms(500);
    }
}
