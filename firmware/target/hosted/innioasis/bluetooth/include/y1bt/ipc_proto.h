/*
 * y1bt — IPC contract between Rockbox and y1-btd.
 *
 * Two channels:
 *   1. Control: line-delimited UTF-8 JSON over UNIX SOCK_STREAM at Y1BT_CTRL_SOCK_PATH.
 *   2. PCM audio: SPSC lock-free ring in shared memory at Y1BT_PCM_RING_PATH.
 *
 * Both Rockbox and y1-btd include this file unchanged.
 */
#ifndef Y1BT_IPC_PROTO_H
#define Y1BT_IPC_PROTO_H

#include <stdint.h>
#include <stdatomic.h>

/* Persistent state (link keys, NVRAM, config, art cache) lives under
 * /storage/sdcard1/.btd/ — the internal SD, FAT32, mounted into the
 * Rockbox chroot at the same path. Survives SYSTEM reflash; debuggable
 * by pulling the card.
 *
 * Volatile runtime state (UNIX socket, PCM ring) lives on the SYSTEM
 * ext4 — FAT32 cannot host special-file types like AF_UNIX sockets, so
 * the IPC socket MUST be on ext4. /tmp/ exists inside the chroot. */
/* Y1BT_STATE_DIR is on the SD card and holds user-visible state (config,
 * cover-art transfer buffers, NVRAM seed).  Daemon accesses these via
 * short-lived open/read/close cycles only -- any persistent fd here would
 * block Rockbox's USB-MSC `umount(SD)`.  Long-lived fds go in
 * Y1BT_PERSIST_DIR on SYSTEM ext4 instead (link-key DB, HCI dump). */
#define Y1BT_STATE_DIR        "/storage/sdcard1/.btd"
#define Y1BT_PERSIST_DIR      "/var/lib/y1bt"
#define Y1BT_CTRL_SOCK_PATH   "/tmp/y1btd.sock"
#define Y1BT_PCM_RING_PATH    "/tmp/y1btd-pcm"
#define Y1BT_ART_IN_DIR       Y1BT_STATE_DIR   "/art-in"
#define Y1BT_ART_OUT_DIR      Y1BT_STATE_DIR   "/art-out"
#define Y1BT_CONFIG_PATH      Y1BT_STATE_DIR   "/y1bt.conf"
#define Y1BT_NVRAM_PATH       Y1BT_STATE_DIR   "/btnvram.bin"

/* ----- PCM ring ----- */

#define Y1BT_PCM_MAGIC        0x52503159u   /* 'Y1PR' LE */
#define Y1BT_PCM_VERSION      1u
#define Y1BT_PCM_CHANNELS     2u
#define Y1BT_PCM_SAMPLE_RATE  44100u
#define Y1BT_PCM_FORMAT_S16LE 1u
#define Y1BT_PCM_FRAMES       16384u  /* ~372 ms */

struct y1bt_pcm_ring {
    uint32_t magic;
    uint32_t version;
    uint32_t channels;
    uint32_t sample_rate;
    uint32_t sample_format;
    uint32_t capacity_frames;
    uint32_t reserved0;
    uint32_t reserved1;
    _Atomic uint32_t head;            /* producer writes head, consumer reads */
    _Atomic uint32_t tail;            /* consumer writes tail, producer reads */
    uint8_t  _pad_to_64[64 - 2*sizeof(_Atomic uint32_t)];
    int16_t  samples[];               /* capacity_frames * channels */
};

#define Y1BT_PCM_RING_BYTES \
    (sizeof(struct y1bt_pcm_ring) + \
     (size_t)Y1BT_PCM_FRAMES * Y1BT_PCM_CHANNELS * sizeof(int16_t))

/* ----- Control messages -----
 *
 * Every message is one JSON object per line. Common envelope:
 *   { "op": "<name>", "id": <u16|null>, "p": { ... } }       (request)
 *   { "ok": true|false, "id": <u16>, "r": { ... } }          (response)
 *   { "event": "<name>", "p": { ... } }                      (push)
 *
 * id=null requests do not expect a response.
 * Unknown ops respond with { "ok": false, "id": .., "r": { "err": "unknown_op" } }.
 *
 * Schemas are documented in y1-platform/docs/bluetooth.md § IPC contract.
 * Constants below are the canonical op-name strings; both sides should use them.
 */

#define Y1BT_OP_INQUIRY_START         "inquiry_start"
#define Y1BT_OP_INQUIRY_CANCEL        "inquiry_cancel"
#define Y1BT_OP_PAIR_DEVICE           "pair_device"
#define Y1BT_OP_SET_NOW_PLAYING       "set_now_playing"
#define Y1BT_OP_SET_PLAYBACK_STATUS   "set_playback_status"
#define Y1BT_OP_SET_POSITION          "set_position"
#define Y1BT_OP_SET_VOLUME            "set_volume"
#define Y1BT_OP_SET_BATTERY           "set_battery"
#define Y1BT_OP_SET_OUTPUT_MODE       "set_output_mode"
#define Y1BT_OP_SET_PAIRING_MODE      "set_pairing_mode"
#define Y1BT_OP_CONNECT_DEVICE        "connect_device"
#define Y1BT_OP_DISCONNECT_DEVICE     "disconnect_device"
#define Y1BT_OP_FORGET_DEVICE         "forget_device"
#define Y1BT_OP_LIST_DEVICES          "list_devices"
#define Y1BT_OP_GET_STATE             "get_state"
#define Y1BT_OP_PAIRING_CONFIRM       "pairing_confirm"

#define Y1BT_EVENT_INQUIRY_RESULT     "inquiry_result"
#define Y1BT_EVENT_INQUIRY_COMPLETE   "inquiry_complete"
#define Y1BT_EVENT_PASSTHROUGH        "passthrough"
#define Y1BT_EVENT_VOLUME_CHANGED     "volume_changed"
#define Y1BT_EVENT_CONNECTION_STATE   "connection_state"
#define Y1BT_EVENT_PAIRING_REQUEST    "pairing_request"
#define Y1BT_EVENT_NOW_PLAYING_IN     "now_playing_in"
#define Y1BT_EVENT_POSITION_IN        "position_in"
#define Y1BT_EVENT_DAEMON_READY       "daemon_ready"

/* Playback status values (mirrors AVRCP Playback Status). */
#define Y1BT_PB_STOPPED      "stopped"
#define Y1BT_PB_PLAYING      "playing"
#define Y1BT_PB_PAUSED       "paused"
#define Y1BT_PB_FWD_SEEK     "fwd_seek"
#define Y1BT_PB_REV_SEEK     "rev_seek"

/* Passthrough op identifiers. */
#define Y1BT_PT_PLAY         "play"
#define Y1BT_PT_PAUSE        "pause"
#define Y1BT_PT_STOP         "stop"
#define Y1BT_PT_NEXT         "next"
#define Y1BT_PT_PREV         "prev"
#define Y1BT_PT_FF           "ff"
#define Y1BT_PT_RW           "rw"
#define Y1BT_PT_VOLUME_UP    "vol_up"
#define Y1BT_PT_VOLUME_DOWN  "vol_down"
#define Y1BT_PT_MUTE         "mute"

/* Output modes. */
#define Y1BT_MODE_LOCAL          "local"
#define Y1BT_MODE_BT_SOURCE      "bluetooth_source"
#define Y1BT_MODE_BT_SINK        "bluetooth_sink"

#endif /* Y1BT_IPC_PROTO_H */
