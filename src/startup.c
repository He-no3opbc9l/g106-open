/* Vector table and reset handler. Entered from the stock bootloader by a plain
 * jump (no MCU reset), so peripheral/NVIC state left by the bootloader is cleaned
 * up here. GPIO state is deliberately NOT touched: PC7 (power latch) must stay high. */
#include "stm32f4.h"

extern uint32_t _estack, _sidata, _sdata, _edata, _sbss, _ebss;
extern int main(void);
void SystemInit(void);

void Reset_Handler(void) __attribute__((section(".text.Reset_Handler"), noreturn, naked));
void Default_Handler(void);

#define WEAK_ALIAS __attribute__((weak, alias("Default_Handler")))
void NMI_Handler(void) WEAK_ALIAS;
void HardFault_Handler(void) WEAK_ALIAS;
void MemManage_Handler(void) WEAK_ALIAS;
void BusFault_Handler(void) WEAK_ALIAS;
void UsageFault_Handler(void) WEAK_ALIAS;
void SVC_Handler(void) WEAK_ALIAS;
void DebugMon_Handler(void) WEAK_ALIAS;
void PendSV_Handler(void) WEAK_ALIAS;
void SysTick_Handler(void) WEAK_ALIAS;
void USART1_IRQHandler(void) WEAK_ALIAS;
void DMA1_Stream3_IRQHandler(void) WEAK_ALIAS;

#define NUM_IRQ 82

__attribute__((section(".isr_vector"), used))
void (*const vector_table[16 + NUM_IRQ])(void) = {
    (void (*)(void))&_estack,
    Reset_Handler,
    NMI_Handler,
    HardFault_Handler,
    MemManage_Handler,
    BusFault_Handler,
    UsageFault_Handler,
    0, 0, 0, 0,
    SVC_Handler,
    DebugMon_Handler,
    0,
    PendSV_Handler,
    SysTick_Handler,
    [16 + 14] = DMA1_Stream3_IRQHandler,
    [16 + 37] = USART1_IRQHandler,
};

static void reset_c(void) __attribute__((noreturn, used));

/* The stock bootloader loads MSP with *(SP & 0xFFFFF) instead of SP (0x080016a4), i.e.
 * garbage. Set SP ourselves before anything touches the stack (stock __main does the same). */
__attribute__((naked)) void Reset_Handler(void)
{
    __asm volatile(
        "ldr r0, =_estack \n"
        "msr msp, r0      \n"
        "mov sp, r0       \n"
        "b reset_c        \n");
}

static void reset_c(void)
{
    __disable_irq();
    SCB_VTOR = (uint32_t)vector_table;
    SCB_CPACR |= (0xFu << 20);              /* CP10/CP11 full access (FPU) */
    __dsb();
    __isb();

    /* Drop whatever the bootloader (or a previous app, after an SWD hot-swap) left armed.
     * The bootloader leaves DMA2 Stream5 running (USART1 RX -> 0x2000078c, inside our stack). */
    SYSTICK->CTRL = 0;
    for (int n = 0; n < 2; n++) {
        uint32_t dma = 0x40026000 + 0x400 * n;
        if (!(RCC->AHB1ENR & (1u << (21 + n))))     /* unclocked: idle, and access bus-faults */
            continue;
        for (int s = 0; s < 8; s++)
            REG32(dma + 0x10 + 0x18 * s) &= ~1u;    /* SxCR.EN = 0 */
        REG32(dma + 0x08) = 0x0F7D0F7D;             /* LIFCR: clear all flags */
        REG32(dma + 0x0C) = 0x0F7D0F7D;             /* HIFCR */
    }
    for (int i = 0; i < 3; i++) {
        NVIC_ICER(i) = 0xFFFFFFFF;
        NVIC_ICPR(i) = 0xFFFFFFFF;
    }

    uint32_t *src = &_sidata, *dst = &_sdata;
    while (dst < &_edata)
        *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss; dst++)
        *dst = 0;

    SystemInit();
    __enable_irq();
    main();
    for (;;)
        ;
}

void Default_Handler(void)
{
    for (;;)
        ;
}
