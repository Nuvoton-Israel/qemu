/*
 * Nuvoton NPCM8xx Video Capture and Differentiation (VCD) engine
 *
 * Copyright 2026 Dell Technologies
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_NPCM8XX_VCD_H
#define HW_NPCM8XX_VCD_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_NPCM8XX_VCD "npcm8xx-vcd"
OBJECT_DECLARE_SIMPLE_TYPE(Npcm8xxVcdState, NPCM8XX_VCD)

#define NPCM_VCD_MMIO_SIZE      0x10000

/* 0x0000..0x7fff is the difference table; registers start at 0x8000 */
#define NPCM_VCD_FBA_ADR        0x8000
#define NPCM_VCD_FBB_ADR        0x8004
#define NPCM_VCD_FB_LP          0x8008
#define NPCM_VCD_CAP_RES        0x800c
#define NPCM_VCD_MODE           0x8014
#define NPCM_VCD_CMD            0x8018
#define NPCM_VCD_STAT           0x801c
#define NPCM_VCD_INTE           0x8020

/* Writable bits: line pitch in 512-byte units, resolution 11 bits per axis */
#define NPCM_VCD_FB_LP_WMASK    0xfe00fe00
#define NPCM_VCD_CAP_RES_WMASK  0x07ff07ff
#define NPCM_VCD_CAPRES_V_MASK  0x7ff

#define NPCM_VCD_CMD_GO         BIT(0)
#define NPCM_VCD_CMD_RST        BIT(1)

#define NPCM_VCD_STAT_DONE      BIT(0)
#define NPCM_VCD_STAT_BUSY      BIT(30)
#define NPCM_VCD_STAT_IRQ       BIT(31)
#define NPCM_VCD_STAT_W1C       0x3fff
#define NPCM_VCD_STAT_LINE_SHIFT 16
#define NPCM_VCD_STAT_LINE_MASK 0x07ff0000

struct Npcm8xxVcdState {
    SysBusDevice parent;

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t regs[NPCM_VCD_MMIO_SIZE / sizeof(uint32_t)];
    uint32_t stat;
};

#endif /* HW_NPCM8XX_VCD_H */
