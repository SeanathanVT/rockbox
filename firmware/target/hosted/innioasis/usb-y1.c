/*
 * Innioasis Y1 — USB mass storage (microSD RW over USB).
 *
 * The stock MT6572 kernel ships the android_usb composite gadget with the
 * mass_storage function built in (proven by the stock init.usb.rc); we drive it
 * from /sys/class/android_usb/android0/ — no kernel rebuild.  Modelled on the
 * hosted-Linux FiiO target (firmware/target/hosted/fiio/usb-fiio.c).  Full design
 * notes, the concurrency model, and the on-device checks are in the y1-platform
 * repo: docs/usb-storage.md.
 *
 * Safety: USB mass storage gives the host raw block access, so we must fully
 * unmount the SD before exposing it, and never expose it while still mounted
 * (concurrent FAT + raw writes corrupt the card).  The interlocks below enforce
 * that.  On disconnect we remount the SD and continue -- the same model as the
 * Hiby hosted target (usb-hiby.c) and the iPod: the generic USB framework
 * reloads tagcache, and the Y1 has no dircache to rebuild.
 */

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/mount.h>
#include "config.h"
#include "disk.h"
#include "usb.h"
#include "sysfs.h"
#include "power.h"
#include "logf.h"

#ifdef HAVE_MULTIDRIVE
/* defined in firmware/target/hosted/filesystem-app.c */
void cleanup_rbhome(void);
void startup_rbhome(void);
#endif

#define ANDROID_USB    "/sys/class/android_usb/android0"
#define LUN_FILE       ANDROID_USB "/f_mass_storage/lun/file"
/* Some hosts (notably car head units) only index storage that advertises itself
 * as a *removable* disk and ignore a fixed disk -- the Kia GEN5W manual lists
 * "USB devices that are not recognized as removable disks" as unsupported. Mark
 * the LUN removable before enabling. Best-effort: a no-op on kernels that don't
 * expose the attribute. */
#define LUN_REMOVABLE  ANDROID_USB "/f_mass_storage/lun/removable"
/* Whole-disk node so the host sees the partition table; init also creates the
 * partition node we mount.  init mounts mmcblk1p1 at MULTIDRIVE_DIR. */
#define SD_DISK_DEV    "/dev/block/mmcblk1"
#define SD_PART_DEV    "/dev/block/mmcblk1p1"
#define SD_MOUNTPOINT  MULTIDRIVE_DIR   /* "/storage/sdcard1" */

static const char sysfs_usb_online[] = "/sys/class/power_supply/usb/online";

/* The android_usb gadget is built into the stock kernel; if it's ever absent
 * (a kernel without CONFIG_USB_G_ANDROID), keep the whole feature inert so we
 * never unmount the SD for nothing. */
static bool gadget_present = false;
/* Set when we actually unmounted the SD, so we only expose it / reboot when we
 * truly took it away from Rockbox. */
static bool sd_released = false;

void usb_init_device(void)
{
    gadget_present = (access(ANDROID_USB "/enable", F_OK) == 0);
}

int usb_detect(void)
{
    if (!gadget_present)
        return USB_EXTRACTED;

    int present = 0;
    sysfs_get_int(sysfs_usb_online, &present);
    return present ? USB_INSERTED : USB_EXTRACTED;
}

void usb_enable(bool on)
{
    if (!gadget_present)
        return;

    if (on)
    {
        /* Interlock: only hand the block device to the host if disk_unmount_all
         * actually released the SD.  If the unmount failed (open files), decline
         * rather than risk concurrent FAT + raw access. */
        if (!sd_released)
        {
            logf("usb-y1: SD not released, not exposing mass storage");
            return;
        }

        sysfs_set_int(ANDROID_USB "/enable", 0);
        sysfs_set_string(ANDROID_USB "/idVendor", USB_VID_STR);
        sysfs_set_string(ANDROID_USB "/idProduct", USB_PID_STR);
        /* USB device descriptor strings (lsusb / OS device name). The SCSI
         * INQUIRY model ("Linux File-CD Gadget") is baked into the kernel's
         * f_mass_storage and isn't overridable here. */
        sysfs_set_string(ANDROID_USB "/iManufacturer", "Innioasis");
        sysfs_set_string(ANDROID_USB "/iProduct", "Y1");
        sysfs_set_string(ANDROID_USB "/functions", "mass_storage");
        sysfs_set_int(LUN_REMOVABLE, 1);
        sysfs_set_string(LUN_FILE, SD_DISK_DEV);
        sysfs_set_int(ANDROID_USB "/enable", 1);
    }
    else
    {
        sysfs_set_int(ANDROID_USB "/enable", 0);
        sysfs_set_string(LUN_FILE, "");
    }
}

/* Called by the USB thread once all threads have released storage. */
int disk_unmount_all(void)
{
#ifdef HAVE_MULTIDRIVE
    cleanup_rbhome();
#endif
    sync();
    /* Real unmount (not MNT_DETACH — a lazy unmount would leave the kernel mount
     * live under the host's raw writes).  Succeeds only once the SD's open fds
     * (config/tagcache under RB_WRITABLE_DIR) are closed by the
     * SYS_EVENT_USB_INSERTED handshake; if it can't, usb_enable() declines. */
    sd_released = (umount(SD_MOUNTPOINT) == 0);
    if (!sd_released)
        logf("usb-y1: umount %s failed (busy)", SD_MOUNTPOINT);
    return 1;
}

/* Called by the USB thread after the host disconnects. */
int disk_mount_all(void)
{
    /* Remount the SD and carry on — no reboot.  The host had raw block access;
     * a fresh mount re-reads the FAT so the file browser sees its changes, and
     * the generic USB framework reloads tagcache (the Y1 has no dircache).
     * Mirrors usb-hiby.c / the iPod's re-read-on-disconnect behaviour. */
    if (sd_released)
    {
        if (mount(SD_PART_DEV, SD_MOUNTPOINT, "vfat", MS_NOATIME, "utf8") != 0
            && errno != EBUSY)
            logf("usb-y1: remount %s failed: %d", SD_MOUNTPOINT, errno);
        sd_released = false;
    }
#ifdef HAVE_MULTIDRIVE
    startup_rbhome();
#endif
    return 1;
}
