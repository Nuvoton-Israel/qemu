/*
 * Nuvoton NPCM8xx AES Engine
 *
 * Emulates the NPCM845 AES crypto accelerator with FIFO-based data path.
 * Supports ECB, CBC, and CTR modes with 128/192/256-bit keys.
 *
 * Register map at base + 0x400..0x600 matches the Nuvoton TRM and the
 * Linux kernel driver at drivers/crypto/npcm/npcm_aes.c.
 *
 * Copyright 2026 Dell Technologies
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/misc/npcm8xx_aes.h"
#include "crypto/aes.h"

enum {
    AES_MODE_ECB = 0,
    AES_MODE_CBC = 1,
    AES_MODE_CTR = 2,
    AES_MODE_MAC = 3,
};

enum {
    AES_KEY_128 = 0,
    AES_KEY_192 = 1,
    AES_KEY_256 = 2,
};

static int npcm_aes_key_bytes(uint32_t nk)
{
    switch (nk) {
    case AES_KEY_128: return 16;
    case AES_KEY_192: return 24;
    case AES_KEY_256: return 32;
    default:          return 16;
    }
}

static void npcm_aes_prepare_keys(Npcm8xxAesState *s)
{
    uint32_t nk = (s->control >> NPCM_AES_CTRL_NK_SHIFT)
                  & NPCM_AES_CTRL_NK_MASK;
    int bits = npcm_aes_key_bytes(nk) * 8;

    AES_set_encrypt_key(s->key, bits, &s->enc_key);
    AES_set_decrypt_key(s->key, bits, &s->dec_key);
    s->keys_valid = true;
}

static void npcm_aes_increment_ctr(uint8_t ctr[16])
{
    for (int i = 15; i >= 0; i--) {
        if (++ctr[i] != 0) {
            break;
        }
    }
}

static void npcm_aes_process_block(Npcm8xxAesState *s)
{
    uint32_t mode = (s->control >> NPCM_AES_CTRL_MODE_SHIFT)
                    & NPCM_AES_CTRL_MODE_MASK;
    bool decrypt = (s->control >> NPCM_AES_CTRL_DEC_ENC_SHIFT) & 1;

    if (!s->keys_valid) {
        npcm_aes_prepare_keys(s);
    }

    switch (mode) {
    case AES_MODE_ECB:
        if (decrypt) {
            AES_decrypt(s->din, s->dout, &s->dec_key);
        } else {
            AES_encrypt(s->din, s->dout, &s->enc_key);
        }
        break;

    case AES_MODE_CBC:
        if (decrypt) {
            uint8_t tmp[16];
            AES_decrypt(s->din, tmp, &s->dec_key);
            for (int i = 0; i < 16; i++) {
                s->dout[i] = tmp[i] ^ s->iv[i];
            }
            memcpy(s->prev_iv, s->iv, 16);
            memcpy(s->iv, s->din, 16);
        } else {
            for (int i = 0; i < 16; i++) {
                s->din[i] ^= s->iv[i];
            }
            AES_encrypt(s->din, s->dout, &s->enc_key);
            memcpy(s->prev_iv, s->iv, 16);
            memcpy(s->iv, s->dout, 16);
        }
        break;

    case AES_MODE_CTR: {
        uint8_t keystream[16];
        AES_encrypt(s->ctr, keystream, &s->enc_key);
        for (int i = 0; i < 16; i++) {
            s->dout[i] = s->din[i] ^ keystream[i];
        }
        memcpy(s->prev_iv, s->ctr, 16);
        npcm_aes_increment_ctr(s->ctr);
        break;
    }

    case AES_MODE_MAC:
        /* MAC mode: CBC encrypt, discard output except last */
        for (int i = 0; i < 16; i++) {
            s->din[i] ^= s->iv[i];
        }
        AES_encrypt(s->din, s->dout, &s->enc_key);
        memcpy(s->iv, s->dout, 16);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "npcm8xx-aes: unsupported mode %u\n", mode);
        memset(s->dout, 0, 16);
        break;
    }

    if (s->dout_count < NPCM_AES_DOUT_DEPTH) {
        memcpy(s->dout_q[(s->dout_head + s->dout_count) % NPCM_AES_DOUT_DEPTH],
               s->dout, 16);
        s->dout_count++;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "npcm8xx-aes: DOUT FIFO overflow\n");
    }
    s->din_idx = 0;
}

