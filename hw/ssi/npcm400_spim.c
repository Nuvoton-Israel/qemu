/*
 * Nuvoton NPCM400 SPIM (SPI Master Controller)
 *
 * Full Normal I/O mode SPI NOR flash emulation with a 1 MiB backing store.
 *
 * Transaction flow (Normal I/O mode, per Zephyr spi_npcm4xx_spim driver):
 *   1. Guest writes CTL1[SS]=0  → CS assert, new command starts.
 *   2. For each byte:
 *        a. Write SPIM_CTL0 (direction QDIODIR, OPMODE=0, DWIDTH=8).
 *        b. Write SPIM_TX0  if transmitting (QDIODIR=1).
 *        c. Write CTL1 |= SPIMEN.
 *        d. HW executes byte, clears SPIMEN, sets CTL0[IF].
 *        e. Read SPIM_RX0   if receiving  (QDIODIR=0).
 *   3. Guest writes CTL1[SS]=1  → CS deassert, command ends.
 *
 * DMA read mode (OPMODE=2): on SPIMEN write, copies DMACNT bytes from
 * flash[FADDR] to guest SRAM at SRAMADDR.  This is used by drivers that
 * do bulk reads.
 *
 * DMM mode (OPMODE=3, reset default): exposes flash as a read-only memory
 * window exported as SysBus MMIO region 1 (mapped at 0x00080000 by the SoC).
 *
 * Spec reference: NPCM400F_AS_Rev.0.91_20240808, §4.7.5 (pp. 245-254)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/ssi/npcm400_spim.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/irq.h"
#include "hw/block/m25p80_sfdp.h"
#include "hw/ssi/ssi.h"
#include "exec/cpu-common.h"   /* cpu_physical_memory_write */
#include "migration/vmstate.h"
#include "system/block-backend.h"

/* --------------------------------------------------------------------------
 * Register offsets (SPIM base = 0x40017000)
 * -------------------------------------------------------------------------- */
#define REG_CTL0        0x00
#define REG_CTL1        0x04
#define REG_RXCLKDLY    0x0C
#define REG_RX0         0x10
#define REG_RX1         0x14
#define REG_RX2         0x18
#define REG_RX3         0x1C
#define REG_TX0         0x20
#define REG_TX1         0x24
#define REG_TX2         0x28
#define REG_TX3         0x2C
#define REG_SRAMADDR    0x30
#define REG_DMACNT      0x34
#define REG_FADDR       0x38
#define REG_DMMCTL      0x44
#define REG_CTL2        0x48

/* --------------------------------------------------------------------------
 * CTL0 field masks / bits  (spec p.246-248)
 * -------------------------------------------------------------------------- */
#define CTL0_CMDCODE_SHIFT  24
#define CTL0_OPMODE_SHIFT   22
#define CTL0_OPMODE_MASK    (0x3u << 22)
#define  OPMODE_NORMAL_IO   0u
#define  OPMODE_DMA_WRITE   1u
#define  OPMODE_DMA_READ    2u
#define  OPMODE_DMM         3u
#define CTL0_QDIODIR        (1u << 15) /* 0 = RX (read flash), 1 = TX (write) */
#define CTL0_BURSTNUM_SHIFT 13
#define CTL0_BURSTNUM_MASK  (0x3u << 13)
#define CTL0_DWIDTH_SHIFT   8
#define CTL0_DWIDTH_MASK    (0x1Fu << 8)
#define CTL0_IF             (1u << 7)  /* interrupt flag  (W1C)  */
#define CTL0_IEN            (1u << 6)  /* interrupt enable */
#define CTL0_B4ADDREN       (1u << 5)  /* 4-byte address mode     */
/* Reset: CMDCODE=0x03, OPMODE=3(DMM), CIPHOFF=1 → bits[1:0]=11b */
#define CTL0_RESET          0x03C00003u

/* --------------------------------------------------------------------------
 * CTL1 field masks / bits  (spec p.248-249)
 * -------------------------------------------------------------------------- */
