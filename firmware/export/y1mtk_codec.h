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
 * Innioasis Y1 — audiohw definitions for the MTK /dev/eac driver
 * (target/hosted/innioasis/pcm-y1mtk.c).
 *
 * HW volume would be SET_ANAAFE_REG writes to MT6323 codec registers, but the
 * gain register set is undocumented in the BSP header, so volume is software
 * only (HAVE_SW_VOLUME_CONTROL in innioasisy1.h).
 */

#ifndef __Y1MTK_CODEC_H__
#define __Y1MTK_CODEC_H__

#define AUDIOHW_CAPS 0

/* name, unit, decimals, step, min, max, default.  Step is 5 dB: one wheel
 * tick moves 5 dB, so the -73..+6 dB range is ~16 steps end to end -- the
 * coarse, fast volume stepping the stock player uses, instead of ~80 1 dB
 * ticks.  (The step is also the Settings > Sound > Volume increment.) */
AUDIOHW_SETTING(VOLUME, "dB", 0, 5, -73, 6, -25)

#endif /* __Y1MTK_CODEC_H__ */
