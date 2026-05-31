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
 * bt-client.c — Rockbox-side glue for the y1-btd Bluetooth daemon.
 *
 * Two channels:
 *  - control: UNIX SOCK_STREAM at Y1BT_CTRL_SOCK_PATH (/tmp/y1btd.sock), line-delimited JSON.
 *  - PCM:     SPSC ring at Y1BT_PCM_RING_PATH (/tmp/y1btd-pcm), mmap'd.
 *
 * Threading model:
 *  - Public API (bt_client_set_*, bt_client_pcm_write) is callable from any
 *    Rockbox thread.
 *  - Control-socket writes serialize on tx_mtx.
 *  - The IPC reader runs on a dedicated pthread that decodes inbound events
 *    and invokes user-supplied handlers. Handlers run on the pump thread —
 *    callers must short-circuit back to Rockbox's event queue if they need
 *    Rockbox-thread context.
 */
#define _GNU_SOURCE              /* memmem */

#include "bt-client.h"
#include "y1bt/ipc_proto.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int  ctrl_fd = -1;
static int  ring_fd = -1;
static struct y1bt_pcm_ring *ring;
static size_t ring_bytes;

static pthread_mutex_t tx_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       pump_thread;
static bool            pump_running;
static atomic_bool     bt_is_output_atomic;

static bt_passthrough_handler_t       cb_pt;
static bt_volume_handler_t            cb_vol;
static bt_connection_handler_t        cb_conn;
static bt_now_playing_in_handler_t    cb_np_in;
static bt_inquiry_result_handler_t    cb_inq;
static bt_inquiry_complete_handler_t  cb_inq_done;
static bt_pairing_request_handler_t   cb_pair;
static void                          (*cb_paired_begin)(void);
static bt_paired_device_handler_t     cb_paired_dev;
static void                          (*cb_paired_done)(void);

static void close_ctrl(void) {
    if (ctrl_fd >= 0) { close(ctrl_fd); ctrl_fd = -1; }
}

static int connect_ctrl(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, Y1BT_CTRL_SOCK_PATH, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        close(fd); return -1;
    }
    return fd;
}

static int open_ring(void) {
    if (ring) return 0;                  /* already attached (idempotent start) */
    int fd = open(Y1BT_PCM_RING_PATH, O_RDWR);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || (size_t) st.st_size < Y1BT_PCM_RING_BYTES) {
        close(fd); return -1;
    }
    ring_bytes = (size_t) st.st_size;
    void *m = mmap(NULL, ring_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { close(fd); return -1; }
    ring = (struct y1bt_pcm_ring *) m;
    if (ring->magic != Y1BT_PCM_MAGIC) {
        munmap(m, ring_bytes); close(fd); return -1;
    }
    ring_fd = fd;
    return 0;
}

/* ---- minimal JSON parsers (mirror of daemon ipc_dispatch.c) ---- */

static bool find_str(const char *line, int len, const char *key,
                     char *out, size_t out_sz) {
    char pat[64];
    int n = snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    if (n <= 0) return false;
    const char *p = memmem(line, len, pat, (size_t) n);
    if (!p) return false;
    p += n;
    const char *lend = line + len;
    /* Walk the value, honoring JSON backslash escapes (only \" and \\
     * matter for our daemon-emitted values; other escapes get passed
     * through verbatim).  Stop at the FIRST unescaped double-quote. */
    size_t oi = 0;
    while (p < lend && *p != '"') {
        char c;
        if (*p == '\\' && p + 1 < lend) {
            p++;                 /* skip backslash */
            c = *p++;            /* take escaped char as-is */
        } else {
            c = *p++;
        }
        if (oi + 1 < out_sz) out[oi++] = c;
    }
    if (p >= lend) return false;
    out[oi] = '\0';
    return true;
}

static bool find_int(const char *line, int len, const char *key, int64_t *out) {
    char pat[64];
    int n = snprintf(pat, sizeof(pat), "\"%s\":", key);
    if (n <= 0) return false;
    const char *p = memmem(line, len, pat, (size_t) n);
    if (!p) return false;
    p += n; while (p < line + len && (*p == ' ' || *p == '\t')) p++;
    char *e; long long v = strtoll(p, &e, 10);
    if (e == p) return false;
    *out = (int64_t) v; return true;
}

