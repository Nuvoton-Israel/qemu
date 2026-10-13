/*
 * Nuvoton NPCM8xx GDMA controller (functional model)
 *
 * Runs a programmed transfer to completion the moment a channel is enabled
 * and sets the TC bit.  Peripheral request handshakes (XREQ) are not
 * modelled: a fixed-address side is accessed one word at a time, which is how
 * the AES FIFO is fed and drained by the Linux driver
 * (drivers/crypto/npcm/npcm_aes.c).
 *
 * Copyright 2026 Dell Technologies
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "hw/misc/npcm8xx_gdma.h"

static void npcm_gdma_run(uint32_t *r)
{
    uint32_t ctl = r[NPCM_GDMA_CTL];
    hwaddr src = r[NPCM_GDMA_SRCB];
    hwaddr dst = r[NPCM_GDMA_DSTB];
    unsigned words = (ctl & NPCM_GDMA_CTL_BURST) ? 4 : 1;
    uint32_t count = r[NPCM_GDMA_TCNT];
    uint32_t unit;

    if (count > NPCM_GDMA_MAX_TCNT) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "npcm8xx-gdma: TCNT %u exceeds limit, clamped to %u\n",
                      count, NPCM_GDMA_MAX_TCNT);
        count = NPCM_GDMA_MAX_TCNT;
    }

    for (unit = 0; unit < count; unit++) {
        for (unsigned w = 0; w < words; w++) {
            MemTxResult res;
            uint32_t v = address_space_ldl_le(&address_space_memory, src,
                                              MEMTXATTRS_UNSPECIFIED, &res);
            if (res != MEMTX_OK) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "npcm8xx-gdma: bad source 0x%" HWADDR_PRIx "\n",
                              src);
                goto done;
            }
            address_space_stl_le(&address_space_memory, dst, v,
                                 MEMTXATTRS_UNSPECIFIED, &res);
            if (res != MEMTX_OK) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "npcm8xx-gdma: bad dest 0x%" HWADDR_PRIx "\n",
                              dst);
                goto done;
            }
            if (!(ctl & NPCM_GDMA_CTL_SAFIX)) {
                src += 4;
            }
            if (!(ctl & NPCM_GDMA_CTL_DAFIX)) {
                dst += 4;
            }
        }
    }

done:
    r[NPCM_GDMA_CSRC] = src;
    r[NPCM_GDMA_CDST] = dst;
    r[NPCM_GDMA_CTCNT] = 0;
    r[NPCM_GDMA_CTL] = (ctl & ~NPCM_GDMA_CTL_EN) | NPCM_GDMA_CTL_TC;
}

static uint64_t npcm_gdma_read(void *opaque, hwaddr addr, unsigned size)
{
    Npcm8xxGdmaState *s = opaque;
    unsigned ch = addr / NPCM_GDMA_CH_STRIDE;
    unsigned reg = (addr % NPCM_GDMA_CH_STRIDE) / 4;

    if (ch >= NPCM_GDMA_NR_CHANNELS || reg >= NPCM_GDMA_NR_REGS) {
        qemu_log_mask(LOG_UNIMP,
                      "npcm8xx-gdma: read from unimplemented 0x%" HWADDR_PRIx
                      "\n", addr);
        return 0;
    }
    return s->reg[ch][reg];
}

static void npcm_gdma_write(void *opaque, hwaddr addr,
                            uint64_t val, unsigned size)
{
    Npcm8xxGdmaState *s = opaque;
    unsigned ch = addr / NPCM_GDMA_CH_STRIDE;
    unsigned reg = (addr % NPCM_GDMA_CH_STRIDE) / 4;
    uint32_t *r;

    if (ch >= NPCM_GDMA_NR_CHANNELS || reg >= NPCM_GDMA_NR_REGS) {
        qemu_log_mask(LOG_UNIMP,
                      "npcm8xx-gdma: write to unimplemented 0x%" HWADDR_PRIx
                      "\n", addr);
        return;
    }
    r = s->reg[ch];

    switch (reg) {
    case NPCM_GDMA_CTL:
        r[NPCM_GDMA_CTL] = val & ~NPCM_GDMA_CTL_TC;
        if (val & NPCM_GDMA_CTL_EN) {
            npcm_gdma_run(r);
        }
        break;
    case NPCM_GDMA_SRCB:
    case NPCM_GDMA_DSTB:
    case NPCM_GDMA_TCNT:
        r[reg] = val;
        break;
    default:
        /* CSRC/CDST/CTCNT are read-only */
        break;
    }
}

static const MemoryRegionOps npcm_gdma_ops = {
    .read = npcm_gdma_read,
    .write = npcm_gdma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void npcm_gdma_reset_hold(Object *obj, ResetType type)
{
    Npcm8xxGdmaState *s = NPCM8XX_GDMA(obj);

    memset(s->reg, 0, sizeof(s->reg));
}

static const VMStateDescription vmstate_npcm8xx_gdma = {
    .name = TYPE_NPCM8XX_GDMA,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_2DARRAY(reg, Npcm8xxGdmaState,
                               NPCM_GDMA_NR_CHANNELS, NPCM_GDMA_NR_REGS),
        VMSTATE_END_OF_LIST()
    }
};

static void npcm_gdma_init(Object *obj)
{
    Npcm8xxGdmaState *s = NPCM8XX_GDMA(obj);

    memory_region_init_io(&s->mmio, obj, &npcm_gdma_ops, s,
                          TYPE_NPCM8XX_GDMA, NPCM_GDMA_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static void npcm_gdma_class_init(ObjectClass *klass, const void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    rc->phases.hold = npcm_gdma_reset_hold;
    dc->vmsd = &vmstate_npcm8xx_gdma;
    dc->desc = "NPCM8xx GDMA controller";
}

static const TypeInfo npcm8xx_gdma_info = {
    .name          = TYPE_NPCM8XX_GDMA,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Npcm8xxGdmaState),
    .instance_init = npcm_gdma_init,
    .class_init    = npcm_gdma_class_init,
};

static void npcm8xx_gdma_register_types(void)
{
    type_register_static(&npcm8xx_gdma_info);
}

type_init(npcm8xx_gdma_register_types)
