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
 * usable.  Callers may retry while it returns false (e.g. the transport isn't
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

/* True when Bluetooth is the active audio output (a sink is connected and
 * rendering our stream).  Lets the volume domain follow the output route:
 * while true the volume tracks/commands the sink (absolute volume); the local
 * speaker/headphone volume is saved and restored around the BT episode so the
 * sink's level never bleeds into local playback. */
bool bt_backend_audio_active(void);

/* ---- device discovery / management (used by the Bluetooth settings menu) ----
 *
 * Notifications from the backend.  May fire from the backend's own thread, so
 * handlers must be cheap and thread-safe.  `cod` is the Bluetooth Class of
 * Device (spec-defined); `rssi` is in dBm. */
struct bt_device_observer {
    void (*scan_result)(const char *addr, const char *name,
                        uint32_t cod, int8_t rssi);
    void (*scan_complete)(void);
    void (*paired_begin)(void);
    void (*paired_device)(const char *addr, const char *name, bool connected);
    void (*paired_done)(void);
    void (*connection_changed)(void);   /* a device connected/disconnected; re-query */
};

/* Register the observer for device events, or clear it with NULL. */
void bt_backend_set_device_observer(const struct bt_device_observer *obs);

/* Pairing confirmation (SSP numeric comparison): the backend invokes this when
 * the peer needs the user to verify a 6-digit `code` shown on both devices.
 * Display it, let the user accept/reject, then call bt_backend_pairing_confirm.
 * May fire from the backend's own thread, so the handler must be cheap and
 * thread-safe. Register with NULL to clear (e.g. auto-accept while no UI). */
typedef void (*bt_pairing_confirm_fn)(const char *addr, const char *name,
                                      uint32_t code);
void bt_backend_set_pairing_handler(bt_pairing_confirm_fn fn);
void bt_backend_pairing_confirm(const char *addr, bool accept);

void bt_backend_set_enabled(bool enabled);          /* radio power on/off */
void bt_backend_request_devices(void);              /* -> paired_* callbacks */
void bt_backend_scan_start(uint16_t duration_s);    /* -> scan_result/scan_complete */
void bt_backend_scan_stop(void);
void bt_backend_connect(const char *addr);
void bt_backend_disconnect(const char *addr);
void bt_backend_pair(const char *addr);
void bt_backend_forget(const char *addr);

#endif /* _BLUETOOTH_BACKEND_H */
