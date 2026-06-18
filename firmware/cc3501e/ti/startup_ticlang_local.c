/******************************************************************************
 *  Filename:       startup_ticlang_local.c
 *
 *  Description:    Startup code for CC35XX device family for use with
 *                  TI Clang/LLVM.
 *
 *  Local copy of the SDK's source/ti/devices/cc35xx/startup_files/
 *  startup_ticlang.c with ONE bench fix added (marked below): ResetISR sets
 *  SCB->VTOR to our vector table before anything can take an exception.  The
 *  CC35xx SES hands the app off with VTOR pointing at the SES's own table
 *  (observed 0x14002000), NOT our resetVecs at 0x10001100; the stock ResetISR
 *  never touches VTOR, so an early IRQ/fault vectors through the wrong table
 *  -> IBUSERR -> forced HardFault -> double-fault -> LOCKUP, before main().
 *  (Confirmed on silicon 2026-06-17: a push-pull POCI-low at main()'s first
 *  line never appears at the Alif -> main() is never reached.)
 *
 *  Copyright (c) 2022-2025 Texas Instruments Incorporated
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *  1) Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *  2) Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *
 *  3) Neither the name of the copyright holder nor the names of its
 *     contributors may be used to endorse or promote products derived from this
 *     software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 ******************************************************************************/

#if !(defined(__clang__))
#error "startup_ticlang_local.c: Unsupported compiler!"
#endif

#include <ti/devices/cc35xx/inc/hw_types.h>
#include <ti/devices/cc35xx/driverlib/setup.h>

void        ResetISR(void);
static void NmiISR(void);
static void FaultISR(void);
static void IntDefaultHandler(void);
extern int  main(void);

extern void _c_int00(void);

extern unsigned long __STACK_END;

/* ARMv8-M System Control Block, Vector Table Offset Register. */
#define ALP_SCB_VTOR (*(volatile unsigned long *)0xE000ED08UL)

