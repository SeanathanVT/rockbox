/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Innioasis Y1 backend for the shared AVRCP glue.  Binds the device-agnostic
 * bt_backend_* interface (firmware/export/bluetooth_backend.h) to the y1-btd
 * client (bt-client.c): translates the generic key/status enums to/from the
 * y1-btd IPC wire constants.  All Y1-/y1-btd-specific knowledge lives here.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 ****************************************************************************/
#include "bluetooth_backend.h"

#include "bt-client.h"
#include "y1bt/ipc_proto.h"

#include <string.h>

static bt_remote_key_fn    s_key_fn;
static bt_remote_volume_fn s_vol_fn;

static int map_key(const char *op)
{
    if (!strcmp(op, Y1BT_PT_PLAY))        return BT_KEY_PLAY;
    if (!strcmp(op, Y1BT_PT_PAUSE))       return BT_KEY_PAUSE;
    if (!strcmp(op, Y1BT_PT_STOP))        return BT_KEY_STOP;
    if (!strcmp(op, Y1BT_PT_NEXT))        return BT_KEY_NEXT;
    if (!strcmp(op, Y1BT_PT_PREV))        return BT_KEY_PREV;
    if (!strcmp(op, Y1BT_PT_FF))          return BT_KEY_FF;
    if (!strcmp(op, Y1BT_PT_RW))          return BT_KEY_REW;
    if (!strcmp(op, Y1BT_PT_VOLUME_UP))   return BT_KEY_VOL_UP;
    if (!strcmp(op, Y1BT_PT_VOLUME_DOWN)) return BT_KEY_VOL_DOWN;
    return -1;                                /* MUTE etc.: not mapped */
}

static const char *status_str(enum bt_playback_status s)
{
    switch (s) {
        case BT_PB_PLAYING:  return Y1BT_PB_PLAYING;
        case BT_PB_PAUSED:   return Y1BT_PB_PAUSED;
        case BT_PB_FWD_SEEK: return Y1BT_PB_FWD_SEEK;
        case BT_PB_REV_SEEK: return Y1BT_PB_REV_SEEK;
        case BT_PB_STOPPED:
        default:             return Y1BT_PB_STOPPED;
    }
}

/* bt-client pump-thread callbacks -> generic handlers. */
static void pt_cb(const char *op, bool pressed)
{
    int k = map_key(op);
    if (k >= 0 && s_key_fn)
        s_key_fn((enum bt_remote_key) k, pressed);
}

static void vol_cb(uint8_t volume_pct)
{
    if (s_vol_fn)
        s_vol_fn(volume_pct);
}

/* ---- bt_backend_* implementation ---- */

bool bt_backend_start(void)
{
    bt_client_set_passthrough_handler(pt_cb);
    bt_client_set_volume_handler(vol_cb);
    return bt_client_start() == 0;
}

void bt_backend_set_remote_handlers(bt_remote_key_fn key_fn,
                                    bt_remote_volume_fn volume_fn)
{
    s_key_fn = key_fn;
    s_vol_fn = volume_fn;
}

void bt_backend_now_playing(const char *title, const char *artist,
                            const char *album, const char *genre,
                            uint32_t track, uint32_t num_tracks,
                            uint32_t length_ms, const char *path)
{
    bt_client_set_now_playing(title, artist, album, genre,
                              track, num_tracks, length_ms, path);
}

void bt_backend_playback_status(enum bt_playback_status status)
{
    bt_client_set_playback_status(status_str(status));
}

void bt_backend_position(uint32_t position_ms)
{
    bt_client_set_position(position_ms);
}

void bt_backend_volume(uint8_t volume_pct)
{
    bt_client_set_volume(volume_pct);
}

void bt_backend_battery(uint8_t percent, bool charging)
{
    bt_client_set_battery(percent, charging);
}
