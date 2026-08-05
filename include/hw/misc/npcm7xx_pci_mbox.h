/*
 * Nuvoton NPCM7xx/NPCM8xx PCI Mailbox emulation.
 *
 * Emulates the dual-ported RAM mailbox between the host-side PCIe bus
 * (BIOS / FPGA) and the iDRAC BMC (SMAD daemon).
 *
 * Hardware layout (from NPCM845 datasheet, sections 10.3.5 and 10.3.6):
 *
 *   pcimbox1:
 *     ctrl regs  0xf084c000  (BMBXSTAT/BMBXCTL/BMBXCMD +
 *                             HMBXSTAT/HMBXCTL/HMBXCMD)
 *     RAM        0xf0848000  ~16 KB (dual-ported data buffer)
 *     IRQ        GIC SPI 105
 *   pcimbox2:
 *     ctrl regs  0xf086c000
 *     RAM        0xf0868000  ~16 KB
 *     IRQ        GIC SPI 106
 *
 * Two register sets exist in the ctrl region:
 *
 *   BMC Core Registers (accessed by Linux npcm7xx-pci-mbox driver via AHB):
 *     BMBXSTAT  +0x00  BMC Mailbox Status  (CIF7-0 R/W1C, HORI bit31 R/W1C)
 *     BMBXCTL   +0x04  BMC Mailbox Control (CIE7-0 R/W, CFGLCK bit30 R/W)
 *     BMBXCMD   +0x08  BMC Mailbox Command (HIF7-0 R/W1S, propagates to
 *                                           HMBXSTAT)
 *
 *   Host BAR1 Registers (BAR1+4000h, accessed by BIOS/FPGA via PCIe):
 *     HMBXSTAT  +0x10  Host Mailbox Status  (HIF7-0 R/W1C, CORI bit31 R/W1C)
 *     HMBXCTL   +0x14  Host Mailbox Control (HIE7-0 R/W, HIDECFG bit31 R/W,
 *                                            CFGLCK bit30 RO mirrors
 *                                            BMBXCTL.CFGLCK)
 *     HMBXCMD   +0x18  Host Mailbox Command (CIF7-0 R/W1S, propagates to
 *                                            BMBXSTAT)
 *
 * Cross-communication:
 *   BMC writes BMBXCMD  (HIF bits W1S) -> those bits set in HMBXSTAT ->
 *   host IRQ
 *   Host writes HMBXCMD (CIF bits W1S) -> those bits set in BMBXSTAT ->
 *   BMC IRQ
 *   BMBXCTL.CFGLCK is mirrored as read-only in HMBXCTL.CFGLCK
 *
 * The RAM region is backed by a POSIX shared-memory file so that a BIOS QEMU
 * process (or FPGA simulation) can mmap the same file and write into it
 * directly, with the QEMU device firing the GIC interrupt on demand.
 *
 * Interrupt signalling between BIOS and BMC uses a Unix datagram socket
 * doorbell (separate from the RAM SHM file):
 *
 *   doorbell-in  : UNIX datagram socket path that iDRAC QEMU binds.
 *                  BIOS sendto() one byte (CIF mask) -> QEMU triggers
 *                  HMBXCMD write -> CIF bits set in BMBXSTAT -> BMC IRQ
 *                  fires.
 *   doorbell-out : UNIX datagram socket path that the BIOS process binds.
 *                  When BMC writes BMBXCMD, QEMU sendto() one byte (HIF
 *                  mask) to this address -> BIOS receives the signal.
 *
 * No iDRAC kernel driver or smad changes are required; from the guest's
 * perspective a normal GIC IRQ arrives exactly as if triggered by hardware.
 *
 * Copyright 2026 Dell Technologies
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef NPCM7XX_PCI_MBOX_H
#define NPCM7XX_PCI_MBOX_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

/*
 * Control register block size: 6 registers x 4 B = 24 B;
 * round up to 0x20 (32 B) for natural alignment.
 *   +0x00  BMBXSTAT   BMC Mailbox Status
 *   +0x04  BMBXCTL    BMC Mailbox Control
 *   +0x08  BMBXCMD    BMC Mailbox Command
 *   +0x10  HMBXSTAT   Host Mailbox Status  (BAR1+4000h equivalent)
 *   +0x14  HMBXCTL    Host Mailbox Control (BAR1+4004h equivalent)
 *   +0x18  HMBXCMD    Host Mailbox Command (BAR1+4008h equivalent)
 */
