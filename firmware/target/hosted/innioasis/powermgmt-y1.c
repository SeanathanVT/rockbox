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
 * Innioasis Y1 — battery voltage/percentage tables.
 *
 * Verified ranges (from kernel dmesg on a charged device):
 *   - AvgVbat at "Battery full" (UI 100%, SOC 99-100): ~4297-4308 mV
 *   - ZCV (Zero-Current Voltage) at full: ~4343 mV
 *   - Stock UI reports 100% when SOC tracking shows 99
 *
 * Discharge curve below is a rough Li-ion estimate; refine by reading
 * /sys/class/power_supply/battery/{capacity,batt_vol} over real discharge cycles.
 */

#include "powermgmt.h"
#include "power.h"

/* Above this, /data is safely writable */
unsigned short battery_level_disksafe = 3500;

/* The stock firmware shuts down at this voltage */
unsigned short battery_level_shutoff  = 3400;

/* mV at 0%, 10%, ..., 100% — discharging */
unsigned short percent_to_volt_discharge[11] =
{
    3400, 3650, 3700, 3740, 3770, 3800, 3850, 3920, 4000, 4100, 4200
};

/* mV at 0%, 10%, ..., 100% — charging */
unsigned short percent_to_volt_charge[11] =
{
    3500, 3760, 3810, 3840, 3880, 3920, 3980, 4060, 4150, 4200, 4250
};
