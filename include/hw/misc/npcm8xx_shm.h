/*
 * Nuvoton NPCM8xx Shared Memory (SHM) module emulation.
 *
 * The SHM module maps up to four windows of the BMC internal RAM3 SRAM into
 * the host address space over eSPI/LPC.  The Linux npcm-espi-mmbi driver
 * uses it for two things only:
 *
 *   1. Window setup at probe time: it programs the window size code and the
 *      four window base addresses so the host can see the MMBI buffers.
 *   2. Host-access notification: SMC_STS.SHM_ACC is set by hardware when the
 *      host touches a mapped window; with SMC_CTL.SHM_ACC_IE set this raises
 *      an interrupt, which the driver uses to kick off its polling work.
 *
 * Register layout (offsets from the block base at 0xc0001000):
 *
 *   +0x00  SMC_STS    SHM Core Status
 *            bit 6   SHM_ACC   Host accessed a mapped window   R/W1C
 *   +0x01  SMC_CTL    SHM Core Control
 *            bit 5   SHM_ACC_IE  Enable interrupt on SHM_ACC   R/W
 *   +0x07  WIN_SIZE   Windows 1/2 size code
 *            bits 7-4  window 2 size = 2^code bytes
 *            bits 3-0  window 1 size = 2^code bytes
 *   +0x20  WIN_BASE1  Window 1 base address (32-bit)
 *   +0x24  WIN_BASE2  Window 2 base address (32-bit)
 *   +0x87  WINE_SIZE  Windows 3/4 size code (same encoding as WIN_SIZE)
 *   +0xA0  WIN_BASE3  Window 3 base address (32-bit)
 *   +0xA4  WIN_BASE4  Window 4 base address (32-bit)
 *
 * Everything else in the 4 KiB block behaves as plain read/write storage so
 * that unmodelled registers do not abort the guest.
 *
 * QEMU-only extension
 * -------------------
 * There is no eSPI host model, so nothing can ever set SHM_ACC on its own.
 * Offset SHM_SIM_ACC (0xF00) is a QEMU-private "simulate host access"
 * trigger: writing a non-zero byte sets SMC_STS.SHM_ACC exactly as the
 * hardware would, which lets a test raise the MMBI interrupt from the guest:
 *
 *     busybox devmem 0xc0001f00 8 1
 *
 * This offset is not a real NPCM8xx register and is never touched by the
 * kernel driver.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef NPCM8XX_SHM_H
#define NPCM8XX_SHM_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

/* Size of the SHM register block. */
#define NPCM8XX_SHM_REGS_SIZE   0x1000

#define TYPE_NPCM8XX_SHM  "npcm8xx-shm"
OBJECT_DECLARE_SIMPLE_TYPE(NPCM8xxSHMState, NPCM8XX_SHM)

struct NPCM8xxSHMState {
    SysBusDevice  parent_obj;

    MemoryRegion  iomem;

    /* Byte-addressable register file; see the layout comment above. */
    uint8_t       regs[NPCM8XX_SHM_REGS_SIZE];

    /* Interrupt output, wired to GIC SPI 11 in npcm8xx.c. */
    qemu_irq      irq;
};

#endif /* NPCM8XX_SHM_H */
