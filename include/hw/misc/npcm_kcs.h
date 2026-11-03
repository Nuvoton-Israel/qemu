/*
 * NPCM BMC-side KCS channels.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NPCM_KCS_H
#define NPCM_KCS_H

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "qemu/bitops.h"
#include "qemu/timer.h"
#include <stdint.h>

#define NPCM_KCS_NR_CHANNELS 3

#define NPCM_KCS_SHM_MAGIC   0x4e4b4353U
#define NPCM_KCS_SHM_VERSION 1

/* Host-to-IDMAQ doorbell: input submission in [2:0], output ack in [5:3]. */
#define NPCM_KCS_DB_INPUT(channel) BIT(channel)
#define NPCM_KCS_DB_OUTPUT_ACK(channel) BIT((channel) + NPCM_KCS_NR_CHANNELS)

typedef struct NPCMKCSSharedChannel {
    uint32_t host_input_seq;
    uint32_t bmc_input_ack_seq;
    uint32_t bmc_output_seq;
    uint32_t host_output_ack_seq;
    uint32_t bmc_output_ack_seq;
    uint8_t host_input;
    uint8_t host_command;
    uint8_t bmc_output;
    uint8_t status;
    uint8_t host_irq;
    uint8_t sci_level;
    uint8_t smi_level;
    uint8_t reserved;
} NPCMKCSSharedChannel;

typedef struct NPCMKCSSharedMemory {
    uint32_t magic;
    uint32_t version;
    uint32_t channel_count;
    uint32_t reserved;
    NPCMKCSSharedChannel channels[NPCM_KCS_NR_CHANNELS];
} NPCMKCSSharedMemory;

typedef struct NPCMKCSChannel {
    uint8_t status;
    uint8_t input;
    uint8_t output;
    uint8_t control;
    uint8_t interrupt_control;
    uint8_t interrupt_enable;
    bool obf_sci;
    bool obf_smi;
    bool ibf_sci;
} NPCMKCSChannel;

typedef struct NPCMKCSSignal {
    struct NPCMKCSState *controller;
    unsigned channel;
    bool smi;
    bool pulse;
    QEMUTimer *timer;
    qemu_irq output;
} NPCMKCSSignal;

struct NPCMKCSState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    Clock *clock;
    NPCMKCSChannel channels[NPCM_KCS_NR_CHANNELS];
    NPCMKCSSignal sci[NPCM_KCS_NR_CHANNELS];
    NPCMKCSSignal smi[NPCM_KCS_NR_CHANNELS];
    NPCMKCSSharedMemory *shared;
    char *shm_path;
    char *doorbell_in_path;
    char *doorbell_out_path;
    int doorbell_in_fd;
    int doorbell_out_fd;
    Error *migration_blocker;
};

#define TYPE_NPCM_KCS "npcm-kcs"
OBJECT_DECLARE_SIMPLE_TYPE(NPCMKCSState, NPCM_KCS)

#endif /* NPCM_KCS_H */
