/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2017 Marcin Bukat
 * Copyright (C) 2016 Amaury Pouly
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <ucontext.h>
#include <backtrace.h>

#include "system.h"
#include "mv.h"
#include "font.h"
#include "power.h"
#include "button.h"
#include "button-devinput.h"
#include "backlight-target.h"
#include "lcd.h"
#include "filesystem-hosted.h"
#include "logf.h"

#if defined(DEBUG) && defined(INNIOASIS_Y1)
#include <stdio.h>
#include <pthread.h>
#include <dirent.h>
#include <fcntl.h>
#endif

/* to make thread-internal.h happy */
uintptr_t *stackbegin;
uintptr_t *stackend;

/* forward-declare */
bool os_file_exists(const char *ospath);

#if defined(DEBUG) && defined(INNIOASIS_Y1)
/* Hang watchdog (DEBUG only).  A crash raises a signal the handler can dump,
 * but a *hang* raises nothing -- so this real OS thread, independent of
 * Rockbox's cooperative scheduler, periodically dumps every thread's kernel
 * state to stderr (-> rockbox.err).  /proc/self/task/<tid>/{comm,wchan,syscall}
 * names what each thread is blocked in; the last dump before the log stops
 * points at the stuck thread (a futex/lock, a read/write, or a busy spin). */
static void wd_emit(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { write(STDERR_FILENO, "?", 1); return; }
    char buf[192];
    ssize_t n = read(fd, buf, sizeof buf);
    close(fd);
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' ')) n--;
    if (n > 0) write(STDERR_FILENO, buf, (size_t)n);
}

static void *y1_watchdog(void *arg)
{
    (void)arg;
    for (;;)
    {
        sleep(4);
        DIR *d = opendir("/proc/self/task");
        if (!d) continue;
        write(STDERR_FILENO, "--- watchdog ---\n", 17);
        struct dirent *e;
        while ((e = readdir(d)) != NULL)
        {
            if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
            char p[80];
            write(STDERR_FILENO, "  tid ", 6);
            write(STDERR_FILENO, e->d_name, strlen(e->d_name));
            write(STDERR_FILENO, " comm=", 6);
            snprintf(p, sizeof p, "/proc/self/task/%s/comm", e->d_name);    wd_emit(p);
            write(STDERR_FILENO, " wchan=", 7);
            snprintf(p, sizeof p, "/proc/self/task/%s/wchan", e->d_name);   wd_emit(p);
            write(STDERR_FILENO, " syscall=", 9);
            snprintf(p, sizeof p, "/proc/self/task/%s/syscall", e->d_name); wd_emit(p);
            write(STDERR_FILENO, "\n", 1);
        }
        closedir(d);
    }
    return NULL;
}
#endif

static void sig_handler(int sig, siginfo_t *siginfo, void *context)
{
    /* safe guard variable - we call backtrace() only on first
     * UIE call. This prevent endless loop if backtrace() touches
     * memory regions which cause abort
     */
    static bool triggered = false;

    lcd_set_backdrop(NULL);
    lcd_set_drawinfo(DRMODE_SOLID, LCD_BLACK, LCD_WHITE);
    unsigned line = 0;

    lcd_setfont(FONT_SYSFIXED);
    lcd_set_viewport(NULL);
    lcd_clear_display();

    /* get context info */
    ucontext_t *uc = (ucontext_t *)context;
#if defined(__arm__)
    unsigned long pc = uc->uc_mcontext.arm_pc;
    unsigned long sp = uc->uc_mcontext.arm_sp;
#else
    unsigned long pc = uc->uc_mcontext.pc;
    unsigned long sp = uc->uc_mcontext.gregs[29];
#endif

    lcd_putsf(0, line++, "%s at %08lx", strsignal(sig), pc);

    if(sig == SIGILL || sig == SIGFPE || sig == SIGSEGV || sig == SIGBUS || sig == SIGTRAP) {
        lcd_putsf(0, line++, "address %p", siginfo->si_addr);
    }

    if(!triggered)
    {
        triggered = true;
        rb_backtrace(pc, sp, &line);
    }

#ifdef ROCKBOX_HAS_LOGF
    lcd_putsf(0, line++, "logf:");
    logf_panic_dump(&line);
#endif

    lcd_update();

    system_exception_wait(); /* If this returns, try to reboot */
    system_reboot();
    while (1) {
        // Make sure we're not throttling the cpu
        usleep(1000);
    }
}

