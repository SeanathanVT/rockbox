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

#include "config.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>

#include "kernel.h"
#include "thread.h"
#include "appevents.h"
#include "audio.h"
#include "metadata.h"
#include "settings.h"
#include "sound.h"
#include "powermgmt.h"

#include "bluetooth_backend.h"

/* ---- inbound (backend thread -> dispatcher) lock-free SPSC ring ---- */

enum { IN_KEY = 0, IN_VOLUME };
struct in_ev { uint8_t kind; uint8_t arg; };   /* KEY: arg=enum bt_remote_key; VOLUME: arg=percent */

#define IN_RING 16
static struct in_ev      in_ring[IN_RING];
static _Atomic unsigned  in_head;   /* producer = backend thread */
static _Atomic unsigned  in_tail;   /* consumer = dispatcher     */

static void in_push(uint8_t kind, uint8_t arg)
{
    unsigned h = atomic_load_explicit(&in_head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&in_tail, memory_order_acquire);
    if (h - t >= IN_RING)
        return;                             /* full: drop (keys are idempotent enough) */
    in_ring[h % IN_RING].kind = kind;
    in_ring[h % IN_RING].arg  = arg;
    atomic_store_explicit(&in_head, h + 1, memory_order_release);
}

/* ---- inbound handlers: invoked by the backend, possibly off-thread ---- */

static void on_remote_key(enum bt_remote_key key, bool pressed)
{
    if (!pressed)                            /* act on the press, ignore release */
        return;
    in_push(IN_KEY, (uint8_t) key);
}

static void on_remote_volume(uint8_t volume_pct)
{
    in_push(IN_VOLUME, volume_pct);
}

/* ---- volume helpers (dispatcher thread) ---- */

static int last_vol_sent = INT_MIN;          /* last percent we PUBLISHED to the sink */

/* Per-output-path volume.  global_status.volume is the LOCAL (speaker/HP)
 * volume except while BT is the active output, when it becomes the BT volume
 * domain (tracks/commands the sink via absolute volume).  We snapshot the local
 * volume entering a BT episode and restore it on exit, so the sink's level --
 * a different scale entirely -- never carries over to local playback. */
static int  saved_local_volume = INT_MIN;
static bool bt_was_active       = false;
#if defined(HAVE_HEADPHONE_DETECTION) || defined(HAVE_LINEOUT_DETECTION)
static bool bt_caused_pause     = false;  /* we paused because the sink went away */
#endif

