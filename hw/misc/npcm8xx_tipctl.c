/*
 * Nuvoton NPCM8xx TIP Controller
 *
 * Copyright 2025 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/misc/npcm8xx_tipctl.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"

#define NPCM8XX_TIPCTL_REGS_SIZE (1 * KiB)

static uint64_t npcm8xx_tipctl_read(void *opaque, hwaddr offset, unsigned size)
{
    if (offset == 0x4) {
        return 0x1;
    }

    qemu_log_mask(LOG_UNIMP, "%s: mostly unimplemented\n", __func__);
    return 0;
}

static void npcm8xx_tipctl_write(void *opaque, hwaddr offset,
                              uint64_t v, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "%s: mostly unimplemented\n", __func__);
}


static const struct MemoryRegionOps npcm8xx_tipctl_ops = {
    .read       = npcm8xx_tipctl_read,
    .write      = npcm8xx_tipctl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid      = {
        .min_access_size        = 4,
        .max_access_size        = 4,
        .unaligned              = false,
    },
};

static void npcm8xx_tipctl_realize(DeviceState *dev, Error **errp)
{
    NPCM8xxTIPCTLState *s = NPCM8XX_TIPCTL(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &npcm8xx_tipctl_ops, s,
                          TYPE_NPCM8XX_TIPCTL, NPCM8XX_TIPCTL_REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}


static void nnpcm8xx_tipctl_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "NPCM8xx TIP Controller";
    dc->realize = npcm8xx_tipctl_realize;
}

static const TypeInfo npcm8xx_tipctl_info[] = {
    {
        .name               = TYPE_NPCM8XX_TIPCTL,
        .parent             = TYPE_SYS_BUS_DEVICE,
        .instance_size      = sizeof(NPCM8xxTIPCTLState),
        .class_init         = nnpcm8xx_tipctl_class_init,
    },
};
DEFINE_TYPES(npcm8xx_tipctl_info)
