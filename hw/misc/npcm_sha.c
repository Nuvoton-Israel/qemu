/*
 * Nuvoton NPCM7xx/8xx SHA Module
 *
 * Copyright (c) Nuvoton Technology Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "qemu/osdep.h"
#include "hw/misc/npcm_sha.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "trace.h"
#include <nettle/sha.h>
#include <byteswap.h>

#define NPCM8XX_SHA_REG_SIZE   0x1000
#define NPCM8XX_SHA_DIN_SWAP   BIT(7)
#define NPCM8XX_SHA_DOUT_SWAP  BIT(6)
#define NPCM8XX_SHA_RST        BIT(2)
#define NPCM8XX_SHA_BUSY       BIT(1)
#define NPCM8XX_SHA_EN         BIT(0)
#define NPCM8XX_SHA_SHA1       BIT(0)
#define NPCM8XX_SHA_CMD_LOAD   BIT(4)
#define NPCM8XX_SHA_CMD_512    BIT(3)
#define NPCM8XX_SHA_CMD_ROUND  BIT(2)
#define NPCM8XX_SHA_CMD_WR     BIT(1)
#define NPCM8XX_SHA_CMD_RD     BIT(0)

enum {
    NPCM_SHA_DATA_IN_REG = 0x00,
    NPCM_SHA_CTR_STS_REG = 0x04,
    NPCM_SHA_CFG_REG = 0x08,
    NPCM_HASH_VER = 0x0C,
    NPCM_SHA512_DATA_IN_REG = 0x10,
    NPCM_SHA512_CTR_STS_REG = 0x14,
    NPCM_SHA512_CMD_REG = 0x18,
    NPCM_SHA512_HASH_OUT_REG = 0x1C,
    NPCM_SHA_HASH_OUT_REG = 0x20,
    NPCM_SHA_HASH_OUT_LAST_REG = 0x3C,
};

static void sha256_starts(NPCM8xxSHAState *s)
{
    sha256_init(&s->sha256ctx);
}
static void sha1_starts(NPCM8xxSHAState *s)
{
    sha1_init(&s->sha1ctx);
}
static void sha512_starts(NPCM8xxSHAState *s, bool is_sha512)
{
    if (is_sha512)
        sha512_init(&s->sha512ctx);
    else
        sha384_init(&s->sha512ctx);
}
static void npcm8xx_sha_write_data(NPCM8xxSHAState *s, uint32_t data)
{
    if ((s->sha_ctr_sts & NPCM8XX_SHA_DIN_SWAP) == 0)
        data = bswap_32(data);
    uint8_t *buf = (uint8_t*)(&data);
    s->sha_ctr_sts |= NPCM8XX_SHA_BUSY;
    if (s->sha_cfg & NPCM8XX_SHA_SHA1)
        sha1_update(&s->sha1ctx, 4, buf);
    else
        sha256_update(&s->sha256ctx, 4, buf);
    s->sha_ctr_sts &= ~NPCM8XX_SHA_BUSY;
}
static void npcm8xx_sha512_write_data(NPCM8xxSHAState *s, uint32_t data)
{
    if (s->sha512_cmd & NPCM8XX_SHA_CMD_LOAD)
    {
        uint32_t *state = (uint32_t *)s->sha512ctx.state;
        if (s->sha512_bytes_index % 2 == 0)
            state[s->sha512_bytes_index+1] = data;
        else
            state[s->sha512_bytes_index-1] = data;
        s->sha512_bytes_index++;
        if (s->sha512_bytes_index == 16) {
            // Note: user must clear load bit after load data manually
            s->sha512_bytes_index = 0;
        }
        return;
    }
    if ((s->sha512_ctr_sts & NPCM8XX_SHA_DIN_SWAP) == 0)
        data = bswap_32(data);
    uint8_t *buf = (uint8_t*)(&data);
    s->sha512_ctr_sts |= NPCM8XX_SHA_BUSY;
    // sha384 and sha512 use the same compress function
    sha512_update(&s->sha512ctx, 4, buf);
    s->sha512_ctr_sts &= ~NPCM8XX_SHA_BUSY;
    if (s->sha512_bytes_index == 32) {
        s->sha512_bytes_index = 0;
    }
}
// we should allow sha384 read all state for load it later
static uint32_t npcm8xx_sha512_read_data(NPCM8xxSHAState *s)
{
    uint32_t value;
    if (s->sha512_bytes_index % 2 == 0)
    {
        // return high 32 bits
        value = s->sha512ctx.state[s->sha512_bytes_index / 2] >> 32;
    }
    else
    {
        value = s->sha512ctx.state[s->sha512_bytes_index / 2] & 0xffffffff;
    }
    if (s->sha512_ctr_sts & NPCM8XX_SHA_EN)
        s->sha512_bytes_index++;
    if (s->sha512_bytes_index == 16)
        s->sha512_bytes_index = 0;
    return value;
}
static void npcm8xx_sha512_handle_cmd(NPCM8xxSHAState *s, uint8_t cmd)
{
    // case first round
    if (cmd & NPCM8XX_SHA_CMD_WR && (cmd & NPCM8XX_SHA_CMD_ROUND) == 0)
    {
        sha512_starts(s, cmd & NPCM8XX_SHA_CMD_512);
    }
    // case set read/write command bit
    if (cmd & NPCM8XX_SHA_CMD_WR || cmd & NPCM8XX_SHA_CMD_RD)
    {
        s->sha512_bytes_index = 0;
    }
    // case clear load bit
    if (s->sha512_cmd & NPCM8XX_SHA_CMD_LOAD &&
         (cmd & NPCM8XX_SHA_CMD_LOAD) == 0)
        s->sha512_bytes_index = 0;
}
static void npcm8xx_sha512_reset(NPCM8xxSHAState *s)
{
    s->sha512_bytes_index = 0;
    memset(&s->sha512ctx, 0, sizeof(struct sha512_ctx));
}

static void npcm8xx_sha_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    NPCM8xxSHAState *s = opaque;
    trace_npcm8xx_sha_write(offset, value, size);
    uint8_t i;

    switch (offset) {
    case NPCM_SHA_CFG_REG:
        s->sha_cfg = value & 0xff; // only 8 bits
        break;
    case NPCM_SHA_HASH_OUT_REG ... NPCM_SHA_HASH_OUT_LAST_REG:
        // we support clear hash output manually once enable bit is set
        if (s->sha_ctr_sts & NPCM8XX_SHA_EN &&
                (s->sha_ctr_sts & NPCM8XX_SHA_BUSY) == 0) {
            i = (offset - NPCM_SHA_HASH_OUT_REG) / 4;
            s->sha256ctx.state[i] = value;
            if (i < _SHA1_DIGEST_LENGTH)
                s->sha1ctx.state[i] = value;
        }
        break;
    case NPCM_SHA_CTR_STS_REG:
        value &= 0xff;
        // clear hash output if reset bit is set
        if (value & NPCM8XX_SHA_RST) {
            // initialize hash output
            if (s->sha_cfg & NPCM8XX_SHA_SHA1) {
                sha1_starts(s);
            } else {
                sha256_starts(s);
            }
            value &= ~NPCM8XX_SHA_RST;
        }
        if (value & NPCM8XX_SHA_BUSY)
            value &= ~NPCM8XX_SHA_BUSY;
        if (s->sha_ctr_sts & NPCM8XX_SHA_BUSY)
            value |= NPCM8XX_SHA_BUSY;
        s->sha_ctr_sts = value;
        break;
    case NPCM_SHA_DATA_IN_REG:
        if ((s->sha_ctr_sts & NPCM8XX_SHA_EN) == 0 ||
                (s->sha_ctr_sts & NPCM8XX_SHA_BUSY))
            break;
        // Process data input
        npcm8xx_sha_write_data(s, value);
        break;
    case NPCM_SHA512_DATA_IN_REG:
        if ((s->sha512_ctr_sts & NPCM8XX_SHA_EN) == 0 ||
                (s->sha512_ctr_sts & NPCM8XX_SHA_BUSY))
            break;
        npcm8xx_sha512_write_data(s, value);
        break;
    case NPCM_SHA512_CTR_STS_REG:
        value &= 0xff;
        if (value & NPCM8XX_SHA_RST) {
            npcm8xx_sha512_reset(s);
            value &= ~NPCM8XX_SHA_RST;
        }
        if (value & NPCM8XX_SHA_BUSY)
            value &= ~NPCM8XX_SHA_BUSY;
        if (s->sha512_ctr_sts & NPCM8XX_SHA_BUSY)
            value |= NPCM8XX_SHA_BUSY;
        s->sha512_ctr_sts = value;
        break;
    case NPCM_SHA512_CMD_REG:
        value &= 0xff;
        if ((s->sha512_ctr_sts & NPCM8XX_SHA_EN) == 0 ||
                (s->sha512_ctr_sts & NPCM8XX_SHA_BUSY))
            break;
        s->sha512_cmd = value;
        npcm8xx_sha512_handle_cmd(s, value);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "npcm8xx_sha: Bad write offset %x\n", (unsigned int)offset);
        break;
    }
}

static uint64_t npcm8xx_sha_read(void *opaque, hwaddr offset, unsigned size)
{
    NPCM8xxSHAState *s = opaque;
    uint64_t value = 0;

    switch (offset) {
    case NPCM_SHA512_HASH_OUT_REG:
        value = npcm8xx_sha512_read_data(s);
        if ((s->sha512_ctr_sts & NPCM8XX_SHA_DOUT_SWAP) == 0)
            value = bswap_32(value);
        break;
    case NPCM_SHA_HASH_OUT_REG ... NPCM_SHA_HASH_OUT_LAST_REG:
        uint8_t i = (offset - NPCM_SHA_HASH_OUT_REG) / 4;
        if (s->sha_cfg & NPCM8XX_SHA_SHA1) {
            if (i < _SHA1_DIGEST_LENGTH)
                value = s->sha1ctx.state[i];
        } else {
            value = s->sha256ctx.state[i];
        }
        if ((s->sha_ctr_sts & NPCM8XX_SHA_DOUT_SWAP) == 0)
            value = bswap_32(value);
        break;
    case NPCM_SHA512_CMD_REG:
        value = s->sha512_cmd;
        break;
    case NPCM_SHA512_CTR_STS_REG:
        value = s->sha512_ctr_sts;
        break;
    case NPCM_SHA_CFG_REG:
        value = s->sha_cfg;
        break;
    case NPCM_SHA_CTR_STS_REG:
        value = s->sha_ctr_sts;
        break;
    case NPCM_HASH_VER:
        value = 0x3;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "npcm8xx_sha: Bad read offset %x\n", (unsigned int)offset);
        break;
    }

    trace_npcm8xx_sha_read(offset, value, size);
    return value;
}

static const MemoryRegionOps npcm8xx_sha_ops = {
    .read = npcm8xx_sha_read,
    .write = npcm8xx_sha_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void npcm8xx_sha_init(Object *obj)
{
    NPCM8xxSHAState *s = NPCM8XX_SHA(obj);

    memory_region_init_io(&s->iomem, obj, &npcm8xx_sha_ops, s, "npcm8xx-sha",
                          NPCM8XX_SHA_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

// clear state
static void npcm8xx_sha_reset(Object *obj, ResetType type)
{
    NPCM8xxSHAState *s = NPCM8XX_SHA(obj);

    s->sha_cfg = 0;
    s->sha_ctr_sts = 0x80;
    s->sha512_ctr_sts = 0x80;
    s->sha512_cmd = 0;
    memset(&s->sha1ctx, 0, sizeof(struct sha1_ctx));
    memset(&s->sha256ctx, 0, sizeof(struct sha256_ctx));
    npcm8xx_sha512_reset(s);
}

static const VMStateDescription vmstate_npcm8xx_sha = {
    .name = "npcm8xx_sha",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(sha_ctr_sts, NPCM8xxSHAState),
        VMSTATE_UINT8(sha_cfg, NPCM8xxSHAState),
        VMSTATE_UINT8(sha512_ctr_sts, NPCM8xxSHAState),
        VMSTATE_UINT8(sha512_cmd, NPCM8xxSHAState),
        VMSTATE_UINT32(sha512_bytes_index, NPCM8xxSHAState),
        VMSTATE_END_OF_LIST(),
    }
};

static void npcm8xx_sha_class_init(ObjectClass *klass, void *data)
{
     ResettableClass *rc = RESETTABLE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "NPCM8xx SHA Module";
    dc->vmsd = &vmstate_npcm8xx_sha;
    rc->phases.enter= npcm8xx_sha_reset;
}

static const TypeInfo npcm8xx_sha_info[] = {
    {
        .name          = TYPE_NPCM8XX_SHA,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(NPCM8xxSHAState),
        .instance_init = npcm8xx_sha_init,
        .class_init    = npcm8xx_sha_class_init,
    }
};

DEFINE_TYPES(npcm8xx_sha_info);