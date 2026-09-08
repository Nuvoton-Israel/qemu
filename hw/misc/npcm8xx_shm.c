/*
 * Nuvoton NPCM8xx Shared Memory (SHM) module emulation.
 *
 * See include/hw/misc/npcm8xx_shm.h for the register layout and for the
 * QEMU-private SHM_SIM_ACC host-access trigger.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/npcm8xx_shm.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

/* Register offsets */
#define SHM_SMC_STS     0x00
#define   SHM_ACC       BIT(6)
#define SHM_SMC_CTL     0x01
#define   SHM_ACC_IE    BIT(5)
#define SHM_WIN_SIZE    0x07
#define SHM_WIN_BASE1   0x20
#define SHM_WIN_BASE2   0x24
#define SHM_WINE_SIZE   0x87
#define SHM_WIN_BASE3   0xA0
#define SHM_WIN_BASE4   0xA4

/*
 * QEMU-private trigger: writing a non-zero byte here sets SMC_STS.SHM_ACC,
 * emulating a host access to a mapped window.  Not a real NPCM8xx register.
 */
#define SHM_SIM_ACC     0xF00

static void npcm8xx_shm_update_irq(NPCM8xxSHMState *s)
{
    bool level = (s->regs[SHM_SMC_STS] & SHM_ACC) &&
                 (s->regs[SHM_SMC_CTL] & SHM_ACC_IE);

    qemu_set_irq(s->irq, level);
}

static uint64_t npcm8xx_shm_read(void *opaque, hwaddr offset, unsigned size)
{
    NPCM8xxSHMState *s = NPCM8XX_SHM(opaque);

    /* impl.max_access_size is 1, so QEMU splits wider accesses for us. */
    return s->regs[offset];
}

static void npcm8xx_shm_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    NPCM8xxSHMState *s = NPCM8XX_SHM(opaque);
    uint8_t val = value;

    switch (offset) {
    case SHM_SMC_STS:
        /* SHM_ACC is write-1-to-clear; the other status bits are read-only. */
        s->regs[SHM_SMC_STS] &= ~(val & SHM_ACC);
        break;

    case SHM_SIM_ACC:
        if (val) {
            s->regs[SHM_SMC_STS] |= SHM_ACC;
        }
        break;

    default:
        s->regs[offset] = val;
        break;
    }

    npcm8xx_shm_update_irq(s);
}

static const MemoryRegionOps npcm8xx_shm_ops = {
    .read = npcm8xx_shm_read,
    .write = npcm8xx_shm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void npcm8xx_shm_reset(DeviceState *dev)
{
    NPCM8xxSHMState *s = NPCM8XX_SHM(dev);

    memset(s->regs, 0, sizeof(s->regs));
    npcm8xx_shm_update_irq(s);
}

static void npcm8xx_shm_realize(DeviceState *dev, Error **errp)
{
    NPCM8xxSHMState *s = NPCM8XX_SHM(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &npcm8xx_shm_ops, s,
                          TYPE_NPCM8XX_SHM, NPCM8XX_SHM_REGS_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static const VMStateDescription vmstate_npcm8xx_shm = {
    .name = "npcm8xx-shm",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, NPCM8xxSHMState, NPCM8XX_SHM_REGS_SIZE),
        VMSTATE_END_OF_LIST(),
    },
};

static void npcm8xx_shm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc    = "NPCM8xx Shared Memory (SHM) module";
    dc->realize = npcm8xx_shm_realize;
    dc->vmsd    = &vmstate_npcm8xx_shm;
    device_class_set_legacy_reset(dc, npcm8xx_shm_reset);
}

static const TypeInfo npcm8xx_shm_info = {
    .name          = TYPE_NPCM8XX_SHM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NPCM8xxSHMState),
    .class_init    = npcm8xx_shm_class_init,
};

static void npcm8xx_shm_register_types(void)
{
    type_register_static(&npcm8xx_shm_info);
}

type_init(npcm8xx_shm_register_types)
