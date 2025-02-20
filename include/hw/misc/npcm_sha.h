/*
 * Nuvoton NPCM7xx SHA Module
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

#ifndef NPCM_SHA_H
#define NPCM_SHA_H

#include "hw/sysbus.h"
#include <nettle/sha.h>

typedef struct NPCM8xxSHAState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint8_t sha_ctr_sts;
    uint8_t sha_cfg;
    uint8_t sha512_ctr_sts;
    uint8_t sha512_cmd;
    struct sha256_ctx sha256ctx;
    struct sha1_ctx sha1ctx;
    // for SHA-512 module
    uint32_t sha512_bytes_index; // used for load sha512 state, and read data
    struct sha512_ctx sha512ctx;
} NPCM8xxSHAState;

#define TYPE_NPCM8XX_SHA "npcm8xx-sha"
#define NPCM8XX_SHA(obj) \
    OBJECT_CHECK(NPCM8xxSHAState, (obj), TYPE_NPCM8XX_SHA)

#endif  /* NPCM_SHA_H */