#define CTL1_DIVIDER_MASK   0xFFFF0000u
#define CTL1_SSACTPOL       (1u << 5)  /* SS active polarity (0=active-low) */
#define CTL1_SS             (1u << 4)  /* 0=CS asserted, 1=CS deasserted    */
#define CTL1_CDINVAL        (1u << 3)  /* cache invalidate (W1C)            */
#define CTL1_CACHEOFF       (1u << 1)
#define CTL1_SPIMEN         (1u << 0)  /* start; HW clears when done        */
/* Reset: SS=1 (deasserted) */
#define CTL1_RESET          0x00000010u

/* --------------------------------------------------------------------------
 * SPI NOR command codes
 * -------------------------------------------------------------------------- */
#define NOR_CMD_WREN    0x06u
#define NOR_CMD_WRDI    0x04u
#define NOR_CMD_RDID    0x9Fu
#define NOR_CMD_RDSR    0x05u
#define NOR_CMD_RDSR2   0x35u
#define NOR_CMD_WRSR    0x01u
#define NOR_CMD_READ    0x03u
#define NOR_CMD_FREAD   0x0Bu
#define NOR_CMD_PP      0x02u
#define NOR_CMD_SE      0x20u  /* Sector  Erase  4 KB  */
#define NOR_CMD_BE32K   0x52u  /* Block   Erase 32 KB  */
#define NOR_CMD_BE64K   0xD8u  /* Block   Erase 64 KB  */
#define NOR_CMD_CE      0xC7u  /* Chip    Erase        */
#define NOR_CMD_CE2     0x60u
#define NOR_CMD_SFDP    0x5Au  /* Read SFDP (3-addr + 1 dummy) */
#define NOR_CMD_EN4B    0xB7u  /* Enter 4-byte address mode    */
#define NOR_CMD_EX4B    0xE9u  /* Exit  4-byte address mode    */

/*
 * JEDEC ID: Winbond W25Q80DV  (1 MiB, 8 Mbit)
 *   Manufacturer: 0xEF
 *   Memory type:  0x40  (SPI NOR)
 *   Capacity:     0x14  (2^20 bytes)
 */
static const uint8_t spim_jedec_id[] = { 0xEF, 0x40, 0x14 };

/* Begin a new command sequence (called when CS is asserted) */
static void spim_spi_reset(NPCM400SPIMState *s)
{
    s->spi_phase     = SPIM_SPI_CMD;
    s->spi_cmd       = 0;
    s->spi_addr      = 0;
    s->spi_addr_cnt  = 0;
    s->spi_dummy_cnt = 0;
    s->spi_pos       = 0;
    s->spi_resp      = NULL;
    s->spi_resp_left = 0;
}

/* Erase helper: fill a range of flash with 0xFF */
static void spim_flash_erase(NPCM400SPIMState *s, uint32_t base, uint32_t sz)
{
    if (!s->wel) {
        return;
    }
    base &= ~(sz - 1);  /* align to erase-unit boundary */
    if (base >= s->flash_size) {
        return;
    }
    uint32_t end = MIN(base + sz, s->flash_size);
    memset(s->flash + base, 0xFF, end - base);
}

/*
 * Process one byte of a Normal I/O transaction.
 *
 * @tx      – byte written to SPIM_TX0 (valid only if is_write)
 * @is_write – QDIODIR: true = host→flash, false = flash→host
 * Returns the received byte (placed in SPIM_RX0).
 */