static bool find_bool(const char *line, int len, const char *key, bool *out) {
    char pat[64];
    int n = snprintf(pat, sizeof(pat), "\"%s\":", key);
    if (n <= 0) return false;
    const char *p = memmem(line, len, pat, (size_t) n);
    if (!p) return false;
    p += n; while (p < line + len && (*p == ' ' || *p == '\t')) p++;
    if (p + 4 <= line + len && !memcmp(p, "true", 4))  { *out = true;  return true; }
    if (p + 5 <= line + len && !memcmp(p, "false", 5)) { *out = false; return true; }
    return false;
}

/* Parse the list_devices reply: {"ok":true,"r":{"devices":[{...},{...}]}}.
 * Objects are flat; we find each one's closing brace while skipping braces that
 * sit inside quoted strings (a device name may contain '}'). */
static void parse_devices(const char *line, int len) {
    const char *lend = line + len;
    const char *p = memmem(line, len, "\"devices\":", 10);
    if (!p) return;
    if (cb_paired_begin) cb_paired_begin();
    const char *q = p;
    while (q < lend) {
        const char *obj = memchr(q, '{', (size_t)(lend - q));
        if (!obj) break;
        const char *end = obj + 1;
        bool inq = false;
        while (end < lend) {
            char c = *end;
            if (inq) { if (c == '\\') end++; else if (c == '"') inq = false; }
            else     { if (c == '"') inq = true; else if (c == '}') break; }
            end++;
        }
        if (end >= lend) break;
        int olen = (int)(end - obj) + 1;
        char addr[20] = {0}, name[64] = {0};
        bool connected = false;
        find_str(obj, olen, "addr", addr, sizeof(addr));
        find_str(obj, olen, "name", name, sizeof(name));
        find_bool(obj, olen, "connected", &connected);
        if (addr[0] && cb_paired_dev) cb_paired_dev(addr, name, connected);
        q = end + 1;
    }
    if (cb_paired_done) cb_paired_done();
}

static void dispatch_event(const char *line, int len) {
    char ev[64] = {0};
    if (!find_str(line, len, "event", ev, sizeof(ev))) {
        /* Not a push event; the only reply we consume is list_devices. */
        if (memmem(line, len, "\"devices\":", 10)) parse_devices(line, len);
        return;
    }

    if (!strcmp(ev, Y1BT_EVENT_PASSTHROUGH)) {
        char op[16] = {0}; bool pressed = false;
        find_str(line, len, "op", op, sizeof(op));
        find_bool(line, len, "pressed", &pressed);
        if (cb_pt) cb_pt(op, pressed);

    } else if (!strcmp(ev, Y1BT_EVENT_VOLUME_CHANGED)) {
        int64_t v = 0;
        find_int(line, len, "volume_percent", &v);
        if (cb_vol) cb_vol((uint8_t)(v < 0 ? 0 : (v > 100 ? 100 : v)));

    } else if (!strcmp(ev, Y1BT_EVENT_CONNECTION_STATE)) {
        char addr[20] = {0}, prof[16] = {0}, state[24] = {0};
        find_str(line, len, "addr",    addr,  sizeof(addr));
        find_str(line, len, "profile", prof,  sizeof(prof));
        find_str(line, len, "state",   state, sizeof(state));
        /* Automatic output routing (phone-like): the A2DP stream coming up or
         * going down switches output to/from Bluetooth.  pcm-y1mtk reads this
         * flag and falls back to headphones/speaker (by jack) when it's off. */
        if (!strcmp(prof, "a2dp")) {
            if (!strcmp(state, "connected"))
                atomic_store(&bt_is_output_atomic, true);
            else if (!strcmp(state, "disconnected"))
                atomic_store(&bt_is_output_atomic, false);
        }
        if (cb_conn) cb_conn(addr, prof, state);

    } else if (!strcmp(ev, Y1BT_EVENT_NOW_PLAYING_IN)) {
        char title[256] = {0}, artist[256] = {0}, album[256] = {0}, art[256] = {0};
        int64_t length_ms = 0;
        find_str(line, len, "title",    title,  sizeof(title));
        find_str(line, len, "artist",   artist, sizeof(artist));
        find_str(line, len, "album",    album,  sizeof(album));
        find_str(line, len, "art_path", art,    sizeof(art));
        find_int(line, len, "length_ms", &length_ms);
        if (cb_np_in) cb_np_in(title, artist, album, (uint32_t) length_ms, art);

    } else if (!strcmp(ev, Y1BT_EVENT_INQUIRY_RESULT)) {
        char addr[20] = {0}, name[256] = {0};
        int64_t cod = 0, rssi = 0;
        find_str(line, len, "addr", addr, sizeof(addr));
        find_str(line, len, "name", name, sizeof(name));
        find_int(line, len, "cod",  &cod);
        find_int(line, len, "rssi", &rssi);
        if (cb_inq) cb_inq(addr, name, (uint32_t) cod, (int8_t) rssi);

    } else if (!strcmp(ev, Y1BT_EVENT_INQUIRY_COMPLETE)) {
        if (cb_inq_done) cb_inq_done();

    } else if (!strcmp(ev, Y1BT_EVENT_PAIRING_REQUEST)) {
        char addr[20] = {0}, name[256] = {0}, kind[16] = {0};
        int64_t code = 0;
        find_str(line, len, "addr", addr, sizeof(addr));
        find_str(line, len, "name", name, sizeof(name));
        find_str(line, len, "kind", kind, sizeof(kind));
        find_int(line, len, "code", &code);
        if (cb_pair) cb_pair(addr, name, kind, (uint32_t) code);
    }
}

