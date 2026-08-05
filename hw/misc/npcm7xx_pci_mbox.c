/*
 * Nuvoton NPCM7xx/NPCM8xx PCI Mailbox emulation.
 *
 * Emulates the two address regions that the upstream npcm7xx-pci-mbox
 * Linux kernel driver expects:
 *
 *   Region 0  ctrl regs (0x20 B): BMC-side and Host-side register sets
 *   Region 1  dual-ported RAM:    data buffer read/written by SMAD via
 *                                 lseek + read/write on
 *                                 /dev/npcm7xx-pci-mbox0
 *
 * Register layout inside the ctrl region (base = 0xf084c000):
 *
 *   BMC Core Registers (NPCM845 datasheet section 10.3.6):
 *     +0x00  BMBXSTAT  BMC Mailbox Status Register
 *              bit 31     HORI  Host Reset Indication      R/W1C
 *              bits 7-0   CIF7-0 Core Interrupt Flags      R/W1C
 *                         (set by host via HMBXCMD; causes BMC IRQ
 *                         when CIE set)
 *     +0x04  BMBXCTL   BMC Mailbox Control Register
 *              bit 30     CFGLCK PCI Config Lock           R/W
 *                         (mirrored RO in HMBXCTL)
 *              bits 7-0   CIE7-0 Core Interrupt Enable     R/W
 *     +0x08  BMBXCMD   BMC Mailbox Command Register
 *              bits 7-0   HIF7-0 Host Interrupt Flags      R/W1S
 *                         (writing propagates to HMBXSTAT; causes
 *                         host IRQ when HIE set)
 *
 *   Host BAR1 Registers (NPCM845 datasheet section 10.3.5,
 *   BAR1+4000h/4004h/4008h):
 *     +0x10  HMBXSTAT  Host Mailbox Status Register
 *              bit 31     CORI  Core Reset Indication      R/W1C  reset=1
 *              bits 7-0   HIF7-0 Host Interrupt Flags      R/W1C
 *                         (set when BMC writes BMBXCMD; causes host
 *                         IRQ when HIE set)
 *     +0x14  HMBXCTL   Host Mailbox Control Register
 *              bit 31     HIDECFG Hide Config Space        R/W
 *              bit 30     CFGLCK PCI Config Lock           RO
 *                         (mirrors BMBXCTL.CFGLCK)
 *              bits 7-0   HIE7-0 Host Interrupt Enable     R/W
 *     +0x18  HMBXCMD   Host Mailbox Command Register
 *              bits 7-0   CIF7-0 Core Interrupt Flags      R/W1S
 *                         (writing propagates to BMBXSTAT; causes
 *                         BMC IRQ when CIE set)
 *
 * Cross-communication / interrupt protocol:
 *
 *   Host to BMC signalling:
 *     Host writes CIF bits to HMBXCMD (W1S)
 *       -> those bits are ORed into BMBXSTAT.CIF
 *       -> BMC IRQ asserted if any (BMBXSTAT.CIF & BMBXCTL.CIE) bit
 *          is set
 *     BMC kernel driver ISR reads BMBXSTAT, processes data, clears
 *     CIF (W1C)
 *
 *   BMC to Host signalling:
 *     BMC writes HIF bits to BMBXCMD (W1S)
 *       -> those bits are ORed into HMBXSTAT.HIF and stored in
 *          BMBXCMD
 *       -> Host IRQ asserted if any (HMBXSTAT.HIF & HMBXCTL.HIE) bit
 *          is set
 *     Host reads HMBXSTAT, processes data, clears HIF (W1C)
 *
 *   CFGLCK mirroring:
 *     BMC writes BMBXCTL.CFGLCK -> visible to host as read-only
 *     HMBXCTL.CFGLCK
 *
 * The RAM region can be backed by a POSIX shared-memory file so that a BIOS
 * QEMU process (or FPGA simulation) can mmap the same file and write into it
 * directly.  When the shm-path property is unset, private anonymous RAM is
 * used (standalone iDRAC boot, normal operation).
 *
 * Copyright 2026 Dell Technologies
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/npcm7xx_pci_mbox.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include <sys/un.h>

/* Control register offsets */