static uint8_t spim_spi_byte(NPCM400SPIMState *s, uint8_t tx, bool is_write)
{
    uint8_t rx = 0xFF;

    switch (s->spi_phase) {

    case SPIM_SPI_CMD:
        s->spi_cmd = tx;
        switch (tx) {
        case NOR_CMD_RDID:
            s->spi_resp      = spim_jedec_id;
            s->spi_resp_left = sizeof(spim_jedec_id);
            s->spi_phase     = SPIM_SPI_DATA;
            break;
        case NOR_CMD_RDSR:
        case NOR_CMD_RDSR2:
            s->spi_phase = SPIM_SPI_DATA;
            break;
        case NOR_CMD_WREN:
            s->wel = true;
            break;
        case NOR_CMD_WRDI:
        case NOR_CMD_EN4B:
        case NOR_CMD_EX4B:
            s->wel = false;
            break;
        case NOR_CMD_WRSR:
            s->spi_phase    = SPIM_SPI_DATA;
            break;
        case NOR_CMD_READ:
            s->spi_addr_cnt  = 3;
            s->spi_addr      = 0;
            s->spi_phase     = SPIM_SPI_ADDR;
            break;
        case NOR_CMD_FREAD:
            s->spi_addr_cnt  = 3;
            s->spi_addr      = 0;
            s->spi_dummy_cnt = 1;
            s->spi_phase     = SPIM_SPI_ADDR;
            break;
        case NOR_CMD_PP:
        case NOR_CMD_SE:
        case NOR_CMD_BE32K:
        case NOR_CMD_BE64K:
            s->spi_addr_cnt = 3;
            s->spi_addr     = 0;
            s->spi_phase    = SPIM_SPI_ADDR;
            break;
        case NOR_CMD_CE:
        case NOR_CMD_CE2:
            if (s->wel) {
                memset(s->flash, 0xFF, s->flash_size);
                s->wel = false;
            }
            break;
        case NOR_CMD_SFDP:
            s->spi_addr_cnt  = 3;
            s->spi_addr      = 0;
            s->spi_dummy_cnt = 1;
            s->spi_phase     = SPIM_SPI_ADDR;
            break;
        default:
            /* Unknown command – ignore */
            break;
        }
        break;

    case SPIM_SPI_ADDR:
        s->spi_addr = (s->spi_addr << 8) | tx;
        s->spi_addr_cnt--;
        if (s->spi_addr_cnt == 0) {
            /* Execute address-only (erase) commands immediately */
            switch (s->spi_cmd) {
            case NOR_CMD_SE:
                spim_flash_erase(s, s->spi_addr, 4 * KiB);
                break;
            case NOR_CMD_BE32K:
                spim_flash_erase(s, s->spi_addr, 32 * KiB);
                break;
            case NOR_CMD_BE64K:
                spim_flash_erase(s, s->spi_addr, 64 * KiB);
                break;
            }
            s->spi_pos = s->spi_addr;
            if (s->spi_dummy_cnt > 0) {
                s->spi_phase = SPIM_SPI_DUMMY;
            } else {
                s->spi_phase = SPIM_SPI_DATA;
            }
        }
        break;

    case SPIM_SPI_DUMMY:
        s->spi_dummy_cnt--;
        if (s->spi_dummy_cnt == 0) {
            s->spi_phase = SPIM_SPI_DATA;
        }
        break;

    case SPIM_SPI_DATA:
        /* Fixed-response commands (e.g. RDID) */
        if (s->spi_resp && s->spi_resp_left > 0) {
            rx = *s->spi_resp++;
            s->spi_resp_left--;
            break;
        }
        s->spi_resp = NULL;

        switch (s->spi_cmd) {
        case NOR_CMD_RDSR:
            /* WEL=bit1, WIP=bit0 (never busy in our model) */
            rx = s->wel ? 0x02u : 0x00u;
            break;
        case NOR_CMD_RDSR2:
            rx = 0x00u;
            break;
        case NOR_CMD_WRSR:
            /* Accept but ignore status register writes */
            break;
        case NOR_CMD_READ:
        case NOR_CMD_FREAD:
            if (s->spi_pos < s->flash_size) {
                rx = s->flash[s->spi_pos++];
            }
            break;
        case NOR_CMD_PP:
            /* NOR page-program: can only change 1→0 (no erase) */
            if (s->wel && s->spi_pos < s->flash_size) {
                s->flash[s->spi_pos] &= tx;
                s->spi_pos++;
            }
            break;
        case NOR_CMD_SFDP:
            /* Reuse QEMU's canonical m25p80 SFDP definition for w25q80bl. */
            rx = m25p80_sfdp_w25q80bl(s->spi_pos++);
            break;
        default:
            break;
        }
        break;
    }

    return rx;
}

