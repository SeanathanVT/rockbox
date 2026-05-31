# Y1 Bluetooth integration

Rockbox-side glue for the `y1-btd` Bluetooth daemon.  The daemon (HCI + BTstack profiles) and its architecture doc live in a separate repository.

## Files

| File | Role |
|---|---|
| `bt-client.h` / `bt-client.c` | UNIX-socket + shm-ring client.  Provides the API Rockbox calls into. |
| `bt-backend.c` | Implements the device-agnostic `bt_backend_*` interface (`firmware/export/bluetooth_backend.h`) over `bt-client`: AVRCP publish/remote-control (mapping the generic key/status enums ↔ `Y1BT_*` wire constants) **and** the device discovery/management ops + observer for the menu.  The shared AVRCP glue (`apps/bluetooth_avrcp.c`) and the BT menu (`apps/menus/bluetooth_menu.c`) talk only to that interface, never to this target. |
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
- **PCM mirror** — `pcm-y1mtk.c` feeds the mixed 44.1 kHz S16 buffer to `bt_client_pcm_write()` when BT output is active (local amps muted; the `/dev/eac` write paces). The SW scaler is pinned to unity while BT is the active output, so the stream is full-scale and the sink's absolute volume is the only attenuation.
- **AVRCP glue** (`apps/bluetooth_avrcp.c`) — publishes now-playing (on each track change **and** when a sink connects, so a CHANGED-driven head unit's pane isn't blank until the next track), playback status, 1 Hz position, volume, and battery to the daemon; applies inbound passthrough keys (play/pause/next/prev/ff/rew via the direct `audio_*` API). Volume is **per-output**: while BT is active the volume tracks/commands the sink (absolute volume), and the local speaker/headphone level is snapshotted on entry and restored on exit (`poll_output_route` keyed on `bt_backend_audio_active()`), so the sink's level never bleeds into local playback. Inbound events arrive on the bt-client pump pthread and hop to a Rockbox dispatcher thread via a lock-free ring.

`bt_client_start()` is idempotent; the AVRCP dispatcher (started at app init) retries it until the daemon's ring exists, so playback control + audio routing work without opening the BT menu first.

## Still to wire

| Hook | Where | Why |
|---|---|---|
| Cover art (v2) | BIP responder on `goep_server.c` | Album art to the car display. |

## Threading model

The IPC reader runs on a dedicated pthread.  Inbound-event handlers (passthrough, volume_changed, connection_state, now_playing_in, inquiry_result) fire **on that thread**.  Rockbox audio + sound APIs are not generally thread-safe — bounce work back to a Rockbox thread via `queue_post()` rather than calling Rockbox APIs directly from a handler.

If you'd prefer a single-threaded model, call `bt_client_pump()` periodically from a Rockbox thread instead; the current API always spawns the pump thread.
