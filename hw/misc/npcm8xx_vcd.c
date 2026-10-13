/*
 * Nuvoton NPCM8xx Video Capture and Differentiation (VCD) engine
 *
 * Models the VCD as it behaves when the host is not driving video: register
 * state is kept, reset and capture/compare commands complete at once, and the
 * difference table stays clear, so software sees an unchanged blank screen.
 *
 * Copyright 2026 Dell Technologies
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/npcm8xx_vcd.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

static void npcm8xx_vcd_update_irq(Npcm8xxVcdState *s)
{
    uint32_t inte = s->regs[NPCM_VCD_INTE / 4];

    qemu_set_irq(s->irq, !!(s->stat & inte & NPCM_VCD_STAT_W1C));
}

static uint64_t npcm8xx_vcd_read(void *opaque, hwaddr offset, unsigned size)
{
    Npcm8xxVcdState *s = opaque;

    if (offset == NPCM_VCD_STAT) {
        uint32_t inte = s->regs[NPCM_VCD_INTE / 4];

        return s->stat |
               ((s->stat & inte & NPCM_VCD_STAT_W1C) ? NPCM_VCD_STAT_IRQ : 0);
    }
    return s->regs[offset / 4];
}

static void npcm8xx_vcd_write(void *opaque, hwaddr offset, uint64_t v,
                              unsigned size)
{
    Npcm8xxVcdState *s = opaque;
    uint32_t value = v;

    switch (offset) {
    case NPCM_VCD_FB_LP:
        s->regs[offset / 4] = value & NPCM_VCD_FB_LP_WMASK;
        break;
    case NPCM_VCD_CAP_RES:
        s->regs[offset / 4] = value & NPCM_VCD_CAP_RES_WMASK;
        break;
    case NPCM_VCD_STAT:
        s->stat &= ~(value & NPCM_VCD_STAT_W1C);
        break;
    case NPCM_VCD_CMD:
        /* GO and RST are self-clearing */
        s->regs[offset / 4] = value & ~(NPCM_VCD_CMD_GO | NPCM_VCD_CMD_RST);
        if (value & NPCM_VCD_CMD_RST) {
            s->stat = NPCM_VCD_STAT_DONE;
        }
        if (value & NPCM_VCD_CMD_GO) {
            /* nothing to capture: finish on the last programmed line */
            uint32_t vdisp = s->regs[NPCM_VCD_CAP_RES / 4] &
                             NPCM_VCD_CAPRES_V_MASK;

            s->stat &= ~(NPCM_VCD_STAT_BUSY | NPCM_VCD_STAT_LINE_MASK);
            s->stat |= NPCM_VCD_STAT_DONE |
                       (vdisp << NPCM_VCD_STAT_LINE_SHIFT);
        }
        break;
    default:
        s->regs[offset / 4] = value;
        break;
    }
    npcm8xx_vcd_update_irq(s);
}

static const MemoryRegionOps npcm8xx_vcd_ops = {
    .read = npcm8xx_vcd_read,
    .write = npcm8xx_vcd_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void npcm8xx_vcd_reset_hold(Object *obj, ResetType type)
{
    Npcm8xxVcdState *s = NPCM8XX_VCD(obj);

    memset(s->regs, 0, sizeof(s->regs));
    s->stat = 0;
}

static void npcm8xx_vcd_init(Object *obj)
{
    Npcm8xxVcdState *s = NPCM8XX_VCD(obj);

    memory_region_init_io(&s->iomem, obj, &npcm8xx_vcd_ops, s,
                          TYPE_NPCM8XX_VCD, NPCM_VCD_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const VMStateDescription vmstate_npcm8xx_vcd = {
    .name = TYPE_NPCM8XX_VCD,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Npcm8xxVcdState,
                             NPCM_VCD_MMIO_SIZE / sizeof(uint32_t)),
        VMSTATE_UINT32(stat, Npcm8xxVcdState),
        VMSTATE_END_OF_LIST(),
    },
};

static void npcm8xx_vcd_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "NPCM8xx Video Capture and Differentiation";
    dc->vmsd = &vmstate_npcm8xx_vcd;
    rc->phases.hold = npcm8xx_vcd_reset_hold;
}

static const TypeInfo npcm8xx_vcd_info = {
    .name = TYPE_NPCM8XX_VCD,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Npcm8xxVcdState),
    .instance_init = npcm8xx_vcd_init,
    .class_init = npcm8xx_vcd_class_init,
};

static void npcm8xx_vcd_register_types(void)
{
    type_register_static(&npcm8xx_vcd_info);
}

type_init(npcm8xx_vcd_register_types)
