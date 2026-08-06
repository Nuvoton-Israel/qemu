/*
 * Nuvoton NPCM400 FIU (Flash Interface Unit)
 *
 * Minimal model covering the two FIU register banks and the three XIP decode
 * windows. The register window is 0x3000 bytes and contains two identical
 * 0x100-byte register banks:
 *   offset 0x0000 : core registers (used by the CPU-side driver)
 *   offset 0x1000 : host registers (used by eSPI host-side access)
 *
 * The Zephyr spi_npcm4xx_fiu driver busy-waits on two conditions:
 *
 *   UMA_CTS (offset 0x01E within each bank) bit 7 EXEC_DONE:
 *     The guest sets this bit to start a UMA flash transaction. Hardware
 *     clears it when the transaction completes. This model clears it on
 *     every read so the spin loop exits immediately.
 *
 *   FIU_MSR_STS (offset 0x042 within each bank) bit 2 MSTR_INACT:
 *     Indicates the FIU master is idle; polled before taking the UMA lock.
 *     This model always reports the master as inactive.
 *
 * The three XIP decode windows (private/shared/backup flash) are exposed as
 * SysBus MMIO regions 1-3 (region 0 is the register window) and always
 * return 0xFF to mimic erased flash.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/ssi/npcm400_fiu.h"
#include "migration/vmstate.h"

/* Register offsets within each 0x1000-byte bank (decoded modulo 0x1000) */
#define FIU_UMA_CTS_OFF         0x01e
#define FIU_MSR_STS_OFF         0x042
#define FIU_UMA_CTS_EXEC_DONE   (1u << 7)
#define FIU_MSR_STS_MSTR_INACT  (1u << 2)

/* --------------------------------------------------------------------------
 * XIP decode windows — always return erased-flash pattern (0xFF per byte)
 * -------------------------------------------------------------------------- */

static uint64_t npcm400_fiu_win_read(void *opaque, hwaddr addr,
                                     unsigned int size)
{
    return MAKE_64BIT_MASK(0, size * 8);
}

static void npcm400_fiu_win_write(void *opaque, hwaddr addr,
                                  uint64_t value, unsigned int size)
{
    /* Writes to XIP windows are ignored. */
}

static const MemoryRegionOps npcm400_fiu_win_ops = {
    .read  = npcm400_fiu_win_read,
    .write = npcm400_fiu_win_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* --------------------------------------------------------------------------
 * FIU register window
 * -------------------------------------------------------------------------- */

static uint64_t npcm400_fiu_read(void *opaque, hwaddr addr, unsigned int size)
{
    NPCM400FIUState *s = opaque;
    hwaddr reg = addr & 0xfffu;   /* decode within the 0x1000-byte bank */
    uint64_t value = 0;

    if (addr + size > NPCM400_FIU_REG_SIZE) {
        return 0;
    }
    for (unsigned int i = 0; i < size; i++) {
        value |= ((uint64_t)s->regs[addr + i]) << (i * 8);
    }

    /* UMA transaction completes instantly: EXEC_DONE is always clear. */
    if (reg == FIU_UMA_CTS_OFF) {
        value &= ~(uint64_t)FIU_UMA_CTS_EXEC_DONE;
    }
    /* FIU master is always idle. */
    if (reg == FIU_MSR_STS_OFF) {
        value |= FIU_MSR_STS_MSTR_INACT;
    }
    return value;
}

static void npcm400_fiu_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned int size)
{
    NPCM400FIUState *s = opaque;

    if (addr + size > NPCM400_FIU_REG_SIZE) {
        return;
    }
    for (unsigned int i = 0; i < size; i++) {
        s->regs[addr + i] = (value >> (i * 8)) & 0xff;
    }
}

static const MemoryRegionOps npcm400_fiu_ops = {
    .read  = npcm400_fiu_read,
    .write = npcm400_fiu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* --------------------------------------------------------------------------
 * Device lifecycle
 * -------------------------------------------------------------------------- */

static void npcm400_fiu_reset(DeviceState *dev)
{
    NPCM400FIUState *s = NPCM400_FIU(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void npcm400_fiu_realize(DeviceState *dev, Error **errp)
{
    NPCM400FIUState *s = NPCM400_FIU(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    char name[40];
    int i;

    /* SysBus MMIO 0: register window (core + host banks) */
    memory_region_init_io(&s->mmio, OBJECT(dev), &npcm400_fiu_ops, s,
                          TYPE_NPCM400_FIU, NPCM400_FIU_REG_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);

    /* SysBus MMIO 1-3: XIP decode windows */
    for (i = 0; i < NPCM400_FIU_NUM_WINS; i++) {
        snprintf(name, sizeof(name), TYPE_NPCM400_FIU ".win%d", i);
        memory_region_init_io(&s->win[i], OBJECT(dev), &npcm400_fiu_win_ops,
                              s, name, 0x10000000);
        sysbus_init_mmio(sbd, &s->win[i]);
    }
}

static const VMStateDescription npcm400_fiu_vmstate = {
    .name = TYPE_NPCM400_FIU,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, NPCM400FIUState, NPCM400_FIU_REG_SIZE),
        VMSTATE_END_OF_LIST(),
    },
};

static void npcm400_fiu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = npcm400_fiu_realize;
    dc->vmsd    = &npcm400_fiu_vmstate;
    device_class_set_legacy_reset(dc, npcm400_fiu_reset);
}

static const TypeInfo npcm400_fiu_info = {
    .name          = TYPE_NPCM400_FIU,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NPCM400FIUState),
    .class_init    = npcm400_fiu_class_init,
};

static void npcm400_fiu_register_types(void)
{
    type_register_static(&npcm400_fiu_info);
}
type_init(npcm400_fiu_register_types)
