# Y1 Bluetooth integration

Rockbox-side glue for the `y1-btd` Bluetooth daemon.  Daemon source + architecture doc: `/work/y1-platform/bluetooth/` and `/work/y1-platform/docs/bluetooth.md`.

## Files

| File | Role |
|---|---|
| `bt-client.h` / `bt-client.c` | UNIX-socket + shm-ring client.  Provides the API Rockbox calls into. |
| `include/y1bt/ipc_proto.h` | Wire-format header.  Vendored copy of the daemon's version — **keep byte-identical**. |

When the protocol changes (new ops, ring layout), update both copies and bump `Y1BT_PCM_VERSION` in `ipc_proto.h` if the shm ring shape moves.

## Build wiring (already in tree)

| Where | Edit |
|---|---|
| `firmware/export/config/innioasisy1.h` | `#define HAVE_BLUETOOTH` |
| `firmware/SOURCES` | adds `target/hosted/innioasis/bluetooth/bt-client.c` under `#ifdef HAVE_BLUETOOTH` |
| `apps/SOURCES` | adds `menus/bluetooth_menu.c` under `#ifdef HAVE_BLUETOOTH` |
| `apps/menus/exported_menus.h` | declares `bluetooth_menu` |
| `apps/menus/main_menu.c` | links `&bluetooth_menu` into the Settings menu |
| `tools/configure` | adds `target/hosted/innioasis/bluetooth/include` to `TARGET_INC` so `<y1bt/ipc_proto.h>` resolves from both apps + firmware trees |

`-lpthread -lrt` are already on the Y1 hosted-target link line.

## What's wired

- **Pixel-style Bluetooth menu** (`apps/menus/bluetooth_menu.c`) — one list: on/off (radio power via `set_enabled`), "My Devices" (OK = connect/disconnect, long-press = forget; from `list_devices`), "Available Devices" (scan + pair-on-select). Inquiry, paired-list, and connection events stream from the daemon's pump thread.
- **Automatic output routing** — the bt-client pump flips `bt_client_set_output()` on A2DP `connection_state` (BT > headphones > speaker; falls back on disconnect).
- **PCM mirror** — `pcm-y1mtk.c` feeds the mixed 44.1 kHz S16 buffer to `bt_client_pcm_write()` when BT output is active (local amps muted; the `/dev/eac` write paces).

`bt_client_start()` is idempotent and called on menu entry; the daemon doesn't need to be running at Rockbox boot.

## Still to wire

| Hook | Where | Why |
|---|---|---|
| Now-playing publish | `apps/playback.c` track-change handler → `bt_client_set_now_playing(...)` | Drives AVRCP metadata to the speaker/car. |
| Position tick | 1 Hz from playback engine → `bt_client_set_position(elapsed_ms)` | AVRCP position. |
| Playback status | play/pause/stop → `bt_client_set_playback_status(...)` | AVRCP status. |
| Volume sync (out) | local `sound_set_volume()` → `bt_client_set_volume(percent)` | Sink sees host-side changes. |
| Volume sync (in) | `bt_client_set_volume_handler(...)` → bounce to Rockbox thread → apply | Absolute Volume from sink/CT. |
| Passthrough keys | `bt_client_set_passthrough_handler(...)` → translate to BUTTON_* | Speaker buttons control playback. |
| Battery push | `powermgmt-y1.c` battery update → `bt_client_set_battery(pct, charging)` | AVRCP battery attribute / GATT BAS. |
| Persist enabled + boot start | a `settings_list.c` flag + app-init `bt_client_start()` | Survive reboot; route without opening the menu first. |

## Threading model

The IPC reader runs on a dedicated pthread.  Inbound-event handlers (passthrough, volume_changed, connection_state, now_playing_in, inquiry_result) fire **on that thread**.  Rockbox audio + sound APIs are not generally thread-safe — bounce work back to a Rockbox thread via `queue_post()` rather than calling Rockbox APIs directly from a handler.

If you'd prefer a single-threaded model, call `bt_client_pump()` periodically from a Rockbox thread instead; the current API always spawns the pump thread.
