/*
 * Nuvoton NPCM8xx AES Engine
 *
 * Copyright 2026 Dell Technologies
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_NPCM8XX_AES_H
#define HW_NPCM8XX_AES_H

#include "hw/core/sysbus.h"
#include "crypto/aes.h"
#include "qom/object.h"

#define TYPE_NPCM8XX_AES "npcm8xx-aes"
OBJECT_DECLARE_SIMPLE_TYPE(Npcm8xxAesState, NPCM8XX_AES)

/* Register offsets from AES base */
#define NPCM_AES_KEY_0          0x400
#define NPCM_AES_KEY_SIZE       0x20    /* 8 x 32-bit = 256 bits max */
#define NPCM_AES_IV_0           0x440
#define NPCM_AES_IV_SIZE        0x10    /* 4 x 32-bit = 128 bits */
#define NPCM_AES_CTR_0          0x460
#define NPCM_AES_CTR_SIZE       0x10
#define NPCM_AES_BUSY           0x470
#define NPCM_AES_SK             0x478
#define NPCM_AES_PREV_IV_0      0x490
#define NPCM_AES_DIN_DOUT       0x4A0
#define NPCM_AES_CONTROL        0x4C0
#define NPCM_AES_VERSION        0x4C4
#define NPCM_AES_HW_FLAGS       0x4C8
#define NPCM_AES_SW_RESET       0x4F4
#define NPCM_AES_DFA_ERR        0x4F8
#define NPCM_AES_RBG_SEED_RDY   0x4FC
#define NPCM_AES_FIFO_DATA      0x500
#define NPCM_AES_FIFO_STATUS    0x600

/* AES_CONTROL fields */
#define NPCM_AES_CTRL_DEC_ENC_SHIFT     0
#define NPCM_AES_CTRL_MODE_SHIFT        2
#define NPCM_AES_CTRL_MODE_MASK         0x07
#define NPCM_AES_CTRL_NK_SHIFT          12
#define NPCM_AES_CTRL_NK_MASK           0x03

/* AES_FIFO_STATUS bits */
#define NPCM_AES_FIFO_DIN_FULL         BIT(0)
#define NPCM_AES_FIFO_DIN_EMPTY        BIT(1)
#define NPCM_AES_FIFO_DOUT_FULL        BIT(2)
#define NPCM_AES_FIFO_DOUT_EMPTY       BIT(3)

/* AES_HW_FLAGS: 256/192 keys + CTR mode + prev_iv */
#define NPCM_AES_HW_FLAGS_VALUE        0x0209

/* AES_VERSION: major=3 minor=0 fixes=0x48 */
#define NPCM_AES_VERSION_VALUE         0x3048

#define NPCM_AES_MMIO_SIZE             0x1000

/*
 * The CPU path keeps up to 3 blocks in flight, but a GDMA request pushes the
 * whole buffer (at most one page) before the output side is drained.
 */
#define NPCM_AES_DOUT_DEPTH            512

struct Npcm8xxAesState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;

    /* Configuration (written in config mode, BUSY=0) */
    uint32_t control;
    uint32_t busy;
    uint8_t  key[32];
    uint8_t  iv[16];
    uint8_t  ctr[16];
    uint8_t  prev_iv[16];

    /* FIFO state */
    uint8_t  din[16];
    uint32_t din_idx;           /* bytes written to din so far */
    uint8_t  dout[16];          /* scratch: block just produced */
    uint8_t  dout_q[NPCM_AES_DOUT_DEPTH][16]; /* DOUT FIFO, in order */
    uint32_t dout_head;
    uint32_t dout_count;        /* complete blocks queued */
    uint32_t dout_idx;          /* bytes read from head block so far */

    /* Prepared key schedules */
    AES_KEY  enc_key;
    AES_KEY  dec_key;
    bool     keys_valid;
};

#endif
