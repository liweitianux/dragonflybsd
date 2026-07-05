/*
 * Minimal 8250/16550 UART (COM1) with TX and RX interrupt support.
 *
 * A guest's early console output polls the UART directly, but its serial tty
 * driver is interrupt-driven: it sends a byte then waits for the THR-empty IRQ,
 * and receives via the RX IRQ. So we raise IRQ4 (through the IOAPIC) both when
 * the transmitter is ready and when host input arrives. Host input is pulled
 * from stdin by uart_rx_poll(), driven from the vCPU loop.
 *
 * Copyright (c) 2026 The DragonFly Project.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "vmm_internal.h"
#include "ioapic.h"

#define UART_IRQ	4

/*
 * 8250/16550 register offsets and status bits, mirroring the canonical values
 * in <dev/serial/ic_layer/ns16550.h> (kernel-only, not installed to
 * /usr/include).  Register offsets are from UART_IOBASE.
 */
#define COM_DATA	0	/* THR/RBR (DLAB=0), DLL (DLAB=1) */
#define COM_IER		1	/* IER (DLAB=0), DLM (DLAB=1) */
#define COM_IIR		2	/* IIR (read), FCR (write) */
#define COM_LCR		3	/* line control; bit 7 = DLAB */
#define COM_MCR		4	/* modem control */
#define COM_LSR		5	/* line status */
#define COM_MSR		6	/* modem status */
#define COM_SCR		7	/* scratch */

#define IER_RDAI	0x01	/* received-data available int enable */
#define IER_THRI	0x02	/* THR-empty int enable */

#define IIR_NONE	0x01	/* no interrupt pending */
#define IIR_THRE	0x02	/* THR empty */
#define IIR_RDA		0x04	/* received data available */

#define LSR_DR		0x01
#define LSR_THRE	0x20
#define LSR_TEMT	0x40

#define MSR_CTS		0x10
#define MSR_DSR		0x20
#define MSR_DCD		0x80

#define RXRING		256

static struct {
	uint8_t ier;
	uint8_t lcr;
	uint8_t mcr;
	uint8_t scr;
	uint8_t dll, dlm;
	bool tx_pending;	/* THR-empty interrupt waiting to be reported */
	uint8_t rx[RXRING];	/* receive ring (host -> guest) */
	unsigned rx_head, rx_tail;
} uart;

static inline bool
rx_empty(void)
{
	return uart.rx_head == uart.rx_tail;
}

void
uart_init(void)
{
	int fl;

	uart.dll = 1;
	/* Non-blocking stdin so uart_rx_poll() never stalls the vCPU loop. */
	fl = fcntl(STDIN_FILENO, F_GETFL, 0);
	if (fl != -1)
		fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);
}

static void
uart_kick_tx(void)
{
	/* Transmitter is always empty; flag the interrupt and raise IRQ4. */
	if (uart.ier & IER_THRI) {
		uart.tx_pending = true;
		ioapic_raise(UART_IRQ);
	}
}

/* Pull host input from stdin into the RX ring and raise the RX interrupt. */
void
uart_rx_poll(void)
{
	uint8_t buf[64];
	ssize_t n, i;

	n = read(STDIN_FILENO, buf, sizeof(buf));
	if (n <= 0)
		return;
	n = (ssize_t)microvm_console_input(buf, (size_t)n);	/* ^A x escape */
	if (n <= 0)
		return;
	for (i = 0; i < n; i++) {
		unsigned next = (uart.rx_head + 1) % RXRING;
		if (next == uart.rx_tail)
			break;			/* ring full: drop the rest */
		uart.rx[uart.rx_head] = buf[i];
		uart.rx_head = next;
	}
	if (!rx_empty() && (uart.ier & IER_RDAI))
		ioapic_raise(UART_IRQ);
}

void
uart_io(uint16_t port, bool in, uint8_t *data, size_t size)
{
	unsigned int reg = port - UART_IOBASE;
	bool dlab = (uart.lcr & 0x80) != 0;
	uint8_t val = 0;

	if (in) {
		memset(data, 0, size);
		switch (reg) {
		case COM_DATA:
			if (dlab) {
				val = uart.dll;
			} else if (!rx_empty()) {	/* RBR: pop received byte */
				val = uart.rx[uart.rx_tail];
				uart.rx_tail = (uart.rx_tail + 1) % RXRING;
			}
			break;
		case COM_IER:
			val = dlab ? uart.dlm : uart.ier;
			break;
		case COM_IIR:
			/* Highest-priority pending interrupt (RX over THRE). */
			if ((uart.ier & IER_RDAI) && !rx_empty()) {
				val = IIR_RDA;
			} else if ((uart.ier & IER_THRI) && uart.tx_pending) {
				uart.tx_pending = false;
				val = IIR_THRE;
			} else {
				val = IIR_NONE;
			}
			break;
		case COM_LCR:	val = uart.lcr; break;
		case COM_MCR:	val = uart.mcr; break;
		case COM_LSR:
			val = LSR_TEMT | LSR_THRE;
			if (!rx_empty())
				val |= LSR_DR;
			break;
		case COM_MSR:	val = MSR_DCD | MSR_DSR | MSR_CTS; break;
		case COM_SCR:	val = uart.scr; break;
		}
		data[0] = val;
		return;
	}

	/* OUT */
	val = data[0];
	switch (reg) {
	case COM_DATA:
		if (dlab) {
			uart.dll = val;
		} else {
			/*
			 * Buffer console output and flush only at end of line.
			 * An fflush() per character forces a write() (and tty
			 * flush) per byte, which on a real terminal costs
			 * hundreds of us each and dominates boot time. Partial
			 * lines (e.g. the shell prompt) are flushed by the
			 * periodic flush in the run loop.
			 */
			putchar((int)val);
			if (val == '\n')
				fflush(stdout);
			uart_kick_tx();		/* THR emptied -> TX int */
		}
		break;
	case COM_IER:
		if (dlab) {
			uart.dlm = val;
		} else {
			uint8_t old = uart.ier;
			uart.ier = val;
			/* Enabling THRI kicks off transmission. */
			if ((val & IER_THRI) && !(old & IER_THRI))
				uart_kick_tx();
			/* Enabling RDAI with data queued raises RX now. */
			if ((val & IER_RDAI) && !rx_empty())
				ioapic_raise(UART_IRQ);
		}
		break;
	case COM_IIR:	break;		/* FCR: no FIFO model */
	case COM_LCR:	uart.lcr = val; break;
	case COM_MCR:	uart.mcr = val; break;
	case COM_LSR:	break;
	case COM_MSR:	break;
	case COM_SCR:	uart.scr = val; break;
	}
}