static void *pump_main(void *arg) {
    (void) arg;
    char buf[4096];
    int buf_len = 0;
    pump_running = true;
    while (pump_running) {
        if (ctrl_fd < 0) {
            ctrl_fd = connect_ctrl();
            if (ctrl_fd < 0) { sleep(1); continue; }
        }
        ssize_t n = read(ctrl_fd, buf + buf_len, sizeof(buf) - 1 - buf_len);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            close_ctrl(); buf_len = 0;
            sleep(1); continue;
        }
        buf_len += (int) n; buf[buf_len] = '\0';
        int start = 0;
        for (int i = 0; i < buf_len; i++) {
            if (buf[i] != '\n') continue;
            if (i > start) dispatch_event(&buf[start], i - start);
            start = i + 1;
        }
        if (start > 0) { buf_len -= start; memmove(buf, buf + start, buf_len); }
    }
    return NULL;
}

/* ---- public API ---- */

int bt_client_start(void) {
    /* control socket is opened lazily by the pump thread. */
    if (open_ring() < 0) return -1;
    if (!pump_running) {
        if (pthread_create(&pump_thread, NULL, pump_main, NULL) != 0) return -1;
    }
    return 0;
}

void bt_client_stop(void) {
    pump_running = false;
    if (pump_thread) { pthread_join(pump_thread, NULL); pump_thread = 0; }
    close_ctrl();
    if (ring) { munmap(ring, ring_bytes); ring = NULL; }
    if (ring_fd >= 0) { close(ring_fd); ring_fd = -1; }
}

bool bt_client_is_output(void)         { return atomic_load(&bt_is_output_atomic); }
void bt_client_set_output(bool on)     { atomic_store(&bt_is_output_atomic, on); }

static void send_op_line(const char *line) {
    if (ctrl_fd < 0) return;
    size_t len = strlen(line);
    pthread_mutex_lock(&tx_mtx);
    while (len) {
        ssize_t w = write(ctrl_fd, line, len);
        if (w < 0) {
            if (errno == EINTR) continue;
            close_ctrl(); break;
        }
        line += w; len -= (size_t) w;
    }
    pthread_mutex_unlock(&tx_mtx);
}

