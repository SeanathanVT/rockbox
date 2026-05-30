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

#ifdef HAVE_BLUETOOTH

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "action.h"
#include "kernel.h"
#include "list.h"
#include "menu.h"
#include "settings.h"
#include "splash.h"
#include "yesno.h"

#include "bluetooth_backend.h"
#include "exported_menus.h"

#define MAX_PAIRED       16
#define MAX_AVAIL        32
#define SCAN_DURATION_S  20

struct bt_dev {
    char addr[18];        /* "AA:BB:CC:DD:EE:FF" */
    char name[64];
    bool connected;
    bool audio;
};

static struct bt_dev    paired[MAX_PAIRED];
static int              paired_n;
static struct bt_dev    avail[MAX_AVAIL];
static int              avail_n;
static bool             bt_enabled = true;   /* mirrors global_status.bluetooth_enabled */
static bool             scanning;
static atomic_int       dirty;
static pthread_mutex_t  mtx = PTHREAD_MUTEX_INITIALIZER;

/* Pending SSP numeric-comparison prompt (filled on the backend thread, drained
 * by the UI thread in action_cb). */
static char             pair_addr[18];
static char             pair_name[64];
static uint32_t         pair_code;
static atomic_bool      pair_pending;

/* Row model -- rebuilt from the tables by the UI thread only. */
enum row_type { R_TOGGLE, R_HDR_MY, R_PAIRED, R_HDR_AVAIL, R_SCANCTL, R_AVAIL };
struct row { uint8_t type; int idx; };
static struct row rows[2 + MAX_PAIRED + 2 + MAX_AVAIL];
static int        rows_n;

/* ---- backend observer callbacks (may run on a backend thread) ---- */

static void on_inquiry_result(const char *addr, const char *name,
                              uint32_t cod, int8_t rssi)
{
    (void) rssi;
    if (!addr || !addr[0]) return;
    pthread_mutex_lock(&mtx);
    int i;
    for (i = 0; i < avail_n; i++)
        if (!strcmp(avail[i].addr, addr)) break;
    if (i == avail_n && avail_n < MAX_AVAIL) {
        avail_n++;
        snprintf(avail[i].addr, sizeof(avail[i].addr), "%s", addr);
        avail[i].audio = (cod & 0x200000u) != 0;
        avail[i].name[0] = '\0';
    }
    if (i < MAX_AVAIL && name && *name)
        snprintf(avail[i].name, sizeof(avail[i].name), "%s", name);
    pthread_mutex_unlock(&mtx);
    atomic_fetch_add(&dirty, 1);
}

static void on_inquiry_complete(void)
{
    scanning = false;
    atomic_fetch_add(&dirty, 1);
}

static void on_paired_begin(void)
{
    pthread_mutex_lock(&mtx);
    paired_n = 0;
    pthread_mutex_unlock(&mtx);
}

static void on_paired_device(const char *addr, const char *name, bool connected)
{
    if (!addr || !addr[0]) return;
    pthread_mutex_lock(&mtx);
    if (paired_n < MAX_PAIRED) {
        struct bt_dev *d = &paired[paired_n++];
        snprintf(d->addr, sizeof(d->addr), "%s", addr);
        snprintf(d->name, sizeof(d->name), "%s", name ? name : "");
        d->connected = connected;
        d->audio = false;
    }
    pthread_mutex_unlock(&mtx);
}

static void on_paired_done(void)
{
    atomic_fetch_add(&dirty, 1);
}

/* A connect/disconnect happened -- re-fetch the authoritative paired list
 * (its connected flags come from the backend). */
static void on_connection_changed(void)
{
    bt_backend_request_devices();
    atomic_fetch_add(&dirty, 1);
}

/* SSP numeric-comparison prompt (backend thread): stash it for the UI thread. */
static void on_pairing_request(const char *addr, const char *name, uint32_t code)
{
    if (!addr || !addr[0]) return;
    pthread_mutex_lock(&mtx);
    snprintf(pair_addr, sizeof(pair_addr), "%s", addr);
    snprintf(pair_name, sizeof(pair_name), "%s", (name && name[0]) ? name : addr);
    pair_code = code;
    pthread_mutex_unlock(&mtx);
    atomic_store(&pair_pending, true);
}