/* BMC Core Registers (section 10.3.6) */
#define BMBXSTAT_REG    0x00  /* BMC Mailbox Status Register  */
#define BMBXCTL_REG     0x04  /* BMC Mailbox Control Register */
#define BMBXCMD_REG     0x08  /* BMC Mailbox Command Register */

/* Host BAR1 Registers (section 10.3.5, BAR1+4000h/4004h/4008h) */
#define HMBXSTAT_REG    0x10  /* Host Mailbox Status Register  */
#define HMBXCTL_REG     0x14  /* Host Mailbox Control Register */
#define HMBXCMD_REG     0x18  /* Host Mailbox Command Register */

/* BMBXSTAT bit definitions */
#define BMBXSTAT_HORI       BIT(31)   /* Host Reset Indication        R/W1C */
#define BMBXSTAT_CIF_MASK   0xFFU     /* Core Interrupt Flags [7:0]   R/W1C */
/* Writeable mask: HORI + CIF7-0 */
#define BMBXSTAT_W_MASK     (BMBXSTAT_HORI | BMBXSTAT_CIF_MASK)

/* BMBXCTL bit definitions */
#define BMBXCTL_CFGLCK      BIT(30)   /* PCI Config Lock              R/W   */
#define BMBXCTL_CIE_MASK    0xFFU     /* Core Interrupt Enable [7:0]  R/W   */
/* Writeable mask */
#define BMBXCTL_W_MASK      (BMBXCTL_CFGLCK | BMBXCTL_CIE_MASK)

/* BMBXCMD bit definitions */
#define BMBXCMD_HIF_MASK    0xFFU     /* Host Interrupt Flags [7:0]   R/W1S */

/* HMBXSTAT bit definitions */
#define HMBXSTAT_CORI       BIT(31)   /* Core Reset Indication        R/W1C */
#define HMBXSTAT_HIF_MASK   0xFFU     /* Host Interrupt Flags [7:0]   R/W1C */
/* Writeable mask: CORI + HIF7-0 */
#define HMBXSTAT_W_MASK     (HMBXSTAT_CORI | HMBXSTAT_HIF_MASK)

/* HMBXCTL bit definitions */
#define HMBXCTL_HIDECFG     BIT(31)   /* Hide Config Space            R/W   */
/* PCI Config Lock, RO (mirrors BMBXCTL) */
#define HMBXCTL_CFGLCK      BIT(30)
#define HMBXCTL_HIE_MASK    0xFFU     /* Host Interrupt Enable [7:0]  R/W   */
/* Writeable mask (CFGLCK is RO so excluded) */
#define HMBXCTL_W_MASK      (HMBXCTL_HIDECFG | HMBXCTL_HIE_MASK)

/* HMBXCMD bit definitions */
#define HMBXCMD_CIF_MASK    0xFFU     /* Core Interrupt Flags [7:0]   R/W1S */

/* Forward declaration needed by doorbell_read (defined after ctrl_write). */
static void npcm7xx_pci_mbox_ctrl_write(void *opaque, hwaddr offset,
                                        uint64_t value, unsigned size);

/* Doorbell (Unix datagram socket) helpers */

/*
 * npcm7xx_pci_mbox_doorbell_read - QEMU I/O handler for the incoming
 * doorbell.
 *
 * Called by QEMU's main loop when the BIOS process sends a datagram to the
 * socket bound at doorbell-in.  Each byte of the datagram is interpreted as
 * a CIF bit-mask and forwarded to HMBXCMD, exactly as if the PCIe host had
 * written to the HMBXCMD register directly.  This propagates to BMBXSTAT and
 * fires the BMC IRQ when the corresponding CIE bit is set, without any
 * change to the iDRAC kernel driver or smad.
 */