void bt_client_set_now_playing(const char *title, const char *artist,
                                const char *album, const char *genre,
                                uint32_t track_num, uint32_t num_tracks,
                                uint32_t length_ms, const char *path) {
    char buf[2048];
    int n = snprintf(buf, sizeof(buf),
        "{\"op\":\"%s\",\"p\":{\"title\":\"%s\",\"artist\":\"%s\",\"album\":\"%s\","
        "\"genre\":\"%s\",\"track_num\":%u,\"num_tracks\":%u,\"length_ms\":%u,"
        "\"path\":\"%s\"}}\n",
        Y1BT_OP_SET_NOW_PLAYING,
        title ? title : "", artist ? artist : "", album ? album : "",
        genre ? genre : "",
        (unsigned) track_num, (unsigned) num_tracks, (unsigned) length_ms,
        path ? path : "");
    if (n > 0 && n < (int) sizeof(buf)) send_op_line(buf);
}

void bt_client_set_playback_status(const char *status) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf),
                     "{\"op\":\"%s\",\"p\":{\"status\":\"%s\"}}\n",
                     Y1BT_OP_SET_PLAYBACK_STATUS, status ? status : "stopped");
    if (n > 0) send_op_line(buf);
}

void bt_client_set_position(uint32_t position_ms) {
    char buf[96];
    int n = snprintf(buf, sizeof(buf),
                     "{\"op\":\"%s\",\"p\":{\"position_ms\":%u}}\n",
                     Y1BT_OP_SET_POSITION, (unsigned) position_ms);
    if (n > 0) send_op_line(buf);
}

void bt_client_set_volume(uint8_t v) {
    char buf[96];
    int n = snprintf(buf, sizeof(buf),
                     "{\"op\":\"%s\",\"p\":{\"volume_percent\":%u}}\n",
                     Y1BT_OP_SET_VOLUME, (unsigned) v);
    if (n > 0) send_op_line(buf);
}

void bt_client_set_battery(uint8_t percent, bool charging) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf),
                     "{\"op\":\"%s\",\"p\":{\"percent\":%u,\"charging\":%s}}\n",
                     Y1BT_OP_SET_BATTERY, (unsigned) percent,
                     charging ? "true" : "false");
    if (n > 0) send_op_line(buf);
}

void bt_client_set_pairing_mode(bool disc, uint16_t timeout_s) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf),
                     "{\"op\":\"%s\",\"p\":{\"discoverable\":%s,\"timeout_s\":%u}}\n",
                     Y1BT_OP_SET_PAIRING_MODE, disc ? "true" : "false",
                     (unsigned) timeout_s);
    if (n > 0) send_op_line(buf);
}

void bt_client_pairing_confirm(const char *addr, bool accept) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf),
                     "{\"op\":\"%s\",\"p\":{\"addr\":\"%s\",\"accept\":%s}}\n",
                     Y1BT_OP_PAIRING_CONFIRM, addr ? addr : "",
                     accept ? "true" : "false");
    if (n > 0) send_op_line(buf);
}

void bt_client_connect_device(const char *addr) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf),
                     "{\"op\":\"%s\",\"p\":{\"addr\":\"%s\"}}\n",
                     Y1BT_OP_CONNECT_DEVICE, addr ? addr : "");
    if (n > 0) send_op_line(buf);
}

void bt_client_disconnect_device(const char *addr) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf),
                     "{\"op\":\"%s\",\"p\":{\"addr\":\"%s\"}}\n",
                     Y1BT_OP_DISCONNECT_DEVICE, addr ? addr : "");
    if (n > 0) send_op_line(buf);
}

void bt_client_forget_device(const char *addr) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf),
                     "{\"op\":\"%s\",\"p\":{\"addr\":\"%s\"}}\n",
                     Y1BT_OP_FORGET_DEVICE, addr ? addr : "");
    if (n > 0) send_op_line(buf);
}

void bt_client_start_inquiry(uint16_t duration_s) {
    char buf[96];
    int n = snprintf(buf, sizeof(buf),
                     "{\"op\":\"%s\",\"p\":{\"duration_s\":%u}}\n",
                     Y1BT_OP_INQUIRY_START, (unsigned) duration_s);
    if (n > 0) send_op_line(buf);
}

