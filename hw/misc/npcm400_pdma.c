/*
 * Nuvoton NPCM400 PDMA (Peripheral DMA Controller)
 *
 * Minimal model for the NPCM400 PDMA controller. The Zephyr I3C driver
 * configures DMA channels via CHCTL/STOP/SWREQ and then polls TDSTS to
 * detect transfer completion. This model auto-completes any channel
 * operation by mirroring the written channel bits into TDSTS on every
 * write to CHCTL, STOP, or SWREQ. TDSTS is W1C as on real hardware.
 *
 * Register layout (offsets within the 0x500-byte window):
 *   0x000 - 0x0FF : DSCT[16]  (scatter-gather descriptors, 16 bytes each)
 *   0x100 - 0x3FF : reserved
 *   0x400 : CHCTL  - Channel Enable
 *   0x404 : STOP   - Channel Stop
 *   0x408 : SWREQ  - Software Request
 *   0x40C : TRGSTS - Trigger Status
 *   0x410 : PRISET - Priority Set
 *   0x414 : PRICLR - Priority Clear
 *   0x418 : INTEN  - Interrupt Enable
 *   0x41C : INTSTS - Interrupt Status
 *   0x420 : ABTSTS - Abort Status
 *   0x424 : TDSTS  - Transfer Done Status (W1C)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/npcm400_pdma.h"
#include "migration/vmstate.h"

/* Control/status register offsets */
#define PDMA_CHCTL_OFF  0x400
#define PDMA_STOP_OFF   0x404
#define PDMA_SWREQ_OFF  0x408
#define PDMA_TDSTS_OFF  0x424

static uint64_t npcm400_pdma_read(void *opaque, hwaddr addr, unsigned int size)
{
    NPCM400PDMAState *s = opaque;
    uint64_t value = 0;

    if (addr + size > NPCM400_PDMA_REG_SIZE) {
        return 0;
    }
    for (unsigned int i = 0; i < size; i++) {
        value |= ((uint64_t)s->regs[addr + i]) << (i * 8);
    }
    return value;
}

static void npcm400_pdma_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned int size)
{
    NPCM400PDMAState *s = opaque;
    uint32_t offset, current, written, tdsts;

    if (addr + size > NPCM400_PDMA_REG_SIZE) {
        return;
    }
    for (unsigned int i = 0; i < size; i++) {
        s->regs[addr + i] = (value >> (i * 8)) & 0xff;
    }

    offset = addr & ~0x3u;
    current = ldl_le_p(&s->regs[offset]);

    switch (offset) {
    case PDMA_CHCTL_OFF:
    case PDMA_STOP_OFF:
    case PDMA_SWREQ_OFF:
        /*
         * Auto-complete: mirror the written channel bits into TDSTS so that
         * the readl_poll_timeout loop in the Zephyr I3C driver sees transfer
         * done immediately.
         */
        tdsts = ldl_le_p(&s->regs[PDMA_TDSTS_OFF]);
        tdsts |= current;
        stl_le_p(&s->regs[PDMA_TDSTS_OFF], tdsts);
        break;

    case PDMA_TDSTS_OFF:
        /* W1C: clear bits written as 1 */
        written = 0;
        for (unsigned int i = 0; i < size; i++) {
            written |= ((uint32_t)((value >> (i * 8)) & 0xff)) << (i * 8);
        }
        tdsts = current & ~written;
        stl_le_p(&s->regs[PDMA_TDSTS_OFF], tdsts);
        break;

    default:
        break;
    }
}

static const MemoryRegionOps npcm400_pdma_ops = {
    .read  = npcm400_pdma_read,
    .write = npcm400_pdma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void npcm400_pdma_reset(DeviceState *dev)
{
    NPCM400PDMAState *s = NPCM400_PDMA(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void npcm400_pdma_realize(DeviceState *dev, Error **errp)
{
    NPCM400PDMAState *s = NPCM400_PDMA(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &npcm400_pdma_ops, s,
                          TYPE_NPCM400_PDMA, NPCM400_PDMA_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static const VMStateDescription npcm400_pdma_vmstate = {
    .name = TYPE_NPCM400_PDMA,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, NPCM400PDMAState, NPCM400_PDMA_REG_SIZE),
        VMSTATE_END_OF_LIST(),
    },
};

static void npcm400_pdma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = npcm400_pdma_realize;
    dc->vmsd    = &npcm400_pdma_vmstate;
    device_class_set_legacy_reset(dc, npcm400_pdma_reset);
}

static const TypeInfo npcm400_pdma_info = {
    .name          = TYPE_NPCM400_PDMA,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NPCM400PDMAState),
    .class_init    = npcm400_pdma_class_init,
};

static void npcm400_pdma_register_types(void)
{
    type_register_static(&npcm400_pdma_info);
}
type_init(npcm400_pdma_register_types)
