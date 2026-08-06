/*
 * Nuvoton NPCM400 SoC
 *
 * Minimal NPCM400 SoC model with Cortex-M4, flash/sram map, and UARTs.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "hw/arm/npcm400_soc.h"
#include "hw/core/qdev-clock.h"
#include "hw/misc/unimp.h"

static const hwaddr npcm400_uart_addr[NPCM400_NUM_UARTS] = {
    0x400c4000,
    0x400c4200,
};

static const int npcm400_uart_irq[NPCM400_NUM_UARTS] = {
    23,
    76,
};

#define NPCM400_RESET_STATUS_ADDR 0x100c7f84

static uint64_t npcm400_reset_status_read(void *opaque, hwaddr addr,
                                          unsigned int size)
{
    return 0x01; /* VCC_POWERUP */
}

static void npcm400_reset_status_write(void *opaque, hwaddr addr,
                                       uint64_t value, unsigned int size)
{
    /* Read-only boot reason byte. */
}

static const MemoryRegionOps npcm400_reset_status_ops = {
    .read  = npcm400_reset_status_read,
    .write = npcm400_reset_status_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static bool npcm400_map_stub_mmio(NPCM400State *s, DeviceState *dev_soc,
                                  MemoryRegion *system_memory,
                                  const char *name,
                                  hwaddr base, uint64_t size,
                                  Error **errp)
{
    Error *err = NULL;
    MemoryRegion *mr;

    if (s->stub_mmio_count >= NPCM400_NUM_STUB_MMIO) {
        error_setg(errp, "npcm400: too many stub MMIO windows");
        return false;
    }

    mr = &s->stub_mmio[s->stub_mmio_count++];
    memory_region_init_ram(mr, OBJECT(dev_soc), name, size, &err);
    if (err) {
        error_propagate(errp, err);
        return false;
    }

    memory_region_add_subregion(system_memory, base, mr);
    return true;
}

static void npcm400_soc_initfn(Object *obj)
{
    NPCM400State *s = NPCM400_SOC(obj);
    int i;

    object_initialize_child(obj, "armv7m", &s->armv7m, TYPE_ARMV7M);

    for (i = 0; i < NPCM400_NUM_UARTS; i++) {
        object_initialize_child(obj, "uart[*]", &s->uart[i],
                                TYPE_NPCM4XX_UART);
    }

    object_initialize_child(obj, "pdma", &s->pdma, TYPE_NPCM400_PDMA);
    object_initialize_child(obj, "fiu",  &s->fiu,  TYPE_NPCM400_FIU);
    object_initialize_child(obj, "spim", &s->spim, TYPE_NPCM400_SPIM);

    s->sysclk = qdev_init_clock_in(DEVICE(s), "sysclk", NULL, NULL, 0);
    s->stub_mmio_count = 0;
}

static void npcm400_soc_realize(DeviceState *dev_soc, Error **errp)
{
    NPCM400State *s = NPCM400_SOC(dev_soc);
    MemoryRegion *system_memory = get_system_memory();
    DeviceState *armv7m;
    Error *err = NULL;
    int i;

    if (!clock_has_source(s->sysclk)) {
        error_setg(errp, "sysclk clock must be wired up by the board code");
        return;
    }

    memory_region_init_ram(&s->sram, OBJECT(dev_soc), "NPCM400.sram",
                           NPCM400_SRAM_SIZE, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    memory_region_add_subregion(system_memory, NPCM400_SRAM_BASE, &s->sram);

    /* PDMA */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->pdma), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->pdma), 0, 0x40015000);

    memory_region_init_io(&s->reset_status, OBJECT(dev_soc),
                          &npcm400_reset_status_ops, s,
                          "npcm400.reset_status", 1);
    memory_region_add_subregion(system_memory, NPCM400_RESET_STATUS_ADDR,
                                &s->reset_status);

    /* FIU: MMIO 0 = register window, MMIO 1-3 = XIP decode windows */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->fiu), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->fiu), 0, 0x40020000);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->fiu), 1, 0x60000000);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->fiu), 2, 0x70000000);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->fiu), 3, 0x80000000);

    /*
     * SPIM: MMIO 0 = register window (0x40017000),
     *       MMIO 1 = DMM flash window (NPCM400_FLASH_BASE = 0x80000).
     *
     * The DMM window replaces the old static ROM: the SPIM flash array is
     * the authoritative copy of flash content, loaded from a drive or from
     * the kernel image by the machine code.
     *
     * flash_alias mirrors the DMM window at 0x00000000 so the Cortex-M
     * reset fetch (MSP from [0x0], PC from [0x4]) sees the flash vector table.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->spim), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->spim), 0, 0x40017000);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->spim), 1, NPCM400_FLASH_BASE);

    memory_region_init_alias(&s->flash_alias, OBJECT(dev_soc),
                             "NPCM400.flash.alias",
                             sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->spim), 1),
                             0, NPCM400_FLASH_SIZE);
    memory_region_add_subregion(system_memory, 0, &s->flash_alias);

    armv7m = DEVICE(&s->armv7m);
    qdev_prop_set_uint32(armv7m, "num-irq", 96);
    qdev_prop_set_uint8(armv7m, "num-prio-bits", 3);
    qdev_prop_set_string(armv7m, "cpu-type", ARM_CPU_TYPE_NAME("cortex-m4"));
    qdev_prop_set_bit(armv7m, "enable-bitband", true);
    qdev_connect_clock_in(armv7m, "cpuclk", s->sysclk);
    object_property_set_link(OBJECT(&s->armv7m), "memory",
                             OBJECT(system_memory), &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->armv7m), errp)) {
        return;
    }

    for (i = 0; i < NPCM400_NUM_UARTS; i++) {
        DeviceState *dev = DEVICE(&s->uart[i]);
        SysBusDevice *busdev = SYS_BUS_DEVICE(dev);

        qdev_prop_set_chr(dev, "chardev", serial_hd(i));
        if (!sysbus_realize(busdev, errp)) {
            return;
        }

        sysbus_mmio_map(busdev, 0, npcm400_uart_addr[i]);
        sysbus_connect_irq(busdev, 0,
                           qdev_get_gpio_in(armv7m, npcm400_uart_irq[i]));
    }

    if (!npcm400_map_stub_mmio(s, dev_soc, system_memory,
                               "npcm400.shm", 0x40010000, 0x2000, errp) ||
            !npcm400_map_stub_mmio(s, dev_soc, system_memory,
                                   "npcm400.smbus", 0x40004000, 0x4000, errp) ||
        !npcm400_map_stub_mmio(s, dev_soc, system_memory,
                               "npcm400.mdc", 0x4000c000, 0x1000, errp) ||
        !npcm400_map_stub_mmio(s, dev_soc, system_memory,
                               "npcm400.pcc", 0x4000d000, 0x2000, errp) ||
        !npcm400_map_stub_mmio(s, dev_soc, system_memory,
                               "npcm400.cdcg", 0x400b5000, 0x1000, errp) ||
        !npcm400_map_stub_mmio(s, dev_soc, system_memory,
                               "npcm400.hfcg", 0x4000e000, 0x2000, errp) ||
        !npcm400_map_stub_mmio(s, dev_soc, system_memory,
                               "npcm400.scfg", 0x400c3000, 0x1000, errp) ||
        !npcm400_map_stub_mmio(s, dev_soc, system_memory,
                               "npcm400.glue", 0x400a5000, 0x2000, errp) ||
        !npcm400_map_stub_mmio(s, dev_soc, system_memory,
                               "npcm400.miwu0", 0x400bb000, 0x2000, errp) ||
        !npcm400_map_stub_mmio(s, dev_soc, system_memory,
                               "npcm400.miwu1", 0x400bd000, 0x2000, errp) ||
        !npcm400_map_stub_mmio(s, dev_soc, system_memory,
                               "npcm400.miwu2", 0x400bf000, 0x2000, errp) ||
        !npcm400_map_stub_mmio(s, dev_soc, system_memory,
                               "npcm400.itim", 0x400b0000, 0x2000, errp) ||
        !npcm400_map_stub_mmio(s, dev_soc, system_memory,
                               "npcm400.watchdog", 0x400d8000, 0x1000, errp) ||
        !npcm400_map_stub_mmio(s, dev_soc, system_memory,
                       "npcm400.misc_d1100", 0x400d1100, 0x1000, errp) ||
        !npcm400_map_stub_mmio(s, dev_soc, system_memory,
                               "npcm400.spip", 0x40016000, 0x1000, errp) ||
        !npcm400_map_stub_mmio(s, dev_soc, system_memory,
                       "npcm400.host_sub0", 0x400c1000, 0x2000, errp) ||
        !npcm400_map_stub_mmio(s, dev_soc, system_memory,
                       "npcm400.host_sub1", 0x400c9000, 0x2000, errp) ||
        !npcm400_map_stub_mmio(s, dev_soc, system_memory,
                       "npcm400.host_sub2", 0x400cb000, 0x2000, errp) ||
        !npcm400_map_stub_mmio(s, dev_soc, system_memory,
                       "npcm400.host_sub3", 0x400cd000, 0x2000, errp) ||
        !npcm400_map_stub_mmio(s, dev_soc, system_memory,
                       "npcm400.host_sub4", 0x400cf000, 0x1000, errp) ||
        !npcm400_map_stub_mmio(s, dev_soc, system_memory,
                               "npcm400.espi", 0x4000a000, 0x2000, errp) ||
        !npcm400_map_stub_mmio(s, dev_soc, system_memory,
                               "npcm400.tach1", 0x400e1000, 0x2000, errp) ||
        !npcm400_map_stub_mmio(s, dev_soc, system_memory,
                               "npcm400.usbd", 0x4001b000, 0x1000, errp) ||
        !npcm400_map_stub_mmio(s, dev_soc, system_memory,
                               "npcm400.gpio", 0x40080000, 0x20000, errp)) {
        return;
    }
}

static void npcm400_soc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = npcm400_soc_realize;
}

static const TypeInfo npcm400_soc_info = {
    .name          = TYPE_NPCM400_SOC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NPCM400State),
    .instance_init = npcm400_soc_initfn,
    .class_init    = npcm400_soc_class_init,
};

static void npcm400_soc_types(void)
{
    type_register_static(&npcm400_soc_info);
}

type_init(npcm400_soc_types)
