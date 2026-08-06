/*
 * Nuvoton NPCM4xx UART
 *
 * This implements the minimal UART register behavior needed for firmware
 * using the NPCM4xx UART driver (polling and basic interrupt usage).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "hw/char/npcm4xx_uart.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"

#define NPCM4XX_UART_UTBUF   0x00
#define NPCM4XX_UART_URBUF   0x02
#define NPCM4XX_UART_UICTRL  0x04
#define NPCM4XX_UART_USTAT   0x06
#define NPCM4XX_UART_UFRS    0x08
#define NPCM4XX_UART_UMDSL   0x0a
#define NPCM4XX_UART_UBAUD   0x0c
#define NPCM4XX_UART_UPSR    0x0e
#define NPCM4XX_UART_UFCTRL  0x16
#define NPCM4XX_UART_UTXFLV  0x18
#define NPCM4XX_UART_URXFLV  0x1a

#define NPCM4XX_UICTRL_TBE   BIT(0)
#define NPCM4XX_UICTRL_RBF   BIT(1)
#define NPCM4XX_UICTRL_ETI   BIT(5)
#define NPCM4XX_UICTRL_ERI   BIT(6)
#define NPCM4XX_USTAT_XMIP   BIT(6)

/*
 * Defaults tuned for NPCM400 board SYSCLK/APB2 = 96MHz.
 * Using Zephyr's npcm4xx UART divisor search algorithm, 115200 baud maps to:
 *   UPSR = 0x08, UBAUD = 0x33
 */
#define NPCM4XX_UART_UPSR_115200_AT_96MHZ   0x08
#define NPCM4XX_UART_UBAUD_115200_AT_96MHZ  0x33

static void npcm4xx_uart_update_irq(NPCM4xxUARTState *s)
{
    bool tx_irq = (s->uictrl & NPCM4XX_UICTRL_ETI) != 0;
    bool rx_irq = (s->uictrl & NPCM4XX_UICTRL_ERI) && s->rx_len;

    qemu_set_irq(s->irq, tx_irq || rx_irq);
}

static uint8_t npcm4xx_uart_get_uictrl(NPCM4xxUARTState *s)
{
    uint8_t val = s->uictrl;

    val |= NPCM4XX_UICTRL_TBE;
    if (s->rx_len) {
        val |= NPCM4XX_UICTRL_RBF;
    } else {
        val &= ~NPCM4XX_UICTRL_RBF;
    }

    return val;
}

static uint8_t npcm4xx_uart_pop_rx(NPCM4xxUARTState *s)
{
    uint8_t val;

    if (!s->rx_len) {
        return 0;
    }

    val = s->rx_fifo[s->rx_head];
    s->rx_head = (s->rx_head + 1) % NPCM4XX_UART_FIFO_LEN;
    s->rx_len--;
    s->urxflv = s->rx_len;
    qemu_chr_fe_accept_input(&s->chr);
    npcm4xx_uart_update_irq(s);

    return val;
}

static uint64_t npcm4xx_uart_read(void *opaque, hwaddr addr, unsigned int size)
{
    NPCM4xxUARTState *s = NPCM4XX_UART(opaque);

    if (size != 1) {
        qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: invalid read size %u at 0x%" HWADDR_PRIx "\n",
                  TYPE_NPCM4XX_UART, size, addr);
        return 0;
    }

    switch (addr) {
    case NPCM4XX_UART_URBUF:
        s->urbuf = npcm4xx_uart_pop_rx(s);
        return s->urbuf;
    case NPCM4XX_UART_UICTRL:
        return npcm4xx_uart_get_uictrl(s);
    case NPCM4XX_UART_USTAT:
        return s->ustat;
    case NPCM4XX_UART_UFRS:
        return s->ufrs;
    case NPCM4XX_UART_UMDSL:
        return s->umdsl;
    case NPCM4XX_UART_UBAUD:
        return s->ubaud;
    case NPCM4XX_UART_UPSR:
        return s->upsr;
    case NPCM4XX_UART_UFCTRL:
        return s->ufctrl;
    case NPCM4XX_UART_UTXFLV:
        return 0;
    case NPCM4XX_UART_URXFLV:
        return s->rx_len;
    case NPCM4XX_UART_UTBUF:
        return s->utbuf;
    default:
        qemu_log_mask(LOG_UNIMP,
                  "%s: unimplemented read at 0x%" HWADDR_PRIx "\n",
                  TYPE_NPCM4XX_UART, addr);
        return 0;
    }
}

