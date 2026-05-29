/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * AVRCP glue between Rockbox playback and the y1-btd Bluetooth daemon.
 *
 * Outbound (Rockbox -> daemon -> Controller):
 *   - now-playing metadata: on PLAYBACK_EVENT_TRACK_CHANGE / CUR_TRACK_READY
 *     (runs on the playback thread; bt_client_set_* only writes the control
 *     socket, so it's safe there).
 *   - playback status / position / volume / battery: polled by a dedicated
 *     Rockbox dispatcher thread (audio_*/sound_* are safe from a real Rockbox
 *     thread, unlike the bt-client pump pthread).
 *
 * Inbound (Controller -> daemon -> Rockbox):
 *   - passthrough keys + absolute volume arrive on the bt-client PUMP pthread.
 *     Its handlers must NOT touch Rockbox APIs, so they only push onto a
 *     lock-free SPSC ring; the dispatcher thread drains it and drives playback.
 ****************************************************************************/
#include "config.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>

#include "kernel.h"
#include "thread.h"
#include "appevents.h"
#include "audio.h"
#include "metadata.h"
#include "settings.h"
#include "sound.h"
#include "powermgmt.h"

#include "bluetooth/bt-client.h"
#include "y1bt/ipc_proto.h"

/* ---- inbound (pump pthread -> dispatcher) lock-free SPSC ring ---- */

enum { IN_PASSTHROUGH = 0, IN_VOLUME };
enum pt_op { PT_NONE = 0, PT_PLAY, PT_PAUSE, PT_STOP, PT_NEXT, PT_PREV,
             PT_FF, PT_RW, PT_VOLUP, PT_VOLDN };

struct in_ev { uint8_t kind; uint8_t arg; };   /* PASSTHROUGH: arg=pt_op; VOLUME: arg=percent */

#define IN_RING 16
static struct in_ev      in_ring[IN_RING];
static _Atomic unsigned  in_head;   /* producer = pump pthread  */
static _Atomic unsigned  in_tail;   /* consumer = dispatcher    */

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

/* ---- bt-client inbound handlers: run on the PUMP pthread, ring-only ---- */

static enum pt_op map_pt(const char *op)
{
    if (!strcmp(op, Y1BT_PT_PLAY))        return PT_PLAY;
    if (!strcmp(op, Y1BT_PT_PAUSE))       return PT_PAUSE;
    if (!strcmp(op, Y1BT_PT_STOP))        return PT_STOP;
    if (!strcmp(op, Y1BT_PT_NEXT))        return PT_NEXT;
    if (!strcmp(op, Y1BT_PT_PREV))        return PT_PREV;
    if (!strcmp(op, Y1BT_PT_FF))          return PT_FF;
    if (!strcmp(op, Y1BT_PT_RW))          return PT_RW;
    if (!strcmp(op, Y1BT_PT_VOLUME_UP))   return PT_VOLUP;
    if (!strcmp(op, Y1BT_PT_VOLUME_DOWN)) return PT_VOLDN;
    return PT_NONE;                          /* MUTE etc. ignored for now */
}

static void on_passthrough(const char *op, bool pressed)
{
    enum pt_op e;
    if (!pressed)                            /* act on the press, ignore release */
        return;
    e = map_pt(op);
    if (e != PT_NONE)
        in_push(IN_PASSTHROUGH, (uint8_t) e);
}

static void on_volume(uint8_t volume_percent)
{
    in_push(IN_VOLUME, volume_percent);
}

/* ---- volume helpers (dispatcher thread) ---- */

static int last_vol_sent = INT_MIN;          /* last percent we PUBLISHED to the CT */

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
    int lo = sound_min(SOUND_VOLUME);
    int hi = sound_max(SOUND_VOLUME);
    int v  = lo + ((int) percent * (hi - lo) + 50) / 100;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    global_settings.volume = v;
    sound_set_volume(v);
    /* Suppress echoing this change straight back to the CT (avoid a feedback
     * loop), using the re-derived percent so poll_volume() sees no delta. */
    last_vol_sent = vol_to_percent(v);
}

static void step_volume(int delta)
{
    int lo = sound_min(SOUND_VOLUME);
    int hi = sound_max(SOUND_VOLUME);
    int v  = global_settings.volume + delta;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    global_settings.volume = v;
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
    switch (arg) {
        case PT_PLAY:
            if (audio_status() & AUDIO_STATUS_PAUSE)
                audio_resume();
            break;
        case PT_PAUSE: audio_pause(); break;
        case PT_STOP:  audio_stop();  break;
        case PT_NEXT:  audio_next();  break;
        case PT_PREV:  audio_prev();  break;
        case PT_FF:
        case PT_RW:
            id3 = audio_current_track();
            if (id3) {
                pos = (long) id3->elapsed + (arg == PT_FF ? 5000 : -5000);
                if (pos < 0) pos = 0;
                audio_ff_rewind(pos);
            }
            break;
        case PT_VOLUP: step_volume(+1); break;
        case PT_VOLDN: step_volume(-1); break;
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

static const char *last_status;

static void poll_status(void)
{
    int st = audio_status();
    const char *s = (st & AUDIO_STATUS_PAUSE) ? Y1BT_PB_PAUSED
                  : (st & AUDIO_STATUS_PLAY)   ? Y1BT_PB_PLAYING
                  :                              Y1BT_PB_STOPPED;
    if (s != last_status) {
        last_status = s;
        bt_client_set_playback_status(s);
    }
}

static void poll_volume(void)
{
    int pct = vol_to_percent(global_settings.volume);
    if (pct != last_vol_sent) {
        last_vol_sent = pct;
        bt_client_set_volume((uint8_t) pct);
    }
}

static void poll_position(void)
{
    struct mp3entry *id3;
    if (!(audio_status() & AUDIO_STATUS_PLAY))
        return;
    id3 = audio_current_track();
    if (id3)
        bt_client_set_position((uint32_t) id3->elapsed);
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
    bt_client_set_battery((uint8_t)(pct < 0 ? 0 : pct), chg != 0);
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
    bt_client_set_now_playing(e->title        ? e->title        : "",
                              e->artist       ? e->artist       : "",
                              e->album        ? e->album        : "",
                              e->genre_string ? e->genre_string : "",
                              (uint32_t) e->tracknum,
                              0,                       /* num_tracks: unknown */
                              (uint32_t) e->length,
                              e->path);
    /* A fresh track means we're playing; nudge status + position now. */
    last_status = Y1BT_PB_PLAYING;
    bt_client_set_playback_status(Y1BT_PB_PLAYING);
    bt_client_set_position(0);
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
            /* The daemon's PCM ring may not exist yet at boot; retry until it
             * does, which also spawns the bt-client pump for inbound events. */
            if (bt_client_start() != 0)
                continue;
            started = true;
        }

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

    /* Register inbound handlers BEFORE the pump starts so no event is missed. */
    bt_client_set_passthrough_handler(on_passthrough);
    bt_client_set_volume_handler(on_volume);

    add_event(PLAYBACK_EVENT_TRACK_CHANGE, on_track);
    add_event(PLAYBACK_EVENT_CUR_TRACK_READY, on_track);

    create_thread(btav_thread, btav_stack, sizeof(btav_stack), 0,
                  btav_name IF_PRIO(, PRIORITY_BACKGROUND) IF_COP(, CPU));
}
