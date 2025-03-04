/*
 * Nuvoton NPCM7xx/NPCM8xx System Global Control Registers.
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
#ifndef NPCM8XX_TIPCTL_H
#define NPCM8XX_TIPCTL_H

#include "exec/memory.h"
#include "hw/sysbus.h"
#include "qom/object.h"


typedef struct NPCM8xxTIPCTLState {
    SysBusDevice parent;

    MemoryRegion iomem;
} NPCM8xxTIPCTLState;

#define TYPE_NPCM8XX_TIPCTL "npcm8xx-tipctl"
#define NPCM8XX_TIPCTL(obj) OBJECT_CHECK(NPCM8xxTIPCTLState, (obj), TYPE_NPCM8XX_TIPCTL)

#endif /* NPCM8XX_TIPCTL_H */
