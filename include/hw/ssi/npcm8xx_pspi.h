/*
 * Nuvoton NPCM8xx Peripheral SPI Module (PSPI)
 *
 * Copyright 2025 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NPCM8XX_PSPI_H
#define NPCM8XX_PSPI_H

#include "hw/ssi/ssi.h"
#include "hw/core/sysbus.h"

/*
 * Number of registers in our device state structure. Don't change this without
 * incrementing the version_id in the vmstate.
 */
#define NPCM8XX_PSPI_NR_REGS 3

/**
 * NPCM8XXPSPIState - Device state for one Flash Interface Unit.
 * @parent: System bus device.
 * @mmio: Memory region for register access.
 * @spi: The SPI bus mastered by this controller.
 * @regs: Register contents.
 * @irq: The interrupt request queue for this module.
 *
 * Each PSPI has a shared bank of registers, and controls up to four chip
 * selects. Each chip select has a dedicated memory region which may be used to
 * read and write the flash connected to that chip select as if it were memory.
 */
typedef struct NPCM8XXPSPIState {
    SysBusDevice parent;

    MemoryRegion mmio;

    SSIBus *spi;
    uint16_t regs[NPCM8XX_PSPI_NR_REGS];
    qemu_irq irq;
} NPCM8XXPSPIState;

#define TYPE_NPCM8XX_PSPI "npcm8xx-pspi"
OBJECT_DECLARE_SIMPLE_TYPE(NPCM8XXPSPIState, NPCM8XX_PSPI)

#endif /* NPCM8XX_PSPI_H */
