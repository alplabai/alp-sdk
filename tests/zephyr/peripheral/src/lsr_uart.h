/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ALP_TEST_LSR_UART_H_
#define ALP_TEST_LSR_UART_H_

#include <stdint.h>

void lsr_uart_reset(void);
void lsr_uart_push(const uint8_t *data, unsigned n);
/* After the Nth poll_in() completes, latch UART_ERROR_OVERRUN (as hardware
 * does mid-burst); the NEXT poll_in() clears it, ns16550-style. */
void lsr_uart_overrun_after_polls(int polls);

/* After the Nth poll_out() completes, latch UART_ERROR_OVERRUN; the NEXT
 * poll_out()/poll_in() (an LSR read) clears it, ns16550-style. */
void lsr_uart_overrun_after_tx(int polls);

#endif
