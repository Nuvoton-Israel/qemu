/*
 * Nuvoton NPCM400 FIU (Flash Interface Unit)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_NPCM400_FIU_H
#define HW_MISC_NPCM400_FIU_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_NPCM400_FIU    "npcm400-fiu"
OBJECT_DECLARE_SIMPLE_TYPE(NPCM400FIUState, NPCM400_FIU)

/*
 * Combined register window covering core (offset 0x000) and host (offset
 * 0x1000) blocks; real hardware has two separate 0x100-byte blocks.
 */
#define NPCM400_FIU_REG_SIZE    0x3000
/* Number of XIP decode windows (private/shared/backup flash) */
#define NPCM400_FIU_NUM_WINS    3

struct NPCM400FIUState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;                   /* SysBus MMIO index 0: register window */
    MemoryRegion win[NPCM400_FIU_NUM_WINS]; /* SysBus MMIO index 1-3: XIP windows */
    uint8_t regs[NPCM400_FIU_REG_SIZE];
};

#endif /* HW_MISC_NPCM400_FIU_H */