#define NPCM7XX_PCI_MBOX_CTRL_SIZE   0x20

/*
 * Dual-ported RAM size: 0x4000 bytes (16 KiB page-aligned).
 * The NPCM845 DTS declares 0x3f00, but QEMU's
 * memory_region_init_ram_from_fd() requires the backing file to be at
 * least page-aligned (0x1000). Rounding up to 0x4000 avoids the
 * "backing store size too small" error when shm-path is used. The
 * extra 256 bytes are harmless (never accessed by the kernel driver,
 * which only uses offsets 0..0x3eff).
 */
#define NPCM7XX_PCI_MBOX_RAM_SIZE    0x4000

#define TYPE_NPCM7XX_PCI_MBOX  "npcm7xx-pci-mbox"
OBJECT_DECLARE_SIMPLE_TYPE(NPCM7xxPciMboxState, NPCM7XX_PCI_MBOX)

struct NPCM7xxPciMboxState {
    SysBusDevice  parent_obj;

    /* Region 0: control registers (BMBXSTAT/CTL/CMD + HMBXSTAT/CTL/CMD) */
    MemoryRegion  ctrl_mr;
    /* Region 1: dual-ported RAM */
    MemoryRegion  ram_mr;

    /*
     * BMC Core register state (accessed by Linux driver via AHB at
     * ctrl base)
     */
    uint32_t      bmbxstat;   /* +0x00 CIF7-0 (R/W1C), HORI (bit31 R/W1C) */
    uint32_t      bmbxctl;    /* +0x04 CIE7-0 (R/W), CFGLCK (bit30 R/W)   */
    uint32_t      bmbxcmd;    /* +0x08 HIF7-0 (R/W1S), sets HMBXSTAT      */

    /* Host BAR1 register state (BAR1+4000h/4004h/4008h from PCIe host view) */
    uint32_t      hmbxstat;   /* +0x10 HIF7-0 (R/W1C), CORI (bit31 R/W1C) */
    uint32_t      hmbxctl;    /* +0x14 HIE7-0 (R/W), HIDECFG (bit31 R/W)  */
    uint32_t      hmbxcmd;    /* +0x18 CIF7-0 (R/W1S), sets BMBXSTAT      */

    /* BMC interrupt output to GIC (GIC SPI 105 / 106) */
    qemu_irq      irq;
    /*
     * Host interrupt output (PCIe INTx/MSI toward BIOS; typically
     * unconnected in the NPCM845 QEMU machine since there is no PCIe
     * host emulation)
     */
    qemu_irq      host_irq;

    /*
     * Optional path to a POSIX shared-memory file backing the RAM region.
     * When set (e.g. "/dev/shm/npcm_pci_mbox") the RAM is mapped from that
     * file so BIOS QEMU / FPGA can write into it from the host side.
     * When empty the RAM is private anonymous memory (standalone iDRAC run).
     */
    char         *shm_path;

    /*
     * Optional Unix datagram socket paths for BIOS/BMC interrupt
     * signalling.
     *
     * doorbell_in_path  - iDRAC QEMU binds here; BIOS sends CIF byte to
     *                     ring the BMC doorbell (triggers HMBXCMD write,
     *                     causes BMC IRQ).
     * doorbell_out_path - BIOS binds here; iDRAC QEMU sends HIF byte when
     *                     the BMC writes BMBXCMD (BMC to BIOS signal).
     * doorbell_in_fd    - bound server socket fd (-1 if not configured).
     * doorbell_out_fd   - unbound sender socket fd (-1 if not configured).
     */
    char         *doorbell_in_path;
    char         *doorbell_out_path;
    int           doorbell_in_fd;
    int           doorbell_out_fd;
};

#endif /* NPCM7XX_PCI_MBOX_H */