static void npcm4xx_uart_write(void *opaque, hwaddr addr,
                               uint64_t value, unsigned int size)
{
    NPCM4xxUARTState *s = NPCM4XX_UART(opaque);
    uint8_t ch = value;

    if (size != 1) {
        qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: invalid write size %u at 0x%" HWADDR_PRIx "\n",
                  TYPE_NPCM4XX_UART, size, addr);
        return;
    }

    switch (addr) {
    case NPCM4XX_UART_UTBUF:
        s->utbuf = ch;
        qemu_chr_fe_write_all(&s->chr, &ch, 1);
        s->ustat &= ~NPCM4XX_USTAT_XMIP;
        break;
    case NPCM4XX_UART_UICTRL:
        s->uictrl = value;
        break;
    case NPCM4XX_UART_USTAT:
        s->ustat = value;
        break;
    case NPCM4XX_UART_UFRS:
        s->ufrs = value;
        break;
    case NPCM4XX_UART_UMDSL:
        s->umdsl = value;
        break;
    case NPCM4XX_UART_UBAUD:
        s->ubaud = value;
        break;
    case NPCM4XX_UART_UPSR:
        s->upsr = value;
        break;
    case NPCM4XX_UART_UFCTRL:
        s->ufctrl = value;
        break;
    case NPCM4XX_UART_UTXFLV:
    case NPCM4XX_UART_URXFLV:
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                  "%s: unimplemented write at 0x%" HWADDR_PRIx "\n",
                  TYPE_NPCM4XX_UART, addr);
        break;
    }

    npcm4xx_uart_update_irq(s);
}

static const MemoryRegionOps npcm4xx_uart_ops = {
    .read = npcm4xx_uart_read,
    .write = npcm4xx_uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void npcm4xx_uart_reset(DeviceState *dev)
{
    NPCM4xxUARTState *s = NPCM4XX_UART(dev);

    s->utbuf = 0;
    s->urbuf = 0;
    s->uictrl = NPCM4XX_UICTRL_TBE;
    s->ustat = 0;
    s->ufrs = 0;
    s->umdsl = 0;
    s->ubaud = NPCM4XX_UART_UBAUD_115200_AT_96MHZ;
    s->upsr = NPCM4XX_UART_UPSR_115200_AT_96MHZ;
    s->ufctrl = 0;
    s->utxflv = 0;
    s->urxflv = 0;
    s->rx_head = 0;
    s->rx_len = 0;

    npcm4xx_uart_update_irq(s);
}

static int npcm4xx_uart_can_receive(void *opaque)
{
    NPCM4xxUARTState *s = NPCM4XX_UART(opaque);

    return NPCM4XX_UART_FIFO_LEN - s->rx_len;
}

static void npcm4xx_uart_receive(void *opaque, const uint8_t *buf, int size)
{
    NPCM4xxUARTState *s = NPCM4XX_UART(opaque);
    int i;

    for (i = 0; i < size && s->rx_len < NPCM4XX_UART_FIFO_LEN; i++) {
        uint8_t pos = (s->rx_head + s->rx_len) % NPCM4XX_UART_FIFO_LEN;
        s->rx_fifo[pos] = buf[i];
        s->rx_len++;
    }

    s->urxflv = s->rx_len;
    npcm4xx_uart_update_irq(s);
}

static void npcm4xx_uart_event(void *opaque, QEMUChrEvent event)
{
    NPCM4xxUARTState *s = NPCM4XX_UART(opaque);

    if (event == CHR_EVENT_BREAK) {
        s->ustat |= BIT(4);
        npcm4xx_uart_update_irq(s);
    }
}

static void npcm4xx_uart_realize(DeviceState *dev, Error **errp)
{
    NPCM4xxUARTState *s = NPCM4XX_UART(dev);

    qemu_chr_fe_set_handlers(&s->chr, npcm4xx_uart_can_receive,
                             npcm4xx_uart_receive, npcm4xx_uart_event,
                             NULL, s, NULL, true);
}

static void npcm4xx_uart_init(Object *obj)
{
    NPCM4xxUARTState *s = NPCM4XX_UART(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mmio, obj, &npcm4xx_uart_ops, s,
                          TYPE_NPCM4XX_UART, 0x100);
    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_irq(sbd, &s->irq);
}

static const Property npcm4xx_uart_properties[] = {
    DEFINE_PROP_CHR("chardev", NPCM4xxUARTState, chr),
};

static void npcm4xx_uart_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = npcm4xx_uart_realize;
    device_class_set_legacy_reset(dc, npcm4xx_uart_reset);
    device_class_set_props(dc, npcm4xx_uart_properties);
}

static const TypeInfo npcm4xx_uart_info = {
    .name = TYPE_NPCM4XX_UART,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NPCM4xxUARTState),
    .instance_init = npcm4xx_uart_init,
    .class_init = npcm4xx_uart_class_init,
};

static void npcm4xx_uart_register_types(void)
{
    type_register_static(&npcm4xx_uart_info);
}

type_init(npcm4xx_uart_register_types)
