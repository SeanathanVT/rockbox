/*
 * Innioasis Y1 — USB stubs.
 *
 * Rockbox sees the Y1 as USB-less for now.  USB-C audio out is a Phase 6
 * stretch goal (kernel rebuild with snd-usb-audio); mass-storage UMS is
 * not planned — the player is loaded by mounting the eMMC user partition
 * from another host.
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