static void npcm7xx_pci_mbox_doorbell_read(void *opaque)
{
    NPCM7xxPciMboxState *s = opaque;
    uint8_t cif;
    ssize_t n;

    /* Drain all pending datagrams in one shot. */
    while ((n = recv(s->doorbell_in_fd, &cif, sizeof(cif),
                     MSG_DONTWAIT)) > 0) {
        /*
         * The BIOS encodes the desired CIF mask in the datagram byte.
         * Pass it through the normal HMBXCMD write path so all W1S,
         * cross-propagation, and IRQ logic applies.
         */
        warn_report("npcm7xx-pci-mbox: doorbell-in datagram received "
                    "CIF=0x%02x bmbxstat_before=0x%08x bmbxctl_cie=0x%02x",
                    cif, s->bmbxstat, s->bmbxctl & BMBXCTL_CIE_MASK);
        npcm7xx_pci_mbox_ctrl_write(s, HMBXCMD_REG, cif, 4);
        warn_report("npcm7xx-pci-mbox: doorbell-in done: "
                    "hmbxcmd=0x%08x bmbxstat=0x%08x irq_level=%d",
                    s->hmbxcmd, s->bmbxstat,
                    !!((s->bmbxstat & BMBXSTAT_CIF_MASK) &
                       (s->bmbxctl  & BMBXCTL_CIE_MASK)));
    }
}

/*
 * npcm7xx_pci_mbox_doorbell_kick - notify the BIOS that the BMC has
 * responded.
 *
 * Called whenever the BMC writes HIF bits to BMBXCMD.  Sends a single byte
 * (the HIF mask) as a datagram to the UNIX socket the BIOS process has
 * bound at doorbell-out.  If the BIOS is not listening the sendto() fails
 * silently (ENOENT / ECONNREFUSED), this is expected during early boot.
 */
static void npcm7xx_pci_mbox_doorbell_kick(NPCM7xxPciMboxState *s, uint8_t hif)
{
    struct sockaddr_un peer;

    if (s->doorbell_out_fd < 0 || !s->doorbell_out_path ||
        !s->doorbell_out_path[0]) {
        return;
    }
    memset(&peer, 0, sizeof(peer));
    peer.sun_family = AF_UNIX;
    strncpy(peer.sun_path, s->doorbell_out_path, sizeof(peer.sun_path) - 1);
    /* Ignore errors: BIOS may not have opened its socket yet. */
    sendto(s->doorbell_out_fd, &hif, sizeof(hif), 0,
           (struct sockaddr *)&peer, sizeof(peer));
}

/*
 * npcm7xx_pci_mbox_doorbell_setup - create and register the doorbell
 * sockets.
 *
 * Called at the end of realize().  Failure is non-fatal: a warning is emitted
 * and the device continues without doorbell support (standalone mode).
 */
static void npcm7xx_pci_mbox_doorbell_setup(NPCM7xxPciMboxState *s)
{
    /* Incoming doorbell (BIOS -> BMC) */
    if (s->doorbell_in_path && s->doorbell_in_path[0]) {
        struct sockaddr_un addr;
        int fd;

        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        if (strlen(s->doorbell_in_path) >= sizeof(addr.sun_path)) {
            warn_report("npcm7xx-pci-mbox: doorbell-in path too long,"
                        " ignored");
            goto out_in;
        }
        strncpy(addr.sun_path, s->doorbell_in_path, sizeof(addr.sun_path) - 1);

        fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (fd < 0) {
            warn_report("npcm7xx-pci-mbox: socket() for doorbell-in: %s",
                        strerror(errno));
            goto out_in;
        }
        unlink(s->doorbell_in_path);   /* remove stale socket file if any */
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            warn_report("npcm7xx-pci-mbox: bind() doorbell-in '%s': %s",
                        s->doorbell_in_path, strerror(errno));
            close(fd);
            goto out_in;
        }
        s->doorbell_in_fd = fd;
        qemu_set_fd_handler(fd, npcm7xx_pci_mbox_doorbell_read, NULL, s);
        warn_report("npcm7xx-pci-mbox: doorbell-in socket bound at"
                    " '%s' (fd=%d)", s->doorbell_in_path, fd);
    }
