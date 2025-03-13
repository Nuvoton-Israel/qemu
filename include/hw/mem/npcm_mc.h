/*
 * Nuvoton NPCM7xx/8xx Memory Controller stub
 *
 * Copyright 2020 Google LLC
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
#ifndef NPCM_MC_H
#define NPCM_MC_H

#include "exec/memory.h"
#include "hw/sysbus.h"
#include "qom/object.h"

/**
 * struct NPCMMCState - Device state for the memory controller.
 * @parent: System bus device.
 * @mmio: Memory region through which registers are accessed.
 */
typedef struct NPCMMCState {
    SysBusDevice parent;

    MemoryRegion mmio;
} NPCMMCState;

struct NPCMMCClass {
    /*< private >*/
    SysBusDeviceClass parent;
    /*< public >*/
};

#define TYPE_NPCM_MC "npcm-mc"
#define TYPE_NPCM7XX_MC "npcm7xx-mc"
#define TYPE_NPCM8XX_MC "npcm8xx-mc"
OBJECT_DECLARE_TYPE(NPCMMCState, NPCMMCClass, NPCM_MC)

#endif /* NPCM_MC_H */
