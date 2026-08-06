/*
 * Nuvoton NPCM400 SPIM (SPI Master Controller)
 *
 * The SPIM controller is the CPU-side SPI master used to access the internal
 * SPI NOR flash chip. It supports three operation modes:
 *
 *   Normal I/O  (OPMODE=0) – byte-by-byte programmatic access
 *   DMA write   (OPMODE=1) – burst write via SRAMADDR/DMACNT/FADDR
 *   DMA read    (OPMODE=2) – burst read  via SRAMADDR/DMACNT/FADDR
 *   DMM         (OPMODE=3) – direct memory-mapped read window (default/reset)
 *
 * The flash content is backed by a 1 MB in-memory array (default) or by a
 * block device attached via the "drive" QEMU property, allowing the Zephyr
 * binary to be pre-loaded with:
 *
 *   -drive file=SMCNPCM_signed.bin,format=raw,if=mtd
 *
 * The DMM window is exported as SysBus MMIO region 1 so the SoC can map it
 * at the flash base address (0x00080000) where the Cortex-M reset vector
 * table lives.
 *
 * Spec reference: NPCM400F_AS_Rev.0.91_20240808, section 4.7.5 (pp. 245-254)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SSI_NPCM400_SPIM_H
#define HW_SSI_NPCM400_SPIM_H

#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "qom/object.h"

/* Forward-declare BlockBackend to avoid pulling sysemu headers into the SOC. */
typedef struct BlockBackend BlockBackend;

#define TYPE_NPCM400_SPIM   "npcm400-spim"
OBJECT_DECLARE_SIMPLE_TYPE(NPCM400SPIMState, NPCM400_SPIM)

/* Physical flash size and register window size */
#define NPCM400_SPIM_FLASH_SIZE (1u << 20)   /* 1 MiB */
#define NPCM400_SPIM_REG_SIZE   0x100u        /* register window (per DTS) */

/* SPI NOR transaction phase */
typedef enum {
    SPIM_SPI_CMD = 0,   /* waiting for command byte */
    SPIM_SPI_ADDR,      /* collecting address bytes  */
    SPIM_SPI_DUMMY,     /* consuming dummy bytes     */
    SPIM_SPI_DATA,      /* transferring data         */
} NPCM400SPIMSPIPhase;

struct NPCM400SPIMState {
    SysBusDevice parent_obj;

    SSIBus *spi;
    qemu_irq flash_cs;

    MemoryRegion mmio;  /* SysBus MMIO 0: register window (0x40017000) */
    MemoryRegion dmm;   /* SysBus MMIO 1: DMM flash window  (0x80000)  */

    /* ---- SPIM registers (spec section 4.7.5) -------------------------- */
    uint32_t ctl0;      /* 0x00 CTL0 - Control/Status Register 0 */
    uint32_t ctl1;      /* 0x04 CTL1 - Control Register 1        */
    uint32_t rxclkdly;  /* 0x0C - RX Clock Delay Control         */
    uint32_t rx[4];     /* 0x10-0x1C - Data Receive Registers    */
    uint32_t tx[4];     /* 0x20-0x2C - Data Transmit Registers   */
    uint32_t sramaddr;  /* 0x30 - SRAM Memory Address            */
    uint32_t dmacnt;    /* 0x34 - DMA Transfer Byte Count        */
    uint32_t faddr;     /* 0x38 - SPI Flash Address              */
    uint32_t dmmctl;    /* 0x44 - DMM Control                    */
    uint32_t ctl2;      /* 0x48 - Control Register 2             */

    /* ---- SPI NOR state machine (active between CS assert/deassert) ---- */
    bool cs_asserted;
    bool wel;                   /* Write Enable Latch              */
    uint8_t spi_cmd;            /* current command byte            */
    NPCM400SPIMSPIPhase spi_phase;
    uint32_t spi_addr;          /* accumulated address             */
    int  spi_addr_cnt;          /* address bytes still expected    */
    int  spi_dummy_cnt;         /* dummy bytes still expected      */
    uint32_t spi_pos;           /* current flash byte position     */
    const uint8_t *spi_resp;    /* pointer into fixed response buf */
    int  spi_resp_left;         /* bytes remaining in fixed resp   */

    /* ---- Flash storage ----------------------------------------------- */
    uint8_t  *flash;            /* 1 MiB flash contents (heap)     */
    uint32_t  flash_size;
    BlockBackend *blk;          /* optional block-device backend   */
};

/**
 * npcm400_spim_load_data - Write raw bytes into the SPIM flash storage.
 *
 * Called by the machine code to pre-populate flash when a kernel image is
 * loaded via -kernel instead of -drive if=mtd.
 */
void npcm400_spim_load_data(NPCM400SPIMState *s, hwaddr offset,
                             const void *data, size_t size);

#endif /* HW_SSI_NPCM400_SPIM_H */