void power_off(void)
{
    backlight_hw_off();
    button_close_device();
    sync();
    system("/sbin/poweroff");
    while (1) {
        // Make sure we're not throttling the cpu
        usleep(1000);
    }
}

void system_init(void)
{
    int *s;
    /* fake stack, to make thread-internal.h happy */
    stackbegin = stackend = (uintptr_t*)&s;
   /* catch some signals for easier debugging */
    struct sigaction sa;
    sigfillset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = &sig_handler;
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGPIPE, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

#if defined(DEBUG) && defined(INNIOASIS_Y1)
    pthread_t wd;
    pthread_create(&wd, NULL, y1_watchdog, NULL);
#endif
}

void system_reboot(void)
{
    backlight_hw_off();
    system("/sbin/reboot");
    while (1) {
        // Make sure we're not throttling the cpu
        usleep(1000);
    }
}

void system_exception_wait(void)
{
    backlight_hw_on();
    backlight_hw_brightness(DEFAULT_BRIGHTNESS_SETTING);
    /* wait until button press and release */
#ifdef HAVE_BUTTON_DATA
    int bdata;
#define BDATA &bdata
#else
#define BDATA
#endif
    while(button_read_device(BDATA) != 0) {}
    while(button_read_device(BDATA) == 0) {}
    while(button_read_device(BDATA) != 0) {}
    while(button_read_device(BDATA) == 0) {}
}

bool hostfs_removable(IF_MD_NONVOID(int drive))
{
#ifdef HAVE_MULTIDRIVE
    if (drive > 0)
        return true;
    else
#endif
#ifdef HAVE_HOTSWAP_STORAGE_AS_MAIN
        return true;
#else
        return false; /* internal: always present */
#endif
}

bool hostfs_present(IF_MD_NONVOID(int drive))
{
#ifdef HAVE_MULTIDRIVE
    if (drive > 0)
#if defined(MULTIDRIVE_DEV)
        return os_file_exists(MULTIDRIVE_DEV);
#else
        return true; // FIXME?
#endif
    else
#endif
#ifdef HAVE_HOTSWAP_STORAGE_AS_MAIN
        return os_file_exists(ROOTDRIVE_DEV);
#else
        return true; /* internal: always present */
#endif
}

#ifdef HAVE_MULTIDRIVE
int volume_drive(int drive)
{
    return drive;
}
#endif /* HAVE_MULTIDRIVE */

#ifdef CONFIG_STORAGE_MULTI
int hostfs_driver_type(int drive)
{
#if (CONFIG_STORAGE & STORAGE_USB)
    return drive > 0 ? STORAGE_USB_NUM : STORAGE_HOSTFS_NUM;
#else
    return drive > 0 ? STORAGE_SD_NUM : STORAGE_HOSTFS_NUM;
#endif
}
#endif /* CONFIG_STORAGE_MULTI */

int hostfs_init(void)
{
    return 0;
}

int hostfs_flush(void)
{
    sync();
    return 0;
}

#ifdef HAVE_HOTSWAP
bool volume_removable(int volume)
{
    /* don't support more than one partition yet, so volume == drive */
    return hostfs_removable(volume);
}

bool volume_present(int volume)
{
    /* don't support more than one partition yet, so volume == drive */
    return hostfs_present(volume);
}
#endif

int volume_partition(int volume)
{
    (void)volume;
    /* Hosted only implement a single parition per "drive" */
    return 0;
}
