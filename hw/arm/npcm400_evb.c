/*
 * Nuvoton NPCM400F EVB machine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/arm/npcm400_soc.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "system/blockdev.h"
#include "system/block-backend.h"

#define NPCM400_SYSCLK_HZ 96000000ULL
#define NPCM400_BOOTROM_VECTOR_SIZE 0x8u
#define NPCM400_BOOTROM_SRAM_BASE  (NPCM400_SRAM_BASE + NPCM400_SRAM_SIZE - 0x7000u)

static const char npcm400_default_bootrom[] = "npcm400_bootrom.bin";

static bool npcm400_load_bootrom(MachineState *machine, NPCM400SPIMState *spim)
{
    const char *bios_name = machine->firmware ?: npcm400_default_bootrom;
    g_autofree char *resolved = NULL;
    g_autoptr(GError) gerr = NULL;
    gsize file_len = 0;
    g_autofree uint8_t *file_buf = NULL;

    /* Accept absolute/relative path directly, then fall back to BIOS search. */
    if (!g_file_get_contents(bios_name, (gchar **)&file_buf, &file_len, &gerr)) {
        gerr = NULL;
        resolved = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
        if (!resolved ||
            !g_file_get_contents(resolved, (gchar **)&file_buf, &file_len, &gerr)) {
            error_report("npcm400: failed to read bootrom '%s': %s",
                         bios_name, gerr ? gerr->message : "not found");
            return false;
        }
    }

    if (file_len < 8) {
        error_report("npcm400: bootrom '%s' too small", bios_name);
        return false;
    }

    /*
     * Bootrom code executes from SRAM; vectors are mirrored at 0x0 so
     * Cortex-M reset fetches MSP/PC from the bootrom image.
     */
    rom_add_blob_fixed("npcm400.bootrom.code", file_buf, file_len,
                       NPCM400_BOOTROM_SRAM_BASE);

    if (spim) {
        npcm400_spim_load_data(spim, 0, file_buf,
                               MIN(file_len, (gsize)NPCM400_BOOTROM_VECTOR_SIZE));
    }

    /* Bootrom load is expected in normal -bios flow; keep startup log quiet. */
    (void)(resolved ?: bios_name);
    (void)file_len;
    return true;
}

static void npcm400_connect_flash(NPCM400SPIMState *spim,
                                  const char *flash_type,
                                  DriveInfo *dinfo)
{
    DeviceState *flash = qdev_new(flash_type);
    qemu_irq flash_cs;
    BlockBackend *blk;
    uint64_t blk_size;
    uint64_t perm;
    uint64_t shared_perm;

    if (dinfo) {
        blk = blk_by_legacy_dinfo(dinfo);
        blk_size = blk_getlength(blk);
        if (blk_size < NPCM400_SPIM_FLASH_SIZE) {
            blk_get_perm(blk, &perm, &shared_perm);
            blk_set_perm(blk, BLK_PERM_ALL, BLK_PERM_ALL, &error_abort);
            blk_truncate(blk, NPCM400_SPIM_FLASH_SIZE, true,
                         PREALLOC_MODE_OFF, BDRV_REQ_ZERO_WRITE,
                         &error_abort);
            blk_set_perm(blk, perm, shared_perm, &error_abort);
        }
        qdev_prop_set_drive(flash, "drive", blk);
    }
    qdev_prop_set_uint8(flash, "cs", 0);
    qdev_realize_and_unref(flash, BUS(spim->spi), &error_fatal);

    flash_cs = qdev_get_gpio_in_named(flash, SSI_GPIO_CS, 0);
    qdev_connect_gpio_out_named(DEVICE(spim), "cs", 0, flash_cs);
}

static bool npcm400_preload_spim_shadow_from_drive(NPCM400SPIMState *spim,
                                                   DriveInfo *dinfo)
{
    BlockBackend *blk;
    int64_t blk_len;
    uint32_t load_len;
    g_autofree uint8_t *buf = NULL;

    if (!spim || !dinfo) {
        return true;
    }

    blk = blk_by_legacy_dinfo(dinfo);
    blk_len = blk_getlength(blk);
    if (blk_len < 0) {
        error_report("npcm400: failed to query mtd drive size for SPIM shadow preload");
        return false;
    }

    load_len = MIN((uint64_t)blk_len, (uint64_t)NPCM400_SPIM_FLASH_SIZE);
    if (load_len == 0) {
        return true;
    }

    buf = g_malloc(load_len);
    if (blk_pread(blk, 0, load_len, buf, 0) < 0) {
        error_report("npcm400: failed to read mtd drive for SPIM shadow preload");
        return false;
    }

    npcm400_spim_load_data(spim, 0, buf, load_len);
    return true;
}

static void npcm400_evb_init(MachineState *machine)
{
    DeviceState *dev;
    NPCM400State *soc;
    Clock *sysclk;
    bool bootrom_loaded = false;
    DriveInfo *dinfo;

    sysclk = clock_new(OBJECT(machine), "SYSCLK");
    clock_set_hz(sysclk, NPCM400_SYSCLK_HZ);

    dev = qdev_new(TYPE_NPCM400_SOC);
    soc = NPCM400_SOC(dev);
    object_property_add_child(OBJECT(machine), "soc", OBJECT(dev));
    qdev_connect_clock_in(dev, "sysclk", sysclk);

    /* Optional external MTD image used by the attached SPI NOR model. */
    dinfo = drive_get(IF_MTD, 0, 0);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    /* Keep SPIM DMM shadow initialized from IF_MTD contents. */
    if (!npcm400_preload_spim_shadow_from_drive(&soc->spim, dinfo)) {
        error_report("npcm400: cannot preload SPIM shadow from mtd drive");
        exit(1);
    }

    /*
     * Match the npcm8xx_connect_flash architecture when an MTD image is
     * provided: attach a real SPI NOR device on SPIM's SSI bus.
     *
     * Without -drive if=mtd we keep the legacy in-controller fallback model
     * so signed-image -kernel flows continue to boot.
     */
    if (dinfo) {
        npcm400_connect_flash(&soc->spim, "w25q80bl", dinfo);
    }

    if (machine->kernel_filename) {
        error_report("npcm400: -kernel is not supported; use -bios <npcm400_bootrom.bin> with -drive if=mtd");
        exit(1);
    }

    bootrom_loaded = npcm400_load_bootrom(machine, &soc->spim);
    if (!bootrom_loaded) {
        error_report("npcm400: boot aborted because vbootrom is required");
        exit(1);
    }

    armv7m_load_kernel(ARM_CPU(first_cpu), NULL, 0, 0);
}

static void npcm400_machine_init(MachineClass *mc)
{
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-m4"),
        NULL
    };

    mc->desc = "Nuvoton NPCM400 EVB (Cortex-M4)";
    mc->init = npcm400_evb_init;
    mc->valid_cpu_types = valid_cpu_types;
    mc->default_ram_size = 0;
}

DEFINE_MACHINE_ARM("npcm400-evb", npcm400_machine_init)
