/*
 *  Panasonic Tablet Button driver
 *  (C) 2012 Heiher <admin@heiher.info>
 *  Modified for FZ-G1 MK4 support
 *
 *  derived from panasonic-laptop.c, Copyright (C) 2002-2004 John Belmonte
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  publicshed by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/ctype.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/acpi.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>


MODULE_AUTHOR("Heiher");
MODULE_DESCRIPTION("ACPI Tablet Button driver for Panasonic CF-18/19/FZ-G1 laptops");
MODULE_LICENSE("GPL");

/* Define ACPI PATHs */
#define METHOD_HBTN_QUERY   "HINF"
#define HBTN_NOTIFY         0x80

#define ACPI_PCC_DRIVER_NAME    "Panasonic Tablet Button Support"
#define ACPI_PCC_DEVICE_NAME    "TabletButton"
#define ACPI_PCC_CLASS          "pcc"

#define ACPI_PCC_INPUT_PHYS    "panasonic/hbtn0"

static int acpi_pcc_hbtn_add(struct acpi_device *device);
static void acpi_pcc_hbtn_notify(struct acpi_device *device, u32 event);

static const struct acpi_device_id pcc_device_ids[] = {
    { "MAT001F", 0},
    { "MAT0020", 0},
    { "MAT0037", 0},  /* FZ-G1 MK4 */
    { "", 0},
};
MODULE_DEVICE_TABLE(acpi, pcc_device_ids);

static struct acpi_driver acpi_pcc_driver = {
    .name =     ACPI_PCC_DRIVER_NAME,
    .class =    ACPI_PCC_CLASS,
    .ids =      pcc_device_ids,
    .ops =      {
                .add =       acpi_pcc_hbtn_add,
                .notify =    acpi_pcc_hbtn_notify,
            },
};

static const struct key_entry panasonic_keymap[] = {
    { KE_KEY, 0x0, { KEY_RESERVED } },
    /* CF-18/19 buttons */
    { KE_KEY, 0x4, { KEY_SCREENLOCK } },
    { KE_KEY, 0x6, { KEY_MSDOS } },
    { KE_KEY, 0x8, { KEY_ESC } },
    { KE_KEY, 0xA, { KEY_MENU } },
    /* FZ-G1 MK4 buttons (codes 0x36, 0x38, 0x42) */
    { KE_KEY, 0x36, { KEY_PROG1 } },     /* A1 button */
    { KE_KEY, 0x38, { KEY_PROG2 } },     /* A2 button */
    { KE_KEY, 0x42, { KEY_LEFTMETA } },  /* Windows button */
    { KE_END, 0 }
};

struct pcc_acpi {
    acpi_handle        handle;
    struct acpi_device    *device;
    struct input_dev    *input_dev;
};

/* hbtn input device driver */

static void acpi_pcc_generate_keyinput(struct pcc_acpi *pcc)
{
    struct input_dev *hotk_input_dev = pcc->input_dev;
    int rc;
    unsigned long long result;
    struct key_entry *ke = NULL;
    unsigned int scancode;
    int pressed;

    rc = acpi_evaluate_integer(pcc->handle, METHOD_HBTN_QUERY,
                   NULL, &result);
    if (!ACPI_SUCCESS(rc)) {
        pr_err("panasonic-hbtn: error getting hbtn status\n");
        return;
    }

    acpi_bus_generate_netlink_event(pcc->device->pnp.device_class,
                    dev_name(&pcc->device->dev), HBTN_NOTIFY, result);

    /* Bit 0 is press/release: 0=press, 1=release */
    pressed = !(result & 0x1);
    scancode = result & ~1UL;

    pr_debug("panasonic-hbtn: raw=0x%llx scancode=0x%x pressed=%d\n",
             result, scancode, pressed);

    ke = sparse_keymap_entry_from_scancode(hotk_input_dev, scancode);
    if (!ke) {
        pr_warn("panasonic-hbtn: Unknown button event: 0x%llx (scancode 0x%x)\n",
                result, scancode);
        return;
    }

    sparse_keymap_report_entry(hotk_input_dev, ke, pressed, false);
}

static void acpi_pcc_hbtn_notify(struct acpi_device *device, u32 event)
{
    struct pcc_acpi *pcc = acpi_driver_data(device);

    switch (event) {
    case HBTN_NOTIFY:
        acpi_pcc_generate_keyinput(pcc);
        break;
    default:
        break;
    }
}

static int acpi_pcc_init_input(struct pcc_acpi *pcc)
{
    struct input_dev *input_dev;
    int error;

    input_dev = input_allocate_device();
    if (!input_dev) {
        pr_err("panasonic-hbtn: Couldn't allocate input device\n");
        return -ENOMEM;
    }

    input_dev->name = ACPI_PCC_DRIVER_NAME;
    input_dev->phys = ACPI_PCC_INPUT_PHYS;
    input_dev->id.bustype = BUS_HOST;
    input_dev->id.vendor = 0x0001;
    input_dev->id.product = 0x0001;
    input_dev->id.version = 0x0100;

    error = sparse_keymap_setup(input_dev, panasonic_keymap, NULL);
    if (error) {
        pr_err("panasonic-hbtn: Unable to setup keymap\n");
        goto err_free_dev;
    }

    error = input_register_device(input_dev);
    if (error) {
        pr_err("panasonic-hbtn: Unable to register input device\n");
        goto err_free_dev;
    }

    pcc->input_dev = input_dev;
    return 0;

 err_free_dev:
    input_free_device(input_dev);
    return error;
}

static void acpi_pcc_destroy_input(struct pcc_acpi *pcc)
{
    input_unregister_device(pcc->input_dev);
}

static int acpi_pcc_hbtn_add(struct acpi_device *device)
{
    struct pcc_acpi *pcc;
    int result;

    if (!device)
        return -EINVAL;

    pcc = kzalloc(sizeof(struct pcc_acpi), GFP_KERNEL);
    if (!pcc) {
        pr_err("panasonic-hbtn: Couldn't allocate memory\n");
        return -ENOMEM;
    }

    pcc->device = device;
    pcc->handle = device->handle;
    device->driver_data = pcc;
    strcpy(acpi_device_name(device), ACPI_PCC_DEVICE_NAME);
    strcpy(acpi_device_class(device), ACPI_PCC_CLASS);

    result = acpi_pcc_init_input(pcc);
    if (result) {
        pr_err("panasonic-hbtn: Error installing keyinput handler\n");
        kfree(pcc);
        return result;
    }

    pr_info("panasonic-hbtn: Tablet button driver loaded for %s\n",
            acpi_device_hid(device));

    return 0;
}

static void acpi_pcc_hbtn_remove(struct acpi_device *device)
{
    struct pcc_acpi *pcc = acpi_driver_data(device);

    if (!device || !pcc)
        return;

    acpi_pcc_destroy_input(pcc);
    kfree(pcc);
}

static int __init acpi_pcc_init(void)
{
    int result;

    if (acpi_disabled)
        return -ENODEV;

    result = acpi_bus_register_driver(&acpi_pcc_driver);
    if (result < 0) {
        pr_err("panasonic-hbtn: Error registering driver\n");
        return -ENODEV;
    }

    return 0;
}

static void __exit acpi_pcc_exit(void)
{
    acpi_bus_unregister_driver(&acpi_pcc_driver);
}

module_init(acpi_pcc_init);
module_exit(acpi_pcc_exit);