static const struct bt_device_observer menu_observer = {
    .scan_result        = on_inquiry_result,
    .scan_complete      = on_inquiry_complete,
    .paired_begin       = on_paired_begin,
    .paired_device      = on_paired_device,
    .paired_done        = on_paired_done,
    .connection_changed = on_connection_changed,
};

/* ---- list rendering (UI thread) ---- */

static void build_rows(void)
{
    rows_n = 0;
    rows[rows_n].type = R_TOGGLE; rows[rows_n].idx = 0; rows_n++;
    if (!bt_enabled) return;

    pthread_mutex_lock(&mtx);
    int pn = paired_n, an = avail_n;
    pthread_mutex_unlock(&mtx);

    if (pn > 0) {
        rows[rows_n].type = R_HDR_MY; rows[rows_n].idx = 0; rows_n++;
        for (int i = 0; i < pn; i++) {
            rows[rows_n].type = R_PAIRED; rows[rows_n].idx = i; rows_n++;
        }
    }
    rows[rows_n].type = R_HDR_AVAIL; rows[rows_n].idx = 0; rows_n++;
    rows[rows_n].type = R_SCANCTL;   rows[rows_n].idx = 0; rows_n++;
    for (int i = 0; i < an; i++) {
        rows[rows_n].type = R_AVAIL; rows[rows_n].idx = i; rows_n++;
    }
}

static const char *get_name(int sel, void *data, char *buf, size_t buf_sz)
{
    (void) data;
    if (sel < 0 || sel >= rows_n) { buf[0] = '\0'; return buf; }
    struct row r = rows[sel];
    pthread_mutex_lock(&mtx);
    switch (r.type) {
    case R_TOGGLE:
        snprintf(buf, buf_sz, "Bluetooth:  %s", bt_enabled ? "On" : "Off");
        break;
    case R_HDR_MY:
        snprintf(buf, buf_sz, "- My Devices -");
        break;
    case R_PAIRED:
        if (r.idx < paired_n) {
            struct bt_dev *d = &paired[r.idx];
            snprintf(buf, buf_sz, "%s  [%s]",
                     d->name[0] ? d->name : d->addr,
                     d->connected ? "Connected" : "Paired");
        } else buf[0] = '\0';
        break;
    case R_HDR_AVAIL:
        snprintf(buf, buf_sz, "- Available Devices -");
        break;
    case R_SCANCTL:
        snprintf(buf, buf_sz, scanning ? "Scanning... (stop)"
                                       : "Scan for devices");
        break;
    case R_AVAIL:
        if (r.idx < avail_n) {
            struct bt_dev *d = &avail[r.idx];
            snprintf(buf, buf_sz, "%s%s", d->name[0] ? d->name : d->addr,
                     d->audio ? "  [audio]" : "");
        } else buf[0] = '\0';
        break;
    default:
        buf[0] = '\0';
        break;
    }
    pthread_mutex_unlock(&mtx);
    return buf;
}

static void toggle_bluetooth(void)
{
    bt_enabled = !bt_enabled;
    global_status.bluetooth_enabled = bt_enabled;
    status_save(true);                  /* remember across reboots */
    bt_backend_set_enabled(bt_enabled);
    if (bt_enabled) {
        bt_backend_request_devices();
    } else {
        pthread_mutex_lock(&mtx);
        paired_n = 0; avail_n = 0; scanning = false;
        pthread_mutex_unlock(&mtx);
    }
    atomic_fetch_add(&dirty, 1);
}

static void toggle_scan(void)
{
    if (scanning) {
        bt_backend_scan_stop();
        scanning = false;
    } else {
        pthread_mutex_lock(&mtx);
        avail_n = 0;
        pthread_mutex_unlock(&mtx);
        bt_backend_scan_start(SCAN_DURATION_S);
        scanning = true;
    }
    atomic_fetch_add(&dirty, 1);
}