static uint64_t npcm_aes_read(void *opaque, hwaddr addr, unsigned size)
{
    Npcm8xxAesState *s = opaque;
    uint32_t offset;

    switch (addr) {
    case NPCM_AES_BUSY:
        /* Read is the busy status; blocks are processed instantly. */
        return 0;

    case NPCM_AES_CONTROL:
        return s->control;

    case NPCM_AES_VERSION:
        return NPCM_AES_VERSION_VALUE;

    case NPCM_AES_HW_FLAGS:
        return NPCM_AES_HW_FLAGS_VALUE;

    case NPCM_AES_SW_RESET:
        return 0;

    case NPCM_AES_DFA_ERR:
        return 0;

    case NPCM_AES_RBG_SEED_RDY:
        return 1;

    case NPCM_AES_SK:
        return 0;

    case NPCM_AES_FIFO_STATUS: {
        uint32_t st = 0;
        if (s->din_idx >= 16) {
            st |= NPCM_AES_FIFO_DIN_FULL;
        }
        if (s->din_idx == 0) {
            st |= NPCM_AES_FIFO_DIN_EMPTY;
        }
        if (s->dout_count > 0) {
            st |= NPCM_AES_FIFO_DOUT_FULL;
        } else {
            st |= NPCM_AES_FIFO_DOUT_EMPTY;
        }
        return st;
    }

    case NPCM_AES_FIFO_DATA:
        if (s->dout_count > 0) {
            uint32_t val = ldl_le_p(&s->dout_q[s->dout_head][s->dout_idx]);
            s->dout_idx += 4;
            if (s->dout_idx >= 16) {
                s->dout_idx = 0;
                s->dout_head = (s->dout_head + 1) % NPCM_AES_DOUT_DEPTH;
                s->dout_count--;
            }
            return val;
        }
        return 0;

    case NPCM_AES_KEY_0 ... (NPCM_AES_KEY_0 + NPCM_AES_KEY_SIZE - 4):
        offset = addr - NPCM_AES_KEY_0;
        return ldl_le_p(&s->key[offset]);

    case NPCM_AES_IV_0 ... (NPCM_AES_IV_0 + NPCM_AES_IV_SIZE - 4):
        offset = addr - NPCM_AES_IV_0;
        return ldl_le_p(&s->iv[offset]);

    case NPCM_AES_CTR_0 ... (NPCM_AES_CTR_0 + NPCM_AES_CTR_SIZE - 4):
        offset = addr - NPCM_AES_CTR_0;
        return ldl_le_p(&s->ctr[offset]);

    case NPCM_AES_PREV_IV_0 ... (NPCM_AES_PREV_IV_0 + 0x0C):
        offset = addr - NPCM_AES_PREV_IV_0;
        return ldl_le_p(&s->prev_iv[offset]);

    default:
        qemu_log_mask(LOG_UNIMP,
                      "npcm8xx-aes: read from unimplemented 0x%" HWADDR_PRIx
                      "\n", addr);
        return 0;
    }
}

static void npcm_aes_write(void *opaque, hwaddr addr,
                            uint64_t val64, unsigned size)
{
    Npcm8xxAesState *s = opaque;
    uint32_t val = val64;
    uint32_t offset;

    switch (addr) {
    case NPCM_AES_BUSY:
        s->busy = val & 1;
        if (s->busy) {
            npcm_aes_prepare_keys(s);
        }
        break;

    case NPCM_AES_CONTROL:
        s->control = val;
        s->keys_valid = false;
        break;

    case NPCM_AES_SW_RESET:
        if (val & 1) {
            s->control = 0;
            s->busy = 0;
            s->din_idx = 0;
            s->dout_idx = 0;
            s->dout_head = 0;
            s->dout_count = 0;
            s->keys_valid = false;
            memset(s->key, 0, sizeof(s->key));
            memset(s->iv, 0, sizeof(s->iv));
            memset(s->ctr, 0, sizeof(s->ctr));
            memset(s->prev_iv, 0, sizeof(s->prev_iv));
            memset(s->din, 0, sizeof(s->din));
            memset(s->dout, 0, sizeof(s->dout));
        }
        break;

    case NPCM_AES_SK:
        /* OTP key load — not supported in emulation */
        break;

    case NPCM_AES_KEY_0 ... (NPCM_AES_KEY_0 + NPCM_AES_KEY_SIZE - 4):
        offset = addr - NPCM_AES_KEY_0;
        stl_le_p(&s->key[offset], val);
        s->keys_valid = false;
        break;

    case NPCM_AES_IV_0 ... (NPCM_AES_IV_0 + NPCM_AES_IV_SIZE - 4):
        offset = addr - NPCM_AES_IV_0;
        stl_le_p(&s->iv[offset], val);
        break;

    case NPCM_AES_CTR_0 ... (NPCM_AES_CTR_0 + NPCM_AES_CTR_SIZE - 4):
        offset = addr - NPCM_AES_CTR_0;
        stl_le_p(&s->ctr[offset], val);
        break;

    case NPCM_AES_FIFO_DATA:
        if (s->din_idx < 16) {
            stl_le_p(&s->din[s->din_idx], val);
            s->din_idx += 4;
            if (s->din_idx >= 16) {
                npcm_aes_process_block(s);
            }
        }
        break;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "npcm8xx-aes: write to unimplemented 0x%" HWADDR_PRIx
                      " = 0x%x\n", addr, val);
        break;
    }
}

