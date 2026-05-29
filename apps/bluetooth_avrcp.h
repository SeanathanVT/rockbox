/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * AVRCP glue: publishes Rockbox playback state (now-playing, status,
 * position, volume, battery) to a connected Bluetooth sink, and applies
 * inbound remote-control keys + absolute volume from it.  Transport is
 * abstracted behind <bluetooth_backend.h>, which each target implements.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 ****************************************************************************/
#ifndef _BLUETOOTH_AVRCP_H
#define _BLUETOOTH_AVRCP_H

/* Start the AVRCP glue: register playback-event + inbound handlers and spawn
 * the dispatcher thread (which also brings up the bt-client). Idempotent;
 * call once from app init. */
void bluetooth_avrcp_init(void);

#endif /* _BLUETOOTH_AVRCP_H */