out_in:

    /* Outgoing doorbell (BMC -> BIOS) */
    if (s->doorbell_out_path && s->doorbell_out_path[0]) {
        struct sockaddr_un addr;
        int fd;

        memset(&addr, 0, sizeof(addr));
        if (strlen(s->doorbell_out_path) >= sizeof(addr.sun_path)) {
            warn_report("npcm7xx-pci-mbox: doorbell-out path too long,"
                        " ignored");
            return;
        }
        fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (fd < 0) {
            warn_report("npcm7xx-pci-mbox: socket() for doorbell-out: %s",
                        strerror(errno));
            return;
        }
        s->doorbell_out_fd = fd;
    }
}

/*
 * npcm7xx_pci_mbox_doorbell_cleanup - close sockets and remove socket
 * file.
 *
 * Called from unrealize().
 */
static void npcm7xx_pci_mbox_doorbell_cleanup(NPCM7xxPciMboxState *s)
{
    if (s->doorbell_in_fd >= 0) {
        qemu_set_fd_handler(s->doorbell_in_fd, NULL, NULL, NULL);
        close(s->doorbell_in_fd);
        s->doorbell_in_fd = -1;
        if (s->doorbell_in_path) {
            unlink(s->doorbell_in_path);
        }
    }
    if (s->doorbell_out_fd >= 0) {
        close(s->doorbell_out_fd);
        s->doorbell_out_fd = -1;
    }
}

/* Helpers */

static void npcm7xx_pci_mbox_update_irq(NPCM7xxPciMboxState *s)
{
    /*
     * BMC IRQ: assert when any CIF bit in BMBXSTAT is set AND the
     * corresponding CIE bit in BMBXCTL is also set.
     */
    int bmc_level = !!((s->bmbxstat & BMBXSTAT_CIF_MASK) &
                       (s->bmbxctl  & BMBXCTL_CIE_MASK));
    qemu_set_irq(s->irq, bmc_level);

    /*
     * Host IRQ: assert when any HIF bit in HMBXSTAT is set AND the
     * corresponding HIE bit in HMBXCTL is also set.
     * (host_irq is typically unconnected in NPCM845 QEMU since there is
     * no PCIe host emulation; qemu_set_irq(NULL, ...) is a safe no-op.)
     */
    int host_level = !!((s->hmbxstat & HMBXSTAT_HIF_MASK) &
                        (s->hmbxctl  & HMBXCTL_HIE_MASK));
    qemu_set_irq(s->host_irq, host_level);
}

/* Control register MMIO */