__attribute__((section(".resetVecs"), retain)) static void (*const resetVectors[])(void) = {
	(void (*)(void))((unsigned long)&__STACK_END),
	ResetISR,          // The reset handler
	NmiISR,            // The NMI handler
	FaultISR,          // The hard fault handler
	IntDefaultHandler, // The MPU fault handler
	IntDefaultHandler, // The bus fault handler
	IntDefaultHandler, // The usage fault handler
	IntDefaultHandler, // Reserved
	IntDefaultHandler, // Reserved
	IntDefaultHandler, // Reserved
	IntDefaultHandler, // Reserved
	IntDefaultHandler, // SVCall handler
	IntDefaultHandler, // Debug monitor handler
	IntDefaultHandler, // Reserved
	IntDefaultHandler, // The PendSV handler
	IntDefaultHandler, // The SysTick handler
	// External interrupts
	IntDefaultHandler, // 16 INT_SP_UART_0_INT_REQ
	IntDefaultHandler, // 17 INT_SP_UART_1_INT_REQ
	IntDefaultHandler, // 18 INT_SP_I2C_0_INTREQ
	IntDefaultHandler, // 19 INT_SP_I2C_1_INTREQ
	IntDefaultHandler, // 20 INT_SP_SPI_0_EVT_REQ
	IntDefaultHandler, // 21 INT_SP_SPI_1_EVT_REQ
	IntDefaultHandler, // 22 INT_GPTIMER_0_EVT_CPU_IRQ
	IntDefaultHandler, // 23 INT_GPTIMER_1_EVT_CPU_IRQ
	IntDefaultHandler, // 24 INT_SP_UART_2_INT_REQ
	IntDefaultHandler, // 25 INT_I2S_IRQ_REQ
	IntDefaultHandler, // 26 INT_EVT_PDM_EVENT_REQ
	IntDefaultHandler, // 27 INT_EVT_SWINT0_REQ
	IntDefaultHandler, // 28 INT_EVT_SWINT1_REQ
	IntDefaultHandler, // 29 INT_EVT_SDMMC_PUB_REQ
	IntDefaultHandler, // 30 INT_SDIO_CARD_IRQ_REQ
	IntDefaultHandler, // 31 INT_ULL_USC_ULPADCHP_PUB_EVT0_REQ
	IntDefaultHandler, // 32 INT_NON_SECURED_GPIO_IRQ_EVT_IND_OUT
	IntDefaultHandler, // 33 INT_SECURED_GPIO_IRQ_EVT_IND_OUT
	IntDefaultHandler, // 34 INT_OSPR_HSM_HOST_0_SEC_IRQ
	IntDefaultHandler, // 35 INT_OSPR_HSM_HOST_0_IRQ
	IntDefaultHandler, // 36 INT_OSPR_HSM_HOST_1_IRQ
	IntDefaultHandler, // 37 INT_SVT_EVT_COMBINED_SYSTIM_OUT_IRQ
	IntDefaultHandler, // 38 INT_SVT_EVT_SYSTIMER_BIT_OUT_IRQ
	IntDefaultHandler, // 39 INT_SVT_EVT_SYSTIMER_OUT_0_IRQ
	IntDefaultHandler, // 40 INT_SVT_EVT_SYSTIMER_OUT_1_IRQ
	IntDefaultHandler, // 41 INT_NON_SECURED_DMA_IRQ_EVT_IND_OUT
	IntDefaultHandler, // 42 INT_SECURED_DMA_IRQ_EVT_IND_OUT
	IntDefaultHandler, // 43 INT_NON_SECURED_DOORBELL_IRQ_EVT_IND_OUT
	IntDefaultHandler, // 44 INT_SECURED_DOORBELL_IRQ_EVT_IND_OUT
	IntDefaultHandler, // 45 INT_ICACHE_ERR_IRQ
	IntDefaultHandler, // 46 INT_OSPI_IRQ
	IntDefaultHandler, // 47 INT_OTFDE_IRQ
	IntDefaultHandler, // 48 INT_XIP_ARB_IRQ
	IntDefaultHandler, // 49 INT_XIP_DMA_SEC_IRQ
	IntDefaultHandler, // 50 INT_XIP_DMA_NONSEC_IRQ
	IntDefaultHandler, // 51 INT_SW_INTERRUPT_0
	IntDefaultHandler, // 52 INT_SW_INTERRUPT_1
	IntDefaultHandler, // 53 INT_SW_INTERRUPT_2
	IntDefaultHandler, // 54 INT_SW_INTERRUPT_3
	IntDefaultHandler, // 55 INT_SW_INTERRUPT_4
	IntDefaultHandler, // 56 INT_SW_INTERRUPT_5
	IntDefaultHandler, // 57 INT_SW_INTERRUPT_6
	IntDefaultHandler, // 58 INT_SW_INTERRUPT_7
	IntDefaultHandler, // 59 INT_PRCM_IRQ
	IntDefaultHandler, // 60 INT_OCLA_IRQ
	IntDefaultHandler, // 61 INT_HIF_FIFO_IRQ
	IntDefaultHandler, // 62 INT_HOST_ELP_TMR_WAKEUP_REQ
	IntDefaultHandler, // 63 INT_NAB_HOST_IRQ
	IntDefaultHandler, // 64 INT_BLE_RFC_GPO_8_IRQ
	IntDefaultHandler, // 65 INT_RTC_EVENT_IRQ
	IntDefaultHandler, // 66 INT_DEBUGSS_HOST_CSYSPWRUPREQ
	IntDefaultHandler, // 67 INT_DEBUGSS_HOST_FORCEACTIVE
	IntDefaultHandler, // 68 INT_SECURED_ERROR_IRQ_EVT_IND_OUT
};

void ResetISR(void)
{
	/* BENCH FIX (2026-06-17): point VTOR at OUR vector table FIRST.  The SES
     * leaves VTOR at its own table (0x14002000), and the stock ResetISR never
     * sets it, so any early exception/IRQ vectors through the wrong table ->
     * IBUSERR -> double-fault -> lockup before main().  Set it before
     * SetupTrimDevice() (which may touch peripherals / take an interrupt). */
	ALP_SCB_VTOR = (unsigned long)resetVectors;
	__asm(" dsb");
	__asm(" isb");

	// Final trim of device
	SetupTrimDevice();

	// Jump to the CCS C Initialization Routine.
	__asm("    .global _c_int00\n"
	      "    bl      _c_int00");

	// If we ever return signal Error
	FaultISR();
}

__attribute__((noreturn)) static void NmiISR(void)
{
	while (1) {
	}
}

__attribute__((noreturn)) static void FaultISR(void)
{
	while (1) {
	}
}

__attribute__((noreturn)) static void IntDefaultHandler(void)
{
	while (1) {
	}
}
