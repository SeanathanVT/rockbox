/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2026 by Sean Halpin
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

/*
 * Innioasis Y1 — button_map() for hosted/button-devinput.c
 *
 * Click-wheel mapping verified by evdev capture.
 * See button-target.h for the physical-to-keycode table.
 */

#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include "button.h"
#include "button-target.h"

int button_map(int keycode)
{
    switch (keycode)
    {
        /* Cardinal buttons */
        case KEY_BACK:           return BUTTON_MENU;    /* top */
        case KEY_REPLY:          return BUTTON_SELECT;  /* center, on event0 (mtk-kpd) */
        case KEY_PLAYPAUSE:      return BUTTON_PLAY;    /* bottom + headset PLAY */
        case KEY_PREVIOUSSONG:   return BUTTON_LEFT;    /* left + headset PREV */
        case KEY_NEXTSONG:       return BUTTON_RIGHT;   /* right + headset NEXT */

        /* Scroll wheel: one tick per paired DOWN/UP. button-devinput.c's
         * HAVE_SCROLLWHEEL path turns each press+release pair into one
         * BUTTON_SCROLL_* queue event. */
        case KEY_LEFT:           return BUTTON_SCROLL_BACK;
        case KEY_RIGHT:          return BUTTON_SCROLL_FWD;

        default:                 return 0;
    }
}

bool headphones_inserted(void)
{
    /* /sys/class/switch/h2w/state: "0" = unplugged, "1" = plugged.
     * Confirmed present on stock Y1. */
    int fd = open("/sys/class/switch/h2w/state", O_RDONLY);
    if (fd < 0) return false;
    char c = '0';
    read(fd, &c, 1);
    close(fd);
    return c != '0';
}
