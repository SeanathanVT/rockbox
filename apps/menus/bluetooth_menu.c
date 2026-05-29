/*
 * Bluetooth settings menu.
 *
 * Talks to the Y1's y1-btd daemon via the bt-client library
 * (firmware/target/hosted/innioasis/bluetooth/).  Top-level menu hangs
 * off the Settings menu; the scan screen pushes inquiry results from the
 * daemon's pump thread into a small mutex-protected list, and the
 * action_callback redraws the simplelist on a timer.
 */
#include "config.h"

#ifdef HAVE_BLUETOOTH

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "action.h"
#include "kernel.h"
#include "list.h"
#include "menu.h"
#include "splash.h"

#include "bluetooth/bt-client.h"
#include "exported_menus.h"

#define SCAN_MAX_DEVICES  32
#define SCAN_DURATION_S   10

struct scan_device {
    char  addr[18];   /* "AA:BB:CC:DD:EE:FF" */
    char  name[64];
    uint32_t cod;
    int8_t  rssi;
    bool   audio;
};

static struct scan_device   scan_devices[SCAN_MAX_DEVICES];
static int                  scan_count;
static atomic_int           scan_dirty;
static bool                 scan_running;
static pthread_mutex_t      scan_mtx = PTHREAD_MUTEX_INITIALIZER;

static void on_inquiry_result(const char *addr, const char *name,
                              uint32_t cod, int8_t rssi)
{
    if (!addr || !addr[0]) return;
    pthread_mutex_lock(&scan_mtx);
    for (int i = 0; i < scan_count; i++) {
        if (!strcmp(scan_devices[i].addr, addr)) {
            if (name && *name)
                snprintf(scan_devices[i].name, sizeof scan_devices[i].name,
                         "%s", name);
            scan_devices[i].rssi = rssi;
            pthread_mutex_unlock(&scan_mtx);
            atomic_fetch_add(&scan_dirty, 1);
            return;
        }
    }
    if (scan_count < SCAN_MAX_DEVICES) {
        struct scan_device *d = &scan_devices[scan_count++];
        snprintf(d->addr, sizeof d->addr, "%s", addr);
        snprintf(d->name, sizeof d->name, "%s", name ? name : "");
        d->cod   = cod;
        d->rssi  = rssi;
        d->audio = (cod & 0x200000u) != 0;
    }
    pthread_mutex_unlock(&scan_mtx);
    atomic_fetch_add(&scan_dirty, 1);
}

static void on_inquiry_complete(void)
{
    scan_running = false;
    atomic_fetch_add(&scan_dirty, 1);
}

static const char *scan_get_name(int sel, void *data, char *buf, size_t buf_sz)
{
    (void) data;
    pthread_mutex_lock(&scan_mtx);
    if (scan_count == 0) {
        snprintf(buf, buf_sz, scan_running ? "Scanning..." : "No devices found");
    } else if (sel < 0 || sel >= scan_count) {
        buf[0] = '\0';
    } else {
        struct scan_device *d = &scan_devices[sel];
        const char *label = d->name[0] ? d->name : d->addr;
        snprintf(buf, buf_sz, "%s%s", label, d->audio ? " [audio]" : "");
    }
    pthread_mutex_unlock(&scan_mtx);
    return buf;
}

static int scan_action_cb(int action, struct gui_synclist *lists)
{
    static int last_seen_dirty = -1;
    int cur = atomic_load(&scan_dirty);
    if (cur != last_seen_dirty) {
        last_seen_dirty = cur;
        pthread_mutex_lock(&scan_mtx);
        int n = scan_count > 0 ? scan_count : 1;
        pthread_mutex_unlock(&scan_mtx);
        gui_synclist_set_nb_items(lists, n);
        if (action == ACTION_NONE) action = ACTION_REDRAW;
    }
    if (action == ACTION_STD_OK) {
        pthread_mutex_lock(&scan_mtx);
        int sel = gui_synclist_get_sel_pos(lists);
        char addr[18] = "";
        if (scan_count > 0 && sel >= 0 && sel < scan_count)
            snprintf(addr, sizeof addr, "%s", scan_devices[sel].addr);
        pthread_mutex_unlock(&scan_mtx);
        if (addr[0]) {
            splashf(HZ, "Pairing %s...", addr);
            bt_client_pair_device(addr);
            return ACTION_STD_CANCEL;   /* close screen after request */
        }
    }
    return action;
}

static int bt_scan_screen(void)
{
    if (bt_client_start() < 0) {
        splashf(HZ * 2, "BT daemon not running");
        return 0;
    }
    pthread_mutex_lock(&scan_mtx);
    scan_count = 0;
    pthread_mutex_unlock(&scan_mtx);
    atomic_store(&scan_dirty, 0);
    scan_running = true;
    bt_client_set_inquiry_result_handler(on_inquiry_result);
    bt_client_set_inquiry_complete_handler(on_inquiry_complete);
    bt_client_start_inquiry(SCAN_DURATION_S);

    struct simplelist_info info;
    simplelist_info_init(&info, "Scan for devices", 1, NULL);
    info.get_name        = scan_get_name;
    info.action_callback = scan_action_cb;
    info.timeout         = HZ / 2;       /* refresh twice a second */
    simplelist_show_list(&info);

    bt_client_cancel_inquiry();
    bt_client_set_inquiry_result_handler(NULL);
    bt_client_set_inquiry_complete_handler(NULL);
    return 0;
}

static int bt_output_toggle(void)
{
    bool now = !bt_client_is_output();
    bt_client_set_output(now);
    splashf(HZ, "Output: %s", now ? "Bluetooth" : "Speaker");
    return 0;
}

MENUITEM_FUNCTION(bt_scan_item,    0, "Scan for devices",
                  bt_scan_screen,    NULL, Icon_NOICON);
MENUITEM_FUNCTION(bt_output_item,  0, "Output (toggle)",
                  bt_output_toggle,  NULL, Icon_NOICON);

MAKE_MENU(bluetooth_menu, "Bluetooth", NULL, Icon_NOICON,
          &bt_scan_item, &bt_output_item);

#endif /* HAVE_BLUETOOTH */
