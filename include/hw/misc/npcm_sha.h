/*
 * Nuvoton NPCM SHA Hardware Accelerator
 *
 * Copyright (c) Nuvoton Technology Corporation.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef NPCM_SHA_H
#define NPCM_SHA_H

#include "hw/sysbus.h"

typedef struct NPCM8xxSHAState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint8_t sha_ctr_sts;
    uint8_t sha_cfg;
    uint8_t sha512_ctr_sts;
    uint8_t sha512_cmd;
    uint32_t sha512_hash_out; /* Assuming SHA-512 or SHA-384 */
    uint32_t sha_hash_out[8]; /* Assuming SHA-256 or SHA-1 */
    uint32_t write_bytes;
    uint8_t buffer[64];
    /* for SHA-512 module */
    uint32_t sha512_bytes_index;
    uint8_t sha512_buffer[128];
    uint64_t sha512_state[8];
} NPCM8xxSHAState;

#define TYPE_NPCM8XX_SHA "npcm8xx-sha"
#define NPCM8XX_SHA(obj) \
    OBJECT_CHECK(NPCM8xxSHAState, (obj), TYPE_NPCM8XX_SHA)

#endif  /* NPCM_SHA_H */