/* --------------------------------------------------------------------------
 * CTL1[SS] transition – CS assert / deassert
 * -------------------------------------------------------------------------- */
static void spim_handle_cs_change(NPCM400SPIMState *s, bool now_asserted)
{
    if (now_asserted == s->cs_asserted) {
        return;
    }
    s->cs_asserted = now_asserted;

    /* SSI GPIO CS is active-low: 0 selects flash, 1 deselects flash. */
    qemu_set_irq(s->flash_cs, !now_asserted);

    if (now_asserted) {
        spim_spi_reset(s);
    } else {
        /* CS deassert ends write commands and clears WEL */
        switch (s->spi_cmd) {
        case NOR_CMD_PP:
        case NOR_CMD_SE:
        case NOR_CMD_BE32K:
        case NOR_CMD_BE64K:
        case NOR_CMD_CE:
        case NOR_CMD_CE2:
        case NOR_CMD_WRSR:
            s->wel = false;
            /* Flush modified pages to the block backend */
            if (s->blk) {
                blk_pwrite(s->blk, 0, s->flash_size, s->flash, 0);
            }
            break;
        default:
            break;
        }
    }
}

/* --------------------------------------------------------------------------
 * Normal I/O execute: triggered by SPIMEN write
 * -------------------------------------------------------------------------- */
static void spim_execute_normal_io(NPCM400SPIMState *s)
{
    bool is_write = (s->ctl0 & CTL0_QDIODIR) != 0;
    uint8_t tx    = is_write ? (uint8_t)(s->tx[0] & 0xFFu) : 0x00u;

    uint8_t rx;
    uint8_t tracked_rx;

    /*
     * Prefer talking to an attached SSI flash device (m25p80 family).
     * If none is attached, keep the built-in fallback model for bring-up.
     */
    tracked_rx = spim_spi_byte(s, tx, is_write);

    if (!QTAILQ_EMPTY(&BUS(s->spi)->children)) {
        rx = ssi_transfer(s->spi, tx) & 0xFFu;

        /*
         * npc m4xx SPIM expects one dummy byte before SFDP payload while the
         * generic m25p80 model consumes dummy clocks differently.
         * Serve SFDP reads through the tracked path to match guest behavior.
         */
        if (!is_write && s->spi_cmd == NOR_CMD_SFDP) {
            rx = tracked_rx;
        }
    } else {
        rx = tracked_rx;
    }

    s->rx[0] = rx;              /* result available in SPIM_RX0  */
    s->ctl0 |= CTL0_IF;         /* signal transfer complete       */
    s->ctl1 &= ~CTL1_SPIMEN;    /* HW auto-clears SPIMEN          */
}

/* --------------------------------------------------------------------------
 * DMA read execute: triggered by SPIMEN write in OPMODE=DMA_READ
 * -------------------------------------------------------------------------- */
static void spim_execute_dma_read(NPCM400SPIMState *s)
{
    uint32_t src  = s->faddr;
    uint32_t dst  = s->sramaddr;
    uint32_t cnt  = s->dmacnt & 0x00FFFFFFu;  /* [23:0] */

    if (cnt == 0 || src >= s->flash_size) {
        goto done;
    }
    uint32_t avail = s->flash_size - src;
    cnt = MIN(cnt, avail);

    cpu_physical_memory_write(dst, s->flash + src, cnt);

done:
    s->ctl0 |= CTL0_IF;
    s->ctl1 &= ~CTL1_SPIMEN;
}

/* --------------------------------------------------------------------------
 * DMM window – read-only view of flash storage
 * -------------------------------------------------------------------------- */
