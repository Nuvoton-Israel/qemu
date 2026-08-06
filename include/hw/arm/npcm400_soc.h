/*
 * Nuvoton NPCM400 SoC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_NPCM400_SOC_H
#define HW_ARM_NPCM400_SOC_H

#include "hw/arm/armv7m.h"
#include "hw/char/npcm4xx_uart.h"
#include "hw/core/clock.h"
#include "hw/misc/npcm400_pdma.h"
#include "hw/ssi/npcm400_fiu.h"
#include "hw/ssi/npcm400_spim.h"
#include "qom/object.h"

#define TYPE_NPCM400_SOC "npcm400-soc"
OBJECT_DECLARE_SIMPLE_TYPE(NPCM400State, NPCM400_SOC)

#define NPCM400_NUM_UARTS 2
#define NPCM400_NUM_STUB_MMIO 64

#define NPCM400_FLASH_BASE 0x00080000
#define NPCM400_FLASH_SIZE (1024 * 1024)
#define NPCM400_SRAM_BASE  0x10008000
#define NPCM400_SRAM_SIZE  (764 * 1024)

struct NPCM400State {
    SysBusDevice parent_obj;

    ARMv7MState armv7m;
    NPCM4xxUARTState uart[NPCM400_NUM_UARTS];

    NPCM400PDMAState pdma;
    NPCM400FIUState  fiu;
    NPCM400SPIMState spim;

    /*
     * flash_alias: alias of the SPIM DMM window placed at 0x00000000 so
     * that the Cortex-M reset fetch (MSP @0x0, PC @0x4) reaches flash.
     */
    MemoryRegion flash_alias;
    MemoryRegion sram;
    MemoryRegion reset_status;
    MemoryRegion stub_mmio[NPCM400_NUM_STUB_MMIO];
    unsigned int stub_mmio_count;

    Clock *sysclk;
};

#endif