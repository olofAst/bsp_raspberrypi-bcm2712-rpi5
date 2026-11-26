/*
 * $QNXLicenseC:
 * Copyright 2025 QNX Software Systems.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You
 * may not reproduce, modify or distribute this software except in
 * compliance with the License. You may obtain a copy of the License
 * at: http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTIES OF ANY KIND, either express or implied.
 *
 * This file may contain contributions from others, either as
 * contributors under the License or as licensors under other terms.
 * Please review this entire file for other proprietary rights or license
 * notices, as well as the QNX Development Suite License Guide at
 * http://licensing.qnx.com/license-guide/ for other information.
 * $
 */

/*
 * Raspberry Pi 5 RP1 support for the PL011 driver.
 *
 * The six RP1 UARTs raise level interrupts that reach the GIC as MSI-X
 * vectors (GIC interrupt 160 + RP1 irq). Their MSIn_CFG.IACK_EN bit is set,
 * so the vector is only re-armed once MSIn_CFG.IACK has been written after
 * the UART has been serviced. The SoC debug UART (UART10, irq 153) is a
 * plain GIC interrupt and needs none of this.
 */

#include "externs.h"
#include <sys/mman.h>
#include <sys/slog.h>
#include <sys/slogcodes.h>

#define RP1_MSIX_GIC_IRQ_BASE       160U
#define RP1_MSIX_CFG_PADDR          0x1f00108000ULL
#define RP1_MSIX_CFG_SIZE           0x1000U
#define RP1_MSIX_CFG_SET(irq)       (0x800U + 0x008U + ((irq) * 4U))
#define RP1_MSIX_CFG_IACK           (1U << 2)

static const uint8_t rp1_uart_irqs[] = { 25, 42, 43, 44, 45, 46 };   /* UART0..UART5 */

void
rp1_iack_init(DEV_PL011 *dev)
{
	unsigned	i;

	dev->rp1_iack_reg = 0;
	for (i = 0; i < sizeof(rp1_uart_irqs) / sizeof(rp1_uart_irqs[0]); i++) {
		if (dev->intr == RP1_MSIX_GIC_IRQ_BASE + rp1_uart_irqs[i]) {
			uintptr_t const base = mmap_device_io(RP1_MSIX_CFG_SIZE, RP1_MSIX_CFG_PADDR);
			if (base == (uintptr_t)MAP_FAILED) {
				slogf(_SLOG_SETCODE(_SLOGC_CHAR, 0), _SLOG_ERROR,
					"devc-serpl011: unable to map RP1 MSI-X config (%s)", strerror(errno));
				return;
			}
			dev->rp1_iack_reg = base + RP1_MSIX_CFG_SET(rp1_uart_irqs[i]);
			return;
		}
	}
}