static uint64_t spim_dmm_read(void *opaque, hwaddr addr, unsigned int size)
{
    NPCM400SPIMState *s = opaque;
    uint64_t val = MAKE_64BIT_MASK(0, size * 8); /* erased flash default */

    if (addr + size > s->flash_size) {
        return val;
    }
    val = 0;
    for (unsigned i = 0; i < size; i++) {
        val |= (uint64_t)s->flash[addr + i] << (i * 8);
    }
    return val;
}

static void spim_dmm_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned int size)
{
    /*
     * NOR flash: CPU cannot write via the DMM window directly.
     * Writes must go through Normal I/O mode (PP command).
     */
    qemu_log_mask(LOG_GUEST_ERROR,
                  TYPE_NPCM400_SPIM ": DMM window write ignored at"
                  " 0x%" HWADDR_PRIx "\n", addr);
}

static const MemoryRegionOps spim_dmm_ops = {
    .read  = spim_dmm_read,
    .write = spim_dmm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* --------------------------------------------------------------------------
 * Register window – read
 * -------------------------------------------------------------------------- */
static uint64_t spim_reg_read(void *opaque, hwaddr addr, unsigned int size)
{
    NPCM400SPIMState *s = opaque;
    uint32_t val32;

    switch (addr & ~0x3u) {
    case REG_CTL0:     val32 = s->ctl0;     break;
    case REG_CTL1:
        /* SPIMEN and CDINVAL are self-clearing: always read as 0 */
        val32 = s->ctl1 & ~(CTL1_SPIMEN | CTL1_CDINVAL);
        break;
    case REG_RXCLKDLY: val32 = s->rxclkdly; break;
    case REG_RX0:      val32 = s->rx[0];    break;
    case REG_RX1:      val32 = s->rx[1];    break;
    case REG_RX2:      val32 = s->rx[2];    break;
    case REG_RX3:      val32 = s->rx[3];    break;
    case REG_TX0:      val32 = s->tx[0];    break;
    case REG_TX1:      val32 = s->tx[1];    break;
    case REG_TX2:      val32 = s->tx[2];    break;
    case REG_TX3:      val32 = s->tx[3];    break;
    case REG_SRAMADDR: val32 = s->sramaddr; break;
    case REG_DMACNT:   val32 = s->dmacnt;   break;
    case REG_FADDR:    val32 = s->faddr;    break;
    case REG_DMMCTL:   val32 = s->dmmctl;   break;
    case REG_CTL2:     val32 = s->ctl2;     break;
    default:
        val32 = 0;
        break;
    }

    /* Extract the requested byte lane(s) */
    unsigned shift = (addr & 0x3u) * 8;
    return (val32 >> shift) & MAKE_64BIT_MASK(0, size * 8);
}

/* --------------------------------------------------------------------------
 * Register window – write
 * -------------------------------------------------------------------------- */
static void spim_reg_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned int size)
{
    NPCM400SPIMState *s = opaque;
    unsigned shift = (addr & 0x3u) * 8;
    uint32_t mask  = (uint32_t)MAKE_64BIT_MASK(0, size * 8) << shift;
    uint32_t wval  = (uint32_t)(value & (mask >> shift)) << shift;

    switch (addr & ~0x3u) {

    case REG_CTL0: {
        /*
         * IF (bit 7) is W1C: writing 1 clears it.
         * All other R/W bits store as-is.
         */
        uint32_t new_ctl0 = (s->ctl0 & ~mask) | wval;
        /* Apply W1C: if guest wrote 1 to IF, clear it */
        if (wval & CTL0_IF) {
            new_ctl0 &= ~CTL0_IF;
        }
        s->ctl0 = new_ctl0;
        break;
    }

    case REG_CTL1: {
        uint32_t new_ctl1 = (s->ctl1 & ~mask) | wval;
        /* CDINVAL is W1C (cache invalidate) – always clears immediately */
        new_ctl1 &= ~CTL1_CDINVAL;

        /* Detect CS state change */
        bool old_cs = !(s->ctl1 & CTL1_SS);
        bool new_cs = !(new_ctl1 & CTL1_SS);

        /* Store without the SPIMEN bit (it will be processed below) */
        s->ctl1 = new_ctl1 & ~CTL1_SPIMEN;

        if (old_cs != new_cs) {
            spim_handle_cs_change(s, new_cs);
        }

        /* Execute transaction if SPIMEN was written */
        if (wval & CTL1_SPIMEN) {
            uint32_t opmode = (s->ctl0 & CTL0_OPMODE_MASK) >> CTL0_OPMODE_SHIFT;
            if (opmode == OPMODE_NORMAL_IO && s->cs_asserted) {
                spim_execute_normal_io(s);
            } else if (opmode == OPMODE_DMA_READ) {
                spim_execute_dma_read(s);
            }
            /* DMA write: not yet implemented; just clear SPIMEN */
            s->ctl1 &= ~CTL1_SPIMEN;
        }
        break;
    }

    case REG_RXCLKDLY: s->rxclkdly = (s->rxclkdly & ~mask) | wval; break;
    case REG_TX0:      s->tx[0]    = (s->tx[0] & ~mask) | wval;    break;
    case REG_TX1:      s->tx[1]    = (s->tx[1] & ~mask) | wval;    break;
    case REG_TX2:      s->tx[2]    = (s->tx[2] & ~mask) | wval;    break;
    case REG_TX3:      s->tx[3]    = (s->tx[3] & ~mask) | wval;    break;
    case REG_SRAMADDR: s->sramaddr = (s->sramaddr & ~mask) | wval;  break;
    case REG_DMACNT:   s->dmacnt   = (s->dmacnt & ~mask) | wval;   break;
    case REG_FADDR:    s->faddr    = (s->faddr & ~mask) | wval;     break;
    case REG_DMMCTL:   s->dmmctl   = (s->dmmctl & ~mask) | wval;   break;
    case REG_CTL2:     s->ctl2     = (s->ctl2 & ~mask) | wval;     break;
    default:
        break;
    }
}

