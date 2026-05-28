# Y1 Bluetooth integration

This directory provides the **Rockbox-side glue** for the y1-btd Bluetooth daemon. The daemon source + design docs live in `/work/y1-platform/bluetooth/` and `/work/y1-platform/docs/bluetooth.md`; this README only covers the integration points inside the Rockbox tree.

## Files

| File | Role |
|---|---|
| `bt-client.h` / `bt-client.c` | UNIX-socket + shm-ring client. Provides the API Rockbox calls. |
| `include/y1bt/ipc_proto.h` | Wire-format header. Vendored copy of the daemon's version; keep them byte-identical. |

The vendored `ipc_proto.h` matches the daemon's at `/work/y1-platform/bluetooth/include/y1bt/ipc_proto.h`. If the protocol changes (new ops, ring layout, etc.), update both — see § Protocol versioning below.

## Wire-up checklist

This scaffold is **not yet wired into Rockbox itself**. The remaining work is documented per integration point:

### 1. Build

Add to the Y1 target's source list (`firmware/SOURCES`, conditional on a `HAVE_BLUETOOTH` define that we set in the Y1's `config-innioasis.h`):

```
#ifdef HAVE_BLUETOOTH
target/hosted/innioasis/bluetooth/bt-client.c
#endif
```

Link against `-lpthread -lrt` (already present for the Y1 hosted target).

### 2. Startup

In `firmware/target/hosted/innioasis/system-y1mtk.c::system_init` (or wherever the hosted-target init runs late in boot):

```c
#ifdef HAVE_BLUETOOTH
#include "bluetooth/bt-client.h"
    bt_client_start();    /* tolerates daemon-not-yet-running */
#endif
```

### 3. PCM output mirror

In `firmware/target/hosted/innioasis/pcm-y1mtk.c`, inside the write loop that drives `/dev/eac`, when `bt_client_is_output()` is true, also mirror the frame to the BT ring:

```c
#ifdef HAVE_BLUETOOTH
    if (bt_client_is_output()) {
        bt_client_pcm_write(pcm_frame, n_frames);
        /* Optionally suppress the /dev/eac write to mute local output;
         * keep mirrored for now so users can hear both during testing. */
    }
#endif
```

(M3 polish: add a Rockbox setting "Output device" that flips `bt_client_set_output(true/false)`.)

### 4. Now-playing publish

In `apps/playback.c`, hook into the existing track-change event callback. Pseudocode:

```c
#ifdef HAVE_BLUETOOTH
#include "bluetooth/bt-client.h"
static void on_track_changed(struct mp3entry *id3) {
    bt_client_set_now_playing(id3->title, id3->artist, id3->album, id3->genre,
                              id3->tracknum, id3->numtracks, id3->length,
                              id3->path);
}
#endif
```

Plus a 1 Hz position tick:

```c
#ifdef HAVE_BLUETOOTH
    bt_client_set_position(audio_current_track()->elapsed);
#endif
```

Plus play/pause/stop status:

```c
#ifdef HAVE_BLUETOOTH
    bt_client_set_playback_status(playing ? "playing" : paused ? "paused" : "stopped");
#endif
```

### 5. Volume sync

Two directions:
- Rockbox-local volume change → publish to daemon: hook `sound_set_volume()` (or its host-side equivalent) and call `bt_client_set_volume(percent)`.
- Daemon-pushed volume change → apply to Rockbox: register a handler at boot:

```c
static void on_bt_volume(uint8_t pct) {
    /* schedule on the Rockbox main thread via a queue post — handler runs on
     * the IPC pump thread and Rockbox sound APIs are usually not thread-safe */
    queue_post(&y1_bt_q, BT_VOLUME_CHANGED, pct);
}
bt_client_set_volume_handler(on_bt_volume);
```

### 6. Passthrough keys

```c
static void on_bt_passthrough(const char *op, bool pressed) {
    int btn = 0;
    if      (!strcmp(op, "play") || !strcmp(op, "pause")) btn = BUTTON_PLAY;
    else if (!strcmp(op, "next"))                          btn = BUTTON_RIGHT;
    else if (!strcmp(op, "prev"))                          btn = BUTTON_LEFT;
    /* … */
    if (btn) queue_post(&button_queue, btn | (pressed ? 0 : BUTTON_REL), 0);
}
bt_client_set_passthrough_handler(on_bt_passthrough);
```

### 7. Battery push

In `powermgmt-y1.c`, when the battery reading updates:

```c
#ifdef HAVE_BLUETOOTH
    bt_client_set_battery(battery_level(), charger_inserted());
#endif
```

### 8. Settings menu

Add a new submenu under Settings → General → Bluetooth (Rockbox menu code lives in `apps/menus/`). Items: On/Off, Discoverable for 60s, Paired devices, Forget device, Output device. The menu uses `bt_client_set_pairing_mode`, `bt_client_connect_device`, etc.

## Threading model

The IPC reader runs on a dedicated pthread. Inbound-event handlers (passthrough, volume_changed, connection_state, now_playing_in) are invoked **on that thread**. Rockbox audio + sound APIs are usually not thread-safe; bounce work back to a Rockbox thread via `queue_post()` rather than calling Rockbox APIs directly from a handler.

If you'd prefer a single-threaded model, call `bt_client_pump()` periodically from a Rockbox thread instead, and skip `pthread_create` by zero-ing `pump_running` before `bt_client_start()`. (The current API always spawns the pump thread; an `bt_client_start_threaded(bool)` overload is a one-line change if needed.)

## Protocol versioning

`ipc_proto.h` defines `Y1BT_PCM_VERSION = 1`. If the on-wire shape changes (ring layout, new mandatory fields), bump that constant in **both** copies of `ipc_proto.h` (this directory's copy and the daemon's). The client's `open_ring()` rejects rings with the wrong magic; mismatched version + identical magic is on us to handle (add a `version` check there in the next iteration).
