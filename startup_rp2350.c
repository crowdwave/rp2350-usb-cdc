/* startup_rp2350.c — RP2350 (Cortex-M33) bare-metal startup.
 * 1. IMAGE_DEF metadata block so the bootrom accepts the image (RP2350 datasheet 5.9.5.1, Arm, secure-boot
 *    disabled): 20 bytes / 5 words, exact LE values. 2. Vector table at the image start. 3. Reset handler:
 *    VTOR, FPU, copy .data, zero .bss, main(). No RP2040 boot2 — bootrom does best-effort XIP. */
#include <stdint.h>

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
int  main(void);
void Reset_Handler(void);
void Default_Handler(void);

/* ---- RP2350 minimum Arm IMAGE_DEF (datasheet Table, §5.9.5.1) ---- */
__attribute__((section(".image_def"), used))
static const uint32_t image_def[5] = {
    0xffffded3u,   /* PICOBIN_BLOCK_MARKER_START                                              */
    0x10210142u,   /* item: 1BS_IMAGE_TYPE, 1 word, IMAGE_TYPE_EXE|SECURITY_S|CPU_ARM|RP2350  */
    0x000001ffu,   /* 2BS_LAST, size = 1                                                      */
    0x00000000u,   /* next-block pointer = 0 -> block loop links to self                      */
    0xab123579u,   /* PICOBIN_BLOCK_MARKER_END                                                */
};

/* ---- vector table (bootrom reads SP@+0, reset@+4 from here at 0x10000000) ---- */
__attribute__((section(".isr_vector"), used))
void (* const g_vectors[])(void) = {
    (void (*)(void))(&_estack),   /* 0: initial SP */
    Reset_Handler,                /* 1: reset      */
    Default_Handler,              /* 2: NMI        */
    Default_Handler,              /* 3: HardFault  */
    Default_Handler,              /* 4: MemManage  */
    Default_Handler,              /* 5: BusFault   */
    Default_Handler,              /* 6: UsageFault */
    Default_Handler,              /* 7: SecureFault (ARMv8-M) */
    0, 0, 0,                      /* 8-10 reserved */
    Default_Handler,              /* 11: SVC       */
    Default_Handler,              /* 12: DebugMon  */
    0,                            /* 13 reserved   */
    Default_Handler,              /* 14: PendSV    */
    Default_Handler,              /* 15: SysTick   */
};

void Reset_Handler(void)
{
    *(volatile uint32_t *)0xE000ED08u = 0x10000000u;          /* SCB->VTOR -> our vector table */
    *(volatile uint32_t *)0xE000ED88u |= (0xFu << 20);        /* CPACR: enable CP10/CP11 (FPU) */
    __asm__ volatile("dsb; isb");

    for(uint32_t *s=&_sidata, *d=&_sdata; d<&_edata; ) *d++ = *s++;   /* copy .data */
    for(uint32_t *b=&_sbss; b<&_ebss; ) *b++ = 0u;                    /* zero .bss  */

    main();
    for(;;) {}
}

void Default_Handler(void) { for(;;) {} }
