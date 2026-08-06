/*
 * Nuvoton NPCM4xx UART
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_NPCM4XX_UART_H
#define HW_NPCM4XX_UART_H

#include "hw/core/sysbus.h"
#include "chardev/char-fe.h"
#include "qom/object.h"

#define TYPE_NPCM4XX_UART "npcm4xx-uart"
OBJECT_DECLARE_SIMPLE_TYPE(NPCM4xxUARTState, NPCM4XX_UART)

#define NPCM4XX_UART_FIFO_LEN 16

struct NPCM4xxUARTState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    CharFrontend chr;
    qemu_irq irq;

    uint8_t utbuf;
    uint8_t urbuf;
    uint8_t uictrl;
    uint8_t ustat;
    uint8_t ufrs;
    uint8_t umdsl;
    uint8_t ubaud;
    uint8_t upsr;
    uint8_t ufctrl;
    uint8_t utxflv;
    uint8_t urxflv;

    uint8_t rx_fifo[NPCM4XX_UART_FIFO_LEN];
    uint8_t rx_head;
    uint8_t rx_len;
};

#endif