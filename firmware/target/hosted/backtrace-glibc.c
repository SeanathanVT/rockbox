/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (c) 2017 Amaury Pouly
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
#include "config.h"
#include "system.h"
#include "lcd.h"
#include <execinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef DEBUG
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#endif

#ifdef DEBUG
/* Durable crash dump to fd 2.  init redirects stderr to rockbox.err on an
 * -o sync mount and glibc keeps stderr unbuffered, so each write(2) survives
 * the post-crash power-cycle.  glibc backtrace() needs unwind tables Rockbox
 * doesn't emit on ARM, so we instead dump the raw memory map (resolves any
 * address to library+offset offline) and a stack scan (recovers caller return
 * addresses for addr2line).  Raw syscalls only -- no malloc/stdio in a crash. */
static void bt_puts(const char *s) { write(STDERR_FILENO, s, strlen(s)); }

static void bt_kv(const char *k, unsigned long v)
{
    char b[32];
    int n = snprintf(b, sizeof b, "%s%08lx\n", k, v);
    if (n > 0) write(STDERR_FILENO, b, n);
}

static void bt_dump(int pc, int sp)
{
    bt_puts("\n=== CRASH DUMP (pc/sp + maps + stack scan) ===\n");
    bt_kv("pc=", (unsigned long)(unsigned)pc);
    bt_kv("sp=", (unsigned long)(unsigned)sp);

    bt_puts("--- /proc/self/maps ---\n");
    int mf = open("/proc/self/maps", O_RDONLY);
    if (mf >= 0) {
        char buf[512]; ssize_t n;
        while ((n = read(mf, buf, sizeof buf)) > 0)
            write(STDERR_FILENO, buf, (size_t)n);
        close(mf);
    }

    /* Rockbox thread stacks live inside its own mapped buffer, so scanning a
     * few hundred words up from sp stays in mapped memory (no fault). */
    bt_puts("--- stack scan ---\n");
    if (sp) {
        unsigned long *p = (unsigned long *)(uintptr_t)(unsigned)sp;
        for (int i = 0; i < 256; i++)
            bt_kv("", p[i]);
    }
    bt_puts("=== END CRASH DUMP ===\n");
}
#endif /* DEBUG */

/* backtrace from the call-site of this function */
void rb_backtrace(int pc, int sp, unsigned *line)
{
#ifdef DEBUG
    bt_dump(pc, sp);
#else
    /* ignore SP and PC */
    (void) pc;
    (void) sp;
#endif

    /* backtrace */
    #define BT_BUF_SIZE 100
    void *buffer[BT_BUF_SIZE];
    int count = backtrace(buffer, BT_BUF_SIZE);
    /* print symbols to stdout for debug */
    fprintf(stdout, "backtrace:\n");
    fflush(stdout);
    backtrace_symbols_fd(buffer, count, STDOUT_FILENO);
    /* print on screen */
    char **strings;
    strings = backtrace_symbols(buffer, count);
    if(strings == NULL)
    {
        perror("backtrace_symbols");
        return;
    }

    for(int i = 0; i < count; i++)
    {
        lcd_putsf(0, (*line)++, "  %s", strings[i]);
        lcd_update();
    }

    free(strings);
}