static void npcm_aes_reset_hold(Object *obj, ResetType type)
{
    Npcm8xxAesState *s = NPCM8XX_AES(obj);

    s->control = 0;
    s->busy = 0;
    s->din_idx = 0;
    s->dout_idx = 0;
    s->dout_head = 0;
    s->dout_count = 0;
    s->keys_valid = false;

    memset(s->key, 0, sizeof(s->key));
    memset(s->iv, 0, sizeof(s->iv));
    memset(s->ctr, 0, sizeof(s->ctr));
    memset(s->prev_iv, 0, sizeof(s->prev_iv));
    memset(s->din, 0, sizeof(s->din));
    memset(s->dout, 0, sizeof(s->dout));
    memset(&s->enc_key, 0, sizeof(s->enc_key));
    memset(&s->dec_key, 0, sizeof(s->dec_key));
}

static const MemoryRegionOps npcm_aes_ops = {
    .read = npcm_aes_read,
    .write = npcm_aes_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static int npcm_aes_post_load(void *opaque, int version_id)
{
    Npcm8xxAesState *s = opaque;

    /*
     * The expanded key schedules are not migrated, only the raw key and
     * control words.  Rebuild them on first use rather than trusting the
     * migrated keys_valid flag.
     */
    s->keys_valid = false;
    return 0;
}

static const VMStateDescription vmstate_npcm8xx_aes = {
    .name = TYPE_NPCM8XX_AES,
    .version_id = 3,
    .minimum_version_id = 3,
    .post_load = npcm_aes_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(control, Npcm8xxAesState),
        VMSTATE_UINT32(busy, Npcm8xxAesState),
        VMSTATE_UINT8_ARRAY(key, Npcm8xxAesState, 32),
        VMSTATE_UINT8_ARRAY(iv, Npcm8xxAesState, 16),
        VMSTATE_UINT8_ARRAY(ctr, Npcm8xxAesState, 16),
        VMSTATE_UINT8_ARRAY(prev_iv, Npcm8xxAesState, 16),
        VMSTATE_UINT8_ARRAY(din, Npcm8xxAesState, 16),
        VMSTATE_UINT32(din_idx, Npcm8xxAesState),
        VMSTATE_UINT8_ARRAY(dout, Npcm8xxAesState, 16),
        VMSTATE_UINT8_2DARRAY(dout_q, Npcm8xxAesState,
                              NPCM_AES_DOUT_DEPTH, 16),
        VMSTATE_UINT32(dout_head, Npcm8xxAesState),
        VMSTATE_UINT32(dout_count, Npcm8xxAesState),
        VMSTATE_UINT32(dout_idx, Npcm8xxAesState),
        VMSTATE_BOOL(keys_valid, Npcm8xxAesState),
        VMSTATE_END_OF_LIST()
    }
};

static void npcm_aes_init(Object *obj)
{
    Npcm8xxAesState *s = NPCM8XX_AES(obj);

    memory_region_init_io(&s->mmio, obj, &npcm_aes_ops, s,
                          TYPE_NPCM8XX_AES, NPCM_AES_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static void npcm_aes_class_init(ObjectClass *klass, const void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    rc->phases.hold = npcm_aes_reset_hold;
    dc->vmsd = &vmstate_npcm8xx_aes;
    dc->desc = "NPCM8xx AES crypto accelerator";
}

static const TypeInfo npcm8xx_aes_info = {
    .name          = TYPE_NPCM8XX_AES,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Npcm8xxAesState),
    .instance_init = npcm_aes_init,
    .class_init    = npcm_aes_class_init,
};

static void npcm8xx_aes_register_types(void)
{
    type_register_static(&npcm8xx_aes_info);
}

type_init(npcm8xx_aes_register_types)
