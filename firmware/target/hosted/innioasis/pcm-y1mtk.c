/*
 * Innioasis Y1 — PCM stub.
 *
 * Real audio path is /dev/eac (MTK proprietary char device, misc major 10 minor 46).
 * See docs/audio-re-progress.md for the RE plan that produces the real driver.
 *
 * This stub exists so the rest of Rockbox links. It accepts PCM data and
 * silently drops it; no sound comes out. Replace `pcm_y1mtk_*` bodies once the
 * /dev/eac ioctl protocol is mapped.
 */

#include "config.h"
#include "audio.h"
#include "audiohw.h"
#include "pcm.h"
#include "pcm-internal.h"
#include "system.h"
#include "kernel.h"
#include "panic.h"

/* --- audiohw interface --- */

void audiohw_preinit(void)         { /* TODO: open /dev/eac control fd, route to headphone */ }
void audiohw_postinit(void)        { }
void audiohw_close(void)           { /* TODO: close /dev/eac fds */ }
void audiohw_set_frequency(int f)  { (void)f; }
void audiohw_set_volume(int l, int r) { (void)l; (void)r; /* TODO: ioctl set HW volume */ }
void audiohw_mute(int mute)        { (void)mute; }

/* --- pcm-internal interface --- */

void pcm_play_dma_init(void)             { }
void pcm_play_dma_postinit(void)         { }
void pcm_play_dma_start(const void *addr, size_t size) { (void)addr; (void)size; }
void pcm_play_dma_stop(void)             { }
void pcm_play_dma_pause(bool pause)      { (void)pause; }

size_t pcm_get_bytes_waiting(void)       { return 0; }
const void *pcm_play_dma_get_peak_buffer(int *count) { *count = 0; return NULL; }

void pcm_dma_apply_settings(void)        { }
