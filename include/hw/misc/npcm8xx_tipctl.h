/*
 * Nuvoton NPCM8xx TIP Controller
 *
 * Copyright 2025 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NPCM8XX_TIPCTL_H
#define NPCM8XX_TIPCTL_H

#include "hw/sysbus.h"
#include "qom/object.h"


typedef struct NPCM8xxTIPCTLState {
    SysBusDevice parent;

    MemoryRegion iomem;
} NPCM8xxTIPCTLState;

#define TYPE_NPCM8XX_TIPCTL "npcm8xx-tipctl"
#define NPCM8XX_TIPCTL(obj) OBJECT_CHECK(NPCM8xxTIPCTLState, (obj), TYPE_NPCM8XX_TIPCTL)

#endif /* NPCM8XX_TIPCTL_H */
