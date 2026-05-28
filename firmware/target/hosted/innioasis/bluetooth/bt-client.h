/*
 * bt-client.h — Rockbox-side glue for the y1-btd Bluetooth daemon.
 *
 * Rockbox calls into this header to publish playback state, ingest BT key
 * events back, and route PCM frames to the daemon's shared-memory ring.
 *
 * Daemon and protocol live in /work/y1-platform/bluetooth/; the on-wire
 * contract is in <y1bt/ipc_proto.h> (shipped alongside this header so
 * Rockbox does not depend on y1-platform/ at build time — see
 * firmware/target/hosted/innioasis/bluetooth/include/y1bt/ipc_proto.h).
 */
#ifndef Y1_BT_CLIENT_H
#define Y1_BT_CLIENT_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Open the control socket + attach to the shared-memory PCM ring.
 * Idempotent. Returns 0 on success, -1 if the daemon isn't running
 * (caller can retry later). */
int  bt_client_start(void);
void bt_client_stop(void);

/* Are we currently routed to BT output? */
bool bt_client_is_output(void);
void bt_client_set_output(bool on);

/* Publish from playback engine on each track change. NULL strings → "". */
void bt_client_set_now_playing(const char *title, const char *artist,
                                const char *album, const char *genre,
                                uint32_t track_num, uint32_t num_tracks,
                                uint32_t length_ms, const char *path);

/* status: "playing" | "paused" | "stopped" | "fwd_seek" | "rev_seek" */
void bt_client_set_playback_status(const char *status);
void bt_client_set_position(uint32_t position_ms);
void bt_client_set_volume(uint8_t volume_percent);
void bt_client_set_battery(uint8_t percent, bool charging);

/* Pairing / connection control (used by the Bluetooth settings menu). */
void bt_client_set_pairing_mode(bool discoverable, uint16_t timeout_s);
void bt_client_connect_device(const char *bd_addr);
void bt_client_disconnect_device(const char *bd_addr);
void bt_client_forget_device(const char *bd_addr);

/* PCM mirror: call from pcm-y1mtk.c's write loop when BT output is selected.
 * Writes up to `frames` interleaved stereo S16LE frames into the shared ring;
 * returns frames consumed (may be < frames if the ring is full, in which case
 * caller should drop / block as appropriate). */
size_t bt_client_pcm_write(const int16_t *samples, size_t frames);

/* --- Inbound events (delivered from the daemon to Rockbox). Hook these
 * from your event-source of choice; the client lib decodes the JSON and
 * invokes the registered handler. */
typedef void (*bt_passthrough_handler_t)(const char *op, bool pressed);
typedef void (*bt_volume_handler_t)(uint8_t volume_percent);
typedef void (*bt_connection_handler_t)(const char *addr,
                                         const char *profile,
                                         const char *state);
typedef void (*bt_now_playing_in_handler_t)(const char *title,
                                              const char *artist,
                                              const char *album,
                                              uint32_t length_ms,
                                              const char *art_path);

void bt_client_set_passthrough_handler(bt_passthrough_handler_t h);
void bt_client_set_volume_handler(bt_volume_handler_t h);
void bt_client_set_connection_handler(bt_connection_handler_t h);
void bt_client_set_now_playing_in_handler(bt_now_playing_in_handler_t h);

/* The client lib runs its IPC pump from a dedicated pthread. If the host
 * prefers single-threaded service, call this periodically instead and pass
 * pump_threaded=false to bt_client_start(). */
void bt_client_pump(void);

#endif /* Y1_BT_CLIENT_H */