static int action_cb(int action, struct gui_synclist *lists)
{
    static int last_dirty = -1;
    int cur = atomic_load(&dirty);
    if (cur != last_dirty) {
        last_dirty = cur;
        build_rows();
        gui_synclist_set_nb_items(lists, rows_n);
        if (action == ACTION_NONE) action = ACTION_REDRAW;
    }

    /* A pairing prompt (SSP numeric comparison) arrived from the backend:
     * show the code so the user can check it matches the other device. */
    if (atomic_exchange(&pair_pending, false)) {
        char addr[18], name[64], prompt[96];
        uint32_t code;
        pthread_mutex_lock(&mtx);
        snprintf(addr, sizeof(addr), "%s", pair_addr);
        snprintf(name, sizeof(name), "%s", pair_name);
        code = pair_code;
        pthread_mutex_unlock(&mtx);
        snprintf(prompt, sizeof(prompt), "Pair %s?  Code %06u", name,
                 (unsigned) code);
        bt_backend_pairing_confirm(addr, yesno_pop(prompt));
        return ACTION_REDRAW;
    }

    int sel = gui_synclist_get_sel_pos(lists);
    struct row r = (sel >= 0 && sel < rows_n) ? rows[sel]
                                              : (struct row){ R_HDR_MY, 0 };

    if (action == ACTION_STD_OK) {
        char addr[18] = "";
        bool was_connected = false;
        pthread_mutex_lock(&mtx);
        if (r.type == R_PAIRED && r.idx < paired_n) {
            snprintf(addr, sizeof(addr), "%s", paired[r.idx].addr);
            was_connected = paired[r.idx].connected;
        } else if (r.type == R_AVAIL && r.idx < avail_n) {
            snprintf(addr, sizeof(addr), "%s", avail[r.idx].addr);
        }
        pthread_mutex_unlock(&mtx);

        switch (r.type) {
        case R_TOGGLE:
            toggle_bluetooth();
            break;
        case R_PAIRED:
            if (!addr[0]) break;
            if (was_connected) {
                bt_backend_disconnect(addr);
            } else {
                splashf(HZ, "Connecting %s", addr);
                bt_backend_connect(addr);
            }
            break;
        case R_SCANCTL:
            toggle_scan();
            break;
        case R_AVAIL:
            if (addr[0]) {
                splashf(HZ, "Pairing %s", addr);
                bt_backend_pair(addr);
            }
            break;
        default:
            break;
        }
        return ACTION_REDRAW;
    }

    if (action == ACTION_STD_CONTEXT && r.type == R_PAIRED) {
        char addr[18] = "", name[64] = "";
        pthread_mutex_lock(&mtx);
        if (r.idx < paired_n) {
            snprintf(addr, sizeof(addr), "%s", paired[r.idx].addr);
            snprintf(name, sizeof(name), "%s",
                     paired[r.idx].name[0] ? paired[r.idx].name : paired[r.idx].addr);
        }
        pthread_mutex_unlock(&mtx);
        if (addr[0]) {
            char prompt[80];
            snprintf(prompt, sizeof(prompt), "Forget %s?", name);
            if (yesno_pop(prompt)) {
                bt_backend_forget(addr);
                splashf(HZ, "Forgot %s", name);
                bt_backend_request_devices();
            }
            atomic_fetch_add(&dirty, 1);
        }
        return ACTION_REDRAW;
    }

    return action;
}

static int bt_main_screen(void)
{
    if (!bt_backend_start()) {
        splashf(HZ * 2, "Bluetooth not available");
        return 0;
    }
    pthread_mutex_lock(&mtx);
    paired_n = 0; avail_n = 0; scanning = false;
    pthread_mutex_unlock(&mtx);
    atomic_store(&dirty, 0);

    bt_enabled = global_status.bluetooth_enabled != 0;
    atomic_store(&pair_pending, false);
    bt_backend_set_device_observer(&menu_observer);
    bt_backend_set_pairing_handler(on_pairing_request);
    if (bt_enabled) bt_backend_request_devices();

    build_rows();

    struct simplelist_info info;
    simplelist_info_init(&info, "Bluetooth", rows_n, NULL);
    info.get_name        = get_name;
    info.action_callback = action_cb;
    info.timeout         = HZ / 2;
    simplelist_show_list(&info);

    if (scanning) { bt_backend_scan_stop(); scanning = false; }
    bt_backend_set_device_observer(NULL);     /* automatic output routing is
                                                 internal to the backend and
                                                 stays active */
    bt_backend_set_pairing_handler(NULL);     /* no UI to confirm once closed */
    return 0;
}

MENUITEM_FUNCTION(bluetooth_menu, 0, "Bluetooth",
                  bt_main_screen, NULL, Icon_NOICON);

#endif /* HAVE_BLUETOOTH */
