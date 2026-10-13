/*
 * Nuvoton NPCM8xx GDMA controller (functional model)
 *
 * Copyright 2026 Dell Technologies
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_NPCM8XX_GDMA_H
#define HW_NPCM8XX_GDMA_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_NPCM8XX_GDMA "npcm8xx-gdma"
OBJECT_DECLARE_SIMPLE_TYPE(Npcm8xxGdmaState, NPCM8XX_GDMA)

#define NPCM_GDMA_MMIO_SIZE     0x1000
#define NPCM_GDMA_NR_CHANNELS   8
#define NPCM_GDMA_CH_STRIDE     0x20

/*
 * Upper bound on TCNT.  A transfer runs synchronously inside the MMIO write
 * and holds the BQL, so a guest-programmed count must not be unbounded.
 */
#define NPCM_GDMA_MAX_TCNT      (1u << 20)

/* Per-channel register offsets */
enum {
    NPCM_GDMA_CTL   = 0,
    NPCM_GDMA_SRCB  = 1,
    NPCM_GDMA_DSTB  = 2,
    NPCM_GDMA_TCNT  = 3,
    NPCM_GDMA_CSRC  = 4,
    NPCM_GDMA_CDST  = 5,
    NPCM_GDMA_CTCNT = 6,
    NPCM_GDMA_NR_REGS = 8,
};

#define NPCM_GDMA_CTL_EN        BIT(0)
#define NPCM_GDMA_CTL_DAFIX     BIT(6)
#define NPCM_GDMA_CTL_SAFIX     BIT(7)
#define NPCM_GDMA_CTL_BURST     BIT(9)
#define NPCM_GDMA_CTL_TC        BIT(18)

struct Npcm8xxGdmaState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    uint32_t reg[NPCM_GDMA_NR_CHANNELS][NPCM_GDMA_NR_REGS];
};

#endif /* HW_NPCM8XX_GDMA_H */