static int vol_to_percent(int v)
{
    int lo = sound_min(SOUND_VOLUME);
    int hi = sound_max(SOUND_VOLUME);
    int pct;
    if (hi <= lo)
        return 0;
    pct = ((v - lo) * 100 + (hi - lo) / 2) / (hi - lo);
    return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

static void apply_abs_volume(uint8_t percent)
{
    /* The sink's reported volume drives the BT domain only.  Ignore it when BT
     * isn't the active output so a stray notification can't move local volume. */
    if (!bt_was_active)
        return;
    int lo = sound_min(SOUND_VOLUME);
    int hi = sound_max(SOUND_VOLUME);
    int v  = lo + ((int) percent * (hi - lo) + 50) / 100;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    global_status.volume = v;
    sound_set_volume(v);
    /* Suppress echoing this change straight back (avoid a feedback loop),
     * using the re-derived percent so poll_volume() sees no delta. */
    last_vol_sent = vol_to_percent(v);
}

static void step_volume(int delta)
{
    int lo = sound_min(SOUND_VOLUME);
    int hi = sound_max(SOUND_VOLUME);
    int v  = global_status.volume + delta;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    global_status.volume = v;
    sound_set_volume(v);
}

/* ---- inbound dispatch (dispatcher thread, Rockbox-safe) ---- */

static void in_handle(uint8_t kind, uint8_t arg)
{
    struct mp3entry *id3;
    long pos;

    if (kind == IN_VOLUME) {
        apply_abs_volume(arg);
        return;
    }
    switch ((enum bt_remote_key) arg) {
        case BT_KEY_PLAY:
            if (audio_status() & AUDIO_STATUS_PAUSE)
                audio_resume();
            break;
        case BT_KEY_PAUSE: audio_pause(); break;
        case BT_KEY_STOP:  audio_stop();  break;
        case BT_KEY_NEXT:  audio_next();  break;
        case BT_KEY_PREV:  audio_prev();  break;
        case BT_KEY_FF:
        case BT_KEY_REW:
            id3 = audio_current_track();
            if (id3) {
                pos = (long) id3->elapsed + (arg == BT_KEY_FF ? 5000 : -5000);
                if (pos < 0) pos = 0;
                audio_ff_rewind(pos);
            }
            break;
        case BT_KEY_VOL_UP: step_volume(+1); break;
        case BT_KEY_VOL_DOWN: step_volume(-1); break;
        default: break;
    }
}

static void in_drain(void)
{
    for (;;) {
        unsigned t = atomic_load_explicit(&in_tail, memory_order_relaxed);
        unsigned h = atomic_load_explicit(&in_head, memory_order_acquire);
        struct in_ev e;
        if (t == h)
            break;
        e = in_ring[t % IN_RING];
        atomic_store_explicit(&in_tail, t + 1, memory_order_release);
        in_handle(e.kind, e.arg);
    }
}

/* ---- outbound polling (dispatcher thread) ---- */

static int last_status = -1;

static void poll_status(void)
{
    int st = audio_status();
    enum bt_playback_status s = (st & AUDIO_STATUS_PAUSE) ? BT_PB_PAUSED
                              : (st & AUDIO_STATUS_PLAY)   ? BT_PB_PLAYING
                              :                              BT_PB_STOPPED;
    if ((int) s != last_status) {
        last_status = s;
        bt_backend_playback_status(s);
    }
}

/* Follow the output route: snapshot/restore the local volume around a BT
 * episode so the two domains never bleed into each other. */
static void poll_output_route(void)
{
    bool active = bt_backend_audio_active();
    if (active == bt_was_active)
        return;
    bt_was_active = active;
    last_vol_sent = INT_MIN;                  /* force re-publish on the new path */
    if (active) {
        /* local -> BT: remember the speaker/HP level.  The sink's VolumeChanged
         * populates the BT domain (global_status.volume) from here on. */
        saved_local_volume = global_status.volume;
    } else if (saved_local_volume != INT_MIN) {
        /* BT -> local: the BT episode left global_status.volume on the sink's
         * scale; restore the speaker/HP volume we entered with. */
        global_status.volume = saved_local_volume;
        sound_set_volume(saved_local_volume);
    }

#if defined(HAVE_HEADPHONE_DETECTION) || defined(HAVE_LINEOUT_DETECTION)
    /* Output-lost -> pause, rather than dumping playback onto the local
     * speaker when the sink goes away.  Mirrors "pause on headphone unplug"
     * and shares its setting; unplug_mode > 1 ("pause and resume") also
     * resumes when the sink comes back. */
    if (!active) {
        if (global_settings.unplug_mode) {
            int st = audio_status();
            if ((st & AUDIO_STATUS_PLAY) && !(st & AUDIO_STATUS_PAUSE)) {
                bt_caused_pause = true;
                audio_pause();
            }
        }
    } else if (bt_caused_pause) {
        if (global_settings.unplug_mode > 1 &&
            (audio_status() & AUDIO_STATUS_PAUSE))
            audio_resume();
        bt_caused_pause = false;
    }
#endif
}

static void poll_volume(void)
{
    if (!bt_was_active)                       /* abs-volume is the BT path only */
        return;
    int pct = vol_to_percent(global_status.volume);
    if (pct != last_vol_sent) {
        last_vol_sent = pct;
        bt_backend_volume((uint8_t) pct);
    }
}

static void poll_position(void)
{
    struct mp3entry *id3;
    if (!(audio_status() & AUDIO_STATUS_PLAY))
        return;
    id3 = audio_current_track();
    if (id3)
        bt_backend_position((uint32_t) id3->elapsed);
}

static int last_batt = -1;
static int last_chg  = -1;

static void poll_battery(void)
{
    int pct = battery_level();
#if CONFIG_CHARGING >= CHARGING_MONITOR
    int chg = (charge_state > DISCHARGING) ? 1 : 0;
#else
    int chg = 0;
#endif
    if (pct == last_batt && chg == last_chg)
        return;
    last_batt = pct;
    last_chg  = chg;
    bt_backend_battery((uint8_t)(pct < 0 ? 0 : pct), chg != 0);
}

/* ---- now-playing (playback thread, via app events) ---- */

static void on_track(unsigned short id, void *data)
{
    struct track_event *te = (struct track_event *) data;
    struct mp3entry *e;
    (void) id;
    if (!te || !te->id3)
        return;
    e = te->id3;
    bt_backend_now_playing(e->title        ? e->title        : "",
                           e->artist       ? e->artist       : "",
                           e->album        ? e->album        : "",
                           e->genre_string ? e->genre_string : "",
                           (uint32_t) e->tracknum,
                           0,                       /* num_tracks: unknown */
                           (uint32_t) e->length,
                           e->path);
    /* A fresh track means we're playing; nudge status + position now. */
    last_status = BT_PB_PLAYING;
    bt_backend_playback_status(BT_PB_PLAYING);
    bt_backend_position(0);
}

/* ---- dispatcher thread ---- */

static long btav_stack[(DEFAULT_STACK_SIZE + 0x1000) / sizeof(long)];
static const char btav_name[] = "bt avrcp";

static void btav_thread(void)
{
    bool started = false;
    int c_pos = 0, c_batt = 0;

    for (;;) {
        sleep(HZ / 20);                      /* ~50 ms */

        if (!started) {
            /* The transport may not be up at boot; retry until it is. */
            if (!bt_backend_start())
                continue;
            started = true;
            /* Honour the persisted radio-power preference (the radio may
             * default to on at boot; a saved "off" turns it back off). */
            bt_backend_set_enabled(global_status.bluetooth_enabled != 0);
        }

        poll_output_route();   /* before in_drain: inbound volume needs the current path */
        in_drain();
        poll_status();
        poll_volume();
        if (++c_pos  >= 20) { c_pos  = 0; poll_position(); }   /* ~1 s  */
        if (++c_batt >= 40) { c_batt = 0; poll_battery();  }   /* ~2 s  */
    }
}

void bluetooth_avrcp_init(void)
{
    static bool inited = false;
    if (inited)
        return;
    inited = true;

    /* Register inbound handlers BEFORE the transport starts so no event is
     * missed once it connects. */
    bt_backend_set_remote_handlers(on_remote_key, on_remote_volume);

    add_event(PLAYBACK_EVENT_TRACK_CHANGE, on_track);
    add_event(PLAYBACK_EVENT_CUR_TRACK_READY, on_track);

    create_thread(btav_thread, btav_stack, sizeof(btav_stack), 0,
                  btav_name IF_PRIO(, PRIORITY_BACKGROUND) IF_COP(, CPU));
}
