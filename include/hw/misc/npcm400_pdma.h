/*
 * Nuvoton NPCM400 PDMA (Peripheral DMA Controller)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_NPCM400_PDMA_H
#define HW_MISC_NPCM400_PDMA_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_NPCM400_PDMA   "npcm400-pdma"
OBJECT_DECLARE_SIMPLE_TYPE(NPCM400PDMAState, NPCM400_PDMA)

/* Total size of the PDMA register window */
#define NPCM400_PDMA_REG_SIZE   0x500

struct NPCM400PDMAState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    uint8_t regs[NPCM400_PDMA_REG_SIZE];
};

#endif /* HW_MISC_NPCM400_PDMA_H */
