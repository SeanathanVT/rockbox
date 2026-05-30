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

#ifndef __LCD_TARGET_H__
#define __LCD_TARGET_H__

/* lcd-linuxfb.c provides lcd_update / lcd_update_rect; suppress the generic
 * versions in lcd-memframe.c. */
#define LCD_OPTIMIZED_UPDATE
#define LCD_OPTIMIZED_UPDATE_RECT

extern fb_data *framebuffer; /* defined in lcd-linuxfb.c */
#define LCD_FRAMEBUF_ADDR(col, row) (framebuffer + (row) * LCD_WIDTH + (col))

/* defined in lcd-memframe.c, called by lcd-linuxfb.c's lcd_enable path — every
 * hosted lcd-target.h sharing lcd-memframe.c declares this. */
extern void lcd_set_active(bool active);

#endif /* __LCD_TARGET_H__ */