static const MemoryRegionOps spim_reg_ops = {
    .read  = spim_reg_read,
    .write = spim_reg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* --------------------------------------------------------------------------
 * Public helper: write arbitrary data into the flash array
 * -------------------------------------------------------------------------- */
void npcm400_spim_load_data(NPCM400SPIMState *s, hwaddr offset,
                             const void *data, size_t size)
{
    if (!s->flash || offset >= s->flash_size) {
        return;
    }
    size_t avail = s->flash_size - (size_t)offset;
    memcpy(s->flash + offset, data, MIN(size, avail));
}

/* --------------------------------------------------------------------------
 * Device lifecycle
 * -------------------------------------------------------------------------- */
static void npcm400_spim_reset(DeviceState *dev)
{
    NPCM400SPIMState *s = NPCM400_SPIM(dev);

    s->ctl0     = CTL0_RESET;
    s->ctl1     = CTL1_RESET;
    s->rxclkdly = 0;
    memset(s->rx, 0, sizeof(s->rx));
    memset(s->tx, 0, sizeof(s->tx));
    s->sramaddr = 0;
    s->dmacnt   = 0;
    s->faddr    = 0;
    s->dmmctl   = 0x00080000u; /* DESELTIM reset value per spec */
    s->ctl2     = 0;

    s->cs_asserted = false;
    s->wel         = false;
    spim_spi_reset(s);

    /* Keep external SSI flash deselected across reset. */
    qemu_set_irq(s->flash_cs, 1);
}

static void npcm400_spim_realize(DeviceState *dev, Error **errp)
{
    NPCM400SPIMState *s = NPCM400_SPIM(dev);
    SysBusDevice *sbd   = SYS_BUS_DEVICE(dev);

    s->spi = ssi_create_bus(dev, "spi");
    qdev_init_gpio_out_named(dev, &s->flash_cs, "cs", 1);

    /* Allocate flash storage */
    s->flash_size = NPCM400_SPIM_FLASH_SIZE;
    s->flash = g_malloc(s->flash_size);
    memset(s->flash, 0xFF, s->flash_size);  /* erased flash = 0xFF */

    /* Load from block backend if provided */
    if (s->blk) {
        int64_t blk_len = blk_getlength(s->blk);
        if (blk_len < 0) {
            error_setg(errp, TYPE_NPCM400_SPIM ": cannot query drive size");
            return;
        }
        uint32_t load_len = MIN((uint64_t)blk_len, (uint64_t)s->flash_size);
        if (blk_pread(s->blk, 0, load_len, s->flash, 0) < 0) {
            error_setg(errp, TYPE_NPCM400_SPIM ": error reading drive");
            return;
        }
        warn_report(TYPE_NPCM400_SPIM ": loaded 0x%x bytes from drive",
                    load_len);
    }

    /* SysBus MMIO 0: register window */
    memory_region_init_io(&s->mmio, OBJECT(dev), &spim_reg_ops, s,
                          TYPE_NPCM400_SPIM ".regs", NPCM400_SPIM_REG_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);

    /* SysBus MMIO 1: DMM read-only flash window */
    memory_region_init_io(&s->dmm, OBJECT(dev), &spim_dmm_ops, s,
                          TYPE_NPCM400_SPIM ".dmm", NPCM400_SPIM_FLASH_SIZE);
    sysbus_init_mmio(sbd, &s->dmm);
}

static void npcm400_spim_finalize(Object *obj)
{
    NPCM400SPIMState *s = NPCM400_SPIM(obj);
    g_free(s->flash);
    s->flash = NULL;
}

static const VMStateDescription npcm400_spim_vmstate = {
    .name = TYPE_NPCM400_SPIM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctl0,     NPCM400SPIMState),
        VMSTATE_UINT32(ctl1,     NPCM400SPIMState),
        VMSTATE_UINT32(rxclkdly, NPCM400SPIMState),
        VMSTATE_UINT32_ARRAY(rx, NPCM400SPIMState, 4),
        VMSTATE_UINT32_ARRAY(tx, NPCM400SPIMState, 4),
        VMSTATE_UINT32(sramaddr, NPCM400SPIMState),
        VMSTATE_UINT32(dmacnt,   NPCM400SPIMState),
        VMSTATE_UINT32(faddr,    NPCM400SPIMState),
        VMSTATE_UINT32(dmmctl,   NPCM400SPIMState),
        VMSTATE_UINT32(ctl2,     NPCM400SPIMState),
        VMSTATE_BOOL(cs_asserted, NPCM400SPIMState),
        VMSTATE_BOOL(wel,         NPCM400SPIMState),
        VMSTATE_UINT8(spi_cmd,    NPCM400SPIMState),
        VMSTATE_UINT32(spi_addr,  NPCM400SPIMState),
        VMSTATE_INT32(spi_addr_cnt,  NPCM400SPIMState),
        VMSTATE_INT32(spi_dummy_cnt, NPCM400SPIMState),
        VMSTATE_UINT32(spi_pos,      NPCM400SPIMState),
        VMSTATE_VBUFFER_UINT32(flash, NPCM400SPIMState, 1, NULL, flash_size),
        VMSTATE_END_OF_LIST(),
    },
};

static const Property npcm400_spim_props[] = {
    DEFINE_PROP_DRIVE("drive", NPCM400SPIMState, blk),
};

static void npcm400_spim_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = npcm400_spim_realize;
    dc->vmsd    = &npcm400_spim_vmstate;
    device_class_set_legacy_reset(dc, npcm400_spim_reset);
    device_class_set_props(dc, npcm400_spim_props);
}

static const TypeInfo npcm400_spim_info = {
    .name          = TYPE_NPCM400_SPIM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NPCM400SPIMState),
    .instance_finalize = npcm400_spim_finalize,
    .class_init    = npcm400_spim_class_init,
};

static void npcm400_spim_register_types(void)
{
    type_register_static(&npcm400_spim_info);
}
type_init(npcm400_spim_register_types)
