/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Abstract Bluetooth backend: the device-agnostic seam between the shared
 * AVRCP glue (apps/bluetooth_avrcp.c) and a target's Bluetooth stack/daemon.
 *
 * The shared layer publishes playback state and receives remote control
 * through this interface only; each target with HAVE_BLUETOOTH provides an
 * implementation under firmware/target/.../ that binds these calls to its own
 * Bluetooth transport.  No target-specific names or wire formats appear above
 * this line.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 ****************************************************************************/
#ifndef _BLUETOOTH_BACKEND_H
#define _BLUETOOTH_BACKEND_H

#include <stdbool.h>
#include <stdint.h>

enum bt_playback_status {
    BT_PB_STOPPED = 0,
    BT_PB_PLAYING,
    BT_PB_PAUSED,
    BT_PB_FWD_SEEK,
    BT_PB_REV_SEEK,
};

/* Remote-control keys the connected sink / controller can send back. */
enum bt_remote_key {
    BT_KEY_PLAY = 0,
    BT_KEY_PAUSE,
    BT_KEY_STOP,
    BT_KEY_NEXT,
    BT_KEY_PREV,
    BT_KEY_FF,
    BT_KEY_REW,
    BT_KEY_VOL_UP,
    BT_KEY_VOL_DOWN,
};

typedef void (*bt_remote_key_fn)(enum bt_remote_key key, bool pressed);
typedef void (*bt_remote_volume_fn)(uint8_t volume_pct);

/* ---- implemented by the target backend ---- */

/* Bring up / attach to the Bluetooth transport.  Idempotent; returns true once
 * usable.  Callers may retry while it returns false (e.g. a daemon that isn't
 * up yet at boot). */
bool bt_backend_start(void);

/* Register handlers for inbound remote control.  The backend invokes these
 * when the sink sends a key or an absolute-volume change -- possibly from its
 * own thread, so handlers must be cheap and thread-safe. */
void bt_backend_set_remote_handlers(bt_remote_key_fn key_fn,
                                    bt_remote_volume_fn volume_fn);

/* Publish playback state to the connected sink.  NULL strings become "". */
void bt_backend_now_playing(const char *title, const char *artist,
                            const char *album, const char *genre,
                            uint32_t track, uint32_t num_tracks,
                            uint32_t length_ms, const char *path);
void bt_backend_playback_status(enum bt_playback_status status);
void bt_backend_position(uint32_t position_ms);
void bt_backend_volume(uint8_t volume_pct);
void bt_backend_battery(uint8_t percent, bool charging);

#endif /* _BLUETOOTH_BACKEND_H */