static uint64_t npcm7xx_pci_mbox_ctrl_read(void *opaque, hwaddr offset,
                                            unsigned size)
{
    NPCM7xxPciMboxState *s = opaque;

    switch (offset) {
    /* BMC Core Registers */
    case BMBXSTAT_REG:
        return s->bmbxstat;

    case BMBXCTL_REG:
        return s->bmbxctl;

    case BMBXCMD_REG:
        return s->bmbxcmd;

    /* Host BAR1 Registers */
    case HMBXSTAT_REG:
        return s->hmbxstat;

    case HMBXCTL_REG:
        /*
         * CFGLCK (bit 30) is read-only from the host side; it mirrors the
         * value that the BMC wrote to BMBXCTL.CFGLCK.  All other bits come
         * from hmbxctl directly.
         */
        return (s->hmbxctl & ~HMBXCTL_CFGLCK) |
               (s->bmbxctl & BMBXCTL_CFGLCK ? HMBXCTL_CFGLCK : 0);

    case HMBXCMD_REG:
        return s->hmbxcmd;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: read at bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void npcm7xx_pci_mbox_ctrl_write(void *opaque, hwaddr offset,
                                        uint64_t value, unsigned size)
{
    NPCM7xxPciMboxState *s = opaque;
    uint32_t v = (uint32_t)value;

    switch (offset) {
    /* BMC Core Registers */

    case BMBXSTAT_REG:
        /*
         * BMBXSTAT is W1C (Write-1-to-Clear).
         * The BMC kernel driver clears CIF bits here after processing an
         * incoming message from the host (BIOS/FPGA).
         * Writing 1 to any bit clears that bit; writing 0 has no effect.
         */
        s->bmbxstat &= ~(v & BMBXSTAT_W_MASK);
        npcm7xx_pci_mbox_update_irq(s);
        break;

    case BMBXCTL_REG:
        /*
         * BMBXCTL is fully R/W.
         * The kernel driver sets CIE bits in open() to enable BMC interrupts.
         * The BMC also sets/clears CFGLCK to lock/unlock PCI config registers;
         * CFGLCK is mirrored as read-only in HMBXCTL.
         */
        s->bmbxctl = v & BMBXCTL_W_MASK;
        npcm7xx_pci_mbox_update_irq(s);
        break;

    case BMBXCMD_REG:
        /*
         * BMBXCMD is W1S (Write-1-to-Set) for HIF bits.
         * The BMC writes HIF bits here to signal the PCIe host (BIOS/FPGA)
         * that a response has been written to the shared RAM.
         * Writing 1 to any HIF bit:
         *   1. Sets that bit in BMBXCMD (W1S, bits accumulate until
         *      host clears)
         *   2. Sets the matching bit in HMBXSTAT.HIF (host sees the flag)
         *   3. May assert the host IRQ if the corresponding HIE bit is set
         * Writing 0 has no effect.
         */
        {
            uint32_t new_hif = v & BMBXCMD_HIF_MASK;
            s->bmbxcmd  |= new_hif;   /* W1S: accumulate in BMBXCMD         */
            s->hmbxstat |= new_hif;   /* propagate to Host Mailbox Status   */
            /* Notify BIOS via doorbell socket (if configured). */
            npcm7xx_pci_mbox_doorbell_kick(s, (uint8_t)new_hif);
        }
        npcm7xx_pci_mbox_update_irq(s);
        break;

    /* Host BAR1 Registers */

    case HMBXSTAT_REG:
        /*
         * HMBXSTAT is W1C (Write-1-to-Clear).
         * The PCIe host (BIOS/FPGA) clears HIF bits here after processing
         * a response written by the BMC.
         * Writing 1 to any bit clears that bit; writing 0 has no effect.
         */
        s->hmbxstat &= ~(v & HMBXSTAT_W_MASK);
        npcm7xx_pci_mbox_update_irq(s);
        break;

    case HMBXCTL_REG:
        /*
         * HMBXCTL is mixed-type:
         *   HIDECFG (bit 31): R/W, hides the PCI config space from host
         *   CFGLCK  (bit 30): RO, mirrors BMBXCTL.CFGLCK; ignored on write
         *   HIE7-0  (bits 7-0): R/W, host interrupt enable bits
         */
        s->hmbxctl = v & HMBXCTL_W_MASK;   /* CFGLCK excluded from W_MASK */
        npcm7xx_pci_mbox_update_irq(s);
        break;

    case HMBXCMD_REG:
        /*
         * HMBXCMD is W1S (Write-1-to-Set) for CIF bits.
         * The PCIe host (BIOS/FPGA) writes CIF bits here to signal the BMC
         * that a new message has been placed in the shared RAM.
         * Writing 1 to any CIF bit:
         *   1. Sets that bit in HMBXCMD (W1S, bits accumulate)
         *   2. Sets the matching bit in BMBXSTAT.CIF (BMC sees the flag)
         *   3. May assert the BMC IRQ if the corresponding CIE bit is set
         * Writing 0 has no effect.
         */
        {
            uint32_t new_cif = v & HMBXCMD_CIF_MASK;
            s->hmbxcmd  |= new_cif;   /* W1S: accumulate in HMBXCMD         */
            s->bmbxstat |= new_cif;   /* propagate to BMC Mailbox Status    */
            qemu_log_mask(LOG_UNIMP,
                          "npcm7xx-pci-mbox: HMBXCMD write CIF=0x%02x -> "
                          "hmbxcmd=0x%08x bmbxstat=0x%08x bmbxctl_cie=0x%02x "
                          "irq=%d\n",
                          new_cif, s->hmbxcmd, s->bmbxstat,
                          s->bmbxctl & BMBXCTL_CIE_MASK,
                          !!((s->bmbxstat & BMBXSTAT_CIF_MASK) &
                             (s->bmbxctl  & BMBXCTL_CIE_MASK)));
        }
        npcm7xx_pci_mbox_update_irq(s);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write at bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        break;
    }
}

static const MemoryRegionOps npcm7xx_pci_mbox_ctrl_ops = {
    .read       = npcm7xx_pci_mbox_ctrl_read,
    .write      = npcm7xx_pci_mbox_ctrl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/* Device realise */

static void npcm7xx_pci_mbox_realize(DeviceState *dev, Error **errp)
{
    NPCM7xxPciMboxState *s = NPCM7XX_PCI_MBOX(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    /*
     * npcm8xx wires TWO instances of this device (pci_mbox1 @0xf0848000 and
     * pci_mbox2 @0xf0868000).  MemoryRegion/RAMBlock names must be globally
     * unique or QEMU aborts ("RAMBlock npcm7xx-pci-mbox-ram already
     * registered"), so give each instance a distinct suffix.
     */
    static int npcm7xx_pci_mbox_instance_id;
    int inst = npcm7xx_pci_mbox_instance_id++;
    g_autofree char *ctrl_name =
        g_strdup_printf("npcm7xx-pci-mbox-ctrl-%d", inst);
    g_autofree char *ram_name =
        g_strdup_printf("npcm7xx-pci-mbox-ram-%d", inst);

    /* Region 0: control registers (BMBXSTAT/CTL/CMD + HMBXSTAT/CTL/CMD) */
    memory_region_init_io(&s->ctrl_mr, OBJECT(s),
                          &npcm7xx_pci_mbox_ctrl_ops, s,
                          ctrl_name,
                          NPCM7XX_PCI_MBOX_CTRL_SIZE);
    sysbus_init_mmio(sbd, &s->ctrl_mr);

    /*
     * Region 1: dual-ported RAM.
     * Only the FIRST instance (mbox1 -> /dev/npcm7xx-pci-mbox0) is the real
     * BMC<->BIOS channel the iDRAC SMAD daemon uses, so only it is backed
     * by the shared shm file.  shm-path is a -global (type-wide) property,
     * so without this guard the SECOND instance (mbox2) would open the
     * SAME shm-path too -> two RAM_SHARED regions mmap'ing one fd, which
     * breaks early guest boot.  mbox2 is an unused second SoC block, so it
     * gets private RAM instead.
     */
    if (inst == 0 && s->shm_path && s->shm_path[0] != '\0') {
        /*
         * Back the RAM with a POSIX shared-memory file so that the BIOS
         * QEMU / FPGA can mmap the same file and write into it directly.
         */
        int fd = open(s->shm_path, O_RDWR | O_CREAT, 0600);
        if (fd < 0) {
            error_setg_errno(errp, errno,
                             "npcm7xx-pci-mbox: cannot open shm file '%s'",
                             s->shm_path);
            return;
        }
        if (ftruncate(fd, NPCM7XX_PCI_MBOX_RAM_SIZE) < 0) {
            error_setg_errno(errp, errno,
                             "npcm7xx-pci-mbox: ftruncate '%s' failed",
                             s->shm_path);
            close(fd);
            return;
        }
        if (!memory_region_init_ram_from_fd(&s->ram_mr, OBJECT(s),
                                            ram_name,
                                            NPCM7XX_PCI_MBOX_RAM_SIZE,
                                            RAM_SHARED, fd, 0, errp)) {
            close(fd);
            return;
        }
        close(fd);   /* MemoryRegion holds its own reference via mmap */
    } else {
        /* Standalone / no shared file: private anonymous RAM */
        memory_region_init_ram(&s->ram_mr, OBJECT(s),
                               ram_name,
                               NPCM7XX_PCI_MBOX_RAM_SIZE, errp);
        if (*errp) {
            return;
        }
    }
    sysbus_init_mmio(sbd, &s->ram_mr);

    /* IRQ 0: BMC interrupt, connected to GIC SPI 105/106 in npcm8xx.c */
    sysbus_init_irq(sbd, &s->irq);
    /*
     * IRQ 1: Host (PCIe) interrupt, typically unconnected in NPCM845 QEMU
     * since there is no PCIe host emulation.  Declared here for completeness
     * and future use; qemu_set_irq(NULL, ...) is a safe no-op.
     */
    sysbus_init_irq(sbd, &s->host_irq);

    /*
     * Optional doorbell sockets for BIOS<->BMC interrupt signalling.  Only
     * the first instance (mbox1) sets them up: doorbell-in/out are
     * -global (type-wide) properties, so a second instance would
     * unlink+rebind the SAME socket path and steal mbox1's doorbell.
     * mbox1 is the channel the iDRAC actually uses.
     */
    if (inst == 0) {
        npcm7xx_pci_mbox_doorbell_setup(s);
    }
}

/* Unrealize */

static void npcm7xx_pci_mbox_unrealize(DeviceState *dev)
{
    npcm7xx_pci_mbox_doorbell_cleanup(NPCM7XX_PCI_MBOX(dev));
}

/* Reset */

static void npcm7xx_pci_mbox_reset(DeviceState *dev)
{
    NPCM7xxPciMboxState *s = NPCM7XX_PCI_MBOX(dev);

    /* BMC Core Registers: all zero on Host Domain / BMC_Reset.PCIMBX reset */
    s->bmbxstat = 0;
    s->bmbxctl  = 0;
    s->bmbxcmd  = 0;

    /*
     * Host BAR1 Registers:
     *   HMBXSTAT reset value is X000_0000h per datasheet; the X bit (CORI,
     *   bit 31) indicates that a PCIMBX reset occurred.  The datasheet note
     *   says "clear this bit at initialization before using it as an
     *   indication", so we initialise CORI=1 to faithfully represent the
     *   post-reset state that guest firmware should clear.
     */
    s->hmbxstat = HMBXSTAT_CORI;
    s->hmbxctl  = 0;
    s->hmbxcmd  = 0;

    npcm7xx_pci_mbox_update_irq(s);
}

/* Instance init */

static void npcm7xx_pci_mbox_instance_init(Object *obj)
{
    NPCM7xxPciMboxState *s = NPCM7XX_PCI_MBOX(obj);
    /*
     * Initialise doorbell fds to -1 so cleanup and kick can safely test
     * whether the sockets have been opened (QEMU zero-initialises state
     * structs, making fd 0 = stdin, which would be wrong to close/use).
     */
    s->doorbell_in_fd  = -1;
    s->doorbell_out_fd = -1;
}

/* Properties */

static const Property npcm7xx_pci_mbox_properties[] = {
    DEFINE_PROP_STRING("shm-path",     NPCM7xxPciMboxState, shm_path),
    DEFINE_PROP_STRING("doorbell-in",  NPCM7xxPciMboxState, doorbell_in_path),
    DEFINE_PROP_STRING("doorbell-out", NPCM7xxPciMboxState, doorbell_out_path),
};

/* Class init */

static void npcm7xx_pci_mbox_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc      = "NPCM7xx/NPCM8xx PCI Mailbox";
    dc->realize   = npcm7xx_pci_mbox_realize;
    dc->unrealize = npcm7xx_pci_mbox_unrealize;
    device_class_set_legacy_reset(dc, npcm7xx_pci_mbox_reset);
    device_class_set_props(dc, npcm7xx_pci_mbox_properties);
}

static const TypeInfo npcm7xx_pci_mbox_info = {
    .name          = TYPE_NPCM7XX_PCI_MBOX,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NPCM7xxPciMboxState),
    .instance_init = npcm7xx_pci_mbox_instance_init,
    .class_init    = npcm7xx_pci_mbox_class_init,
};

static void npcm7xx_pci_mbox_register_types(void)
{
    type_register_static(&npcm7xx_pci_mbox_info);
}

type_init(npcm7xx_pci_mbox_register_types)
