/*
 * Innioasis Y1 — USB stubs (mass storage planned).
 *
 * Rockbox sees the Y1 as USB-less for now.  Mass-storage UMS (SD-card RW over
 * USB) is feasible on the stock kernel via the android_usb gadget — no kernel
 * rebuild — modelled on usb-fiio.c; see docs/usb-storage.md in the y1-platform
 * repo for the implementation plan.  USB-C audio out (host mode) is a separate
 * Phase 6 stretch goal (kernel rebuild with snd-usb-audio).
 */

#include "config.h"
#include "usb.h"
#include "disk.h"

int usb_detect(void)
{
    return USB_EXTRACTED;
}

void usb_init_device(void)
{
}

void usb_enable(bool on)
{
    (void)on;
}

int disk_mount_all(void)
{
    return 1;
}

int disk_unmount_all(void)
{
    return 1;
}