void bt_client_cancel_inquiry(void) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "{\"op\":\"%s\"}\n", Y1BT_OP_INQUIRY_CANCEL);
    if (n > 0) send_op_line(buf);
}

void bt_client_set_enabled(bool on) {
    char buf[80];
    int n = snprintf(buf, sizeof(buf),
                     "{\"op\":\"%s\",\"p\":{\"enabled\":%s}}\n",
                     Y1BT_OP_SET_ENABLED, on ? "true" : "false");
    if (n > 0) send_op_line(buf);
}

void bt_client_request_devices(void) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "{\"op\":\"%s\"}\n", Y1BT_OP_LIST_DEVICES);
    if (n > 0) send_op_line(buf);
}

void bt_client_set_paired_handler(void (*begin)(void),
                                  bt_paired_device_handler_t device,
                                  void (*done)(void)) {
    cb_paired_begin = begin;
    cb_paired_dev   = device;
    cb_paired_done  = done;
}

void bt_client_pair_device(const char *addr) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf),
                     "{\"op\":\"%s\",\"p\":{\"addr\":\"%s\"}}\n",
                     Y1BT_OP_PAIR_DEVICE, addr ? addr : "");
    if (n > 0) send_op_line(buf);
}

size_t bt_client_pcm_write(const int16_t *samples, size_t frames) {
    if (!ring || !samples || !frames) return 0;
    uint32_t cap = ring->capacity_frames;
    uint32_t h   = atomic_load_explicit(&ring->head, memory_order_relaxed);
    uint32_t t   = atomic_load_explicit(&ring->tail, memory_order_acquire);
    uint32_t fill = (h >= t) ? (h - t) : (cap - (t - h));
    size_t free_frames = (size_t) (cap - 1 - fill);
    if (frames > free_frames) frames = free_frames;
    if (frames == 0) return 0;
    size_t first = frames;
    if (h + frames > cap) first = cap - h;
    memcpy(&ring->samples[h * Y1BT_PCM_CHANNELS], samples,
           first * Y1BT_PCM_CHANNELS * sizeof(int16_t));
    if (frames > first) {
        memcpy(&ring->samples[0],
               &samples[first * Y1BT_PCM_CHANNELS],
               (frames - first) * Y1BT_PCM_CHANNELS * sizeof(int16_t));
    }
    atomic_store_explicit(&ring->head, (uint32_t)((h + frames) % cap),
                          memory_order_release);
    return frames;
}

void bt_client_set_passthrough_handler(bt_passthrough_handler_t h)     { cb_pt   = h; }
void bt_client_set_volume_handler(bt_volume_handler_t h)               { cb_vol  = h; }
void bt_client_set_connection_handler(bt_connection_handler_t h)       { cb_conn = h; }
void bt_client_set_pairing_request_handler(bt_pairing_request_handler_t h) { cb_pair = h; }
void bt_client_set_now_playing_in_handler(bt_now_playing_in_handler_t h){cb_np_in= h; }
void bt_client_set_inquiry_result_handler(bt_inquiry_result_handler_t h)   { cb_inq      = h; }
void bt_client_set_inquiry_complete_handler(bt_inquiry_complete_handler_t h){cb_inq_done = h; }

void bt_client_pump(void) {
    /* If threaded pump is running, no-op. Otherwise a single non-blocking
     * read drain. */
    if (pump_running) return;
    if (ctrl_fd < 0) { ctrl_fd = connect_ctrl(); if (ctrl_fd < 0) return; }
    int fl = fcntl(ctrl_fd, F_GETFL, 0);
    if (fl >= 0) fcntl(ctrl_fd, F_SETFL, fl | O_NONBLOCK);
    char buf[4096]; int buf_len = 0;
    ssize_t n = read(ctrl_fd, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf_len = (int) n; buf[buf_len] = '\0';
    int start = 0;
    for (int i = 0; i < buf_len; i++) {
        if (buf[i] != '\n') continue;
        if (i > start) dispatch_event(&buf[start], i - start);
        start = i + 1;
    }
}
