/*
 * Innioasis Y1 — PCM driver for MediaTek MT6572 /dev/eac.
 *
 * The kernel-side audio is the MTK "EAC" character device — `misc 10:46`,
 * magic 'C' ioctls. Userspace flow (matches `AudDrv_ioctl.h` in the
 * MT6572 BSP kernel, cross-referenced against /system/lib/libaudio.primary
 * .default.so — see `docs/audio-stack.md`):
 *
 *   open("/dev/eac", O_RDWR)
 *   ioctl(fd, ALLOCATE_MEMIF_DL1, buffer_size)   // alloc DRAM ring buffer
 *   ioctl(fd, SET_HEADPHONE_ON,   1)             // route to headphone-out
 *   ioctl(fd, START_MEMIF_TYPE,   MEM_DL1=0)     // arm the AFE chain
 *   write(fd, pcm_buf, len)                      // blocking — copy_from_user
 *                                                // into kernel ring; AFE DMA
 *                                                // pulls from the same ring.
 *   ...
 *   ioctl(fd, STANDBY_MEMIF_TYPE, MEM_DL1=0)
 *   ioctl(fd, SET_HEADPHONE_OFF, 0)
 *   ioctl(fd, FREE_MEMIF_DL1)
 *   close(fd)
 *
 * The kernel's `AUDDRV_DL1_MAX_BUFFER_LENGTH` is 0x4000 (16 KB) — we ask for
 * 8 KB which gives ~46 ms of 44.1k S16_LE stereo. The kernel will scale down
 * silently if we ask for more.
 *
 * Sample rate / channel / bitwidth: the kernel driver gets these from the
 * AFE register writes done elsewhere (the HAL uses `AudioAnalogReg` for the
 * codec setup). For a first cut we trust the stock register values left
 * over from Android boot; codec re-init via `SET_ANAAFE_REG` is a follow-up.
 */

#include "config.h"
#include "audio.h"
#include "audiohw.h"
#include "pcm.h"
#include "pcm-internal.h"
#include "system.h"
#include "kernel.h"
#include "panic.h"
#include "logf.h"

#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <errno.h>

/* -------------------------------------------------------------------------- */
/* MTK AUD_DRV ioctl numbers (from mediatek/platform/mt6572/kernel/drivers/   */
/* sound/AudDrv_ioctl.h — magic = 'C').                                       */

#define AUD_DRV_IOC_MAGIC       'C'
#define _AUD_IOWR(nr, sz)       (((3U)<<30) | ((sz)<<16) | (AUD_DRV_IOC_MAGIC<<8) | (nr))
#define _AUD_IOW(nr, sz)        (((1U)<<30) | ((sz)<<16) | (AUD_DRV_IOC_MAGIC<<8) | (nr))

#define ALLOCATE_MEMIF_DL1      _AUD_IOWR(0x10, 4)  /* arg = buf size (bytes)  */
#define FREE_MEMIF_DL1          _AUD_IOWR(0x11, 4)
#define AUD_RESTART             _AUD_IOWR(0x1F, 4)
#define START_MEMIF_TYPE        _AUD_IOWR(0x20, 4)  /* arg = MEMIF_BUFFER_TYPE */
#define STANDBY_MEMIF_TYPE      _AUD_IOWR(0x21, 4)
#define SET_HEADPHONE_ON        _AUD_IOW (0xa4, 4)
#define SET_HEADPHONE_OFF       _AUD_IOW (0xa5, 4)

/* MEMIF_BUFFER_TYPE (from AudDrv_Kernel.h) */
#define MEM_DL1                 0

/* Kernel ring buffer length we request. Kernel caps at 0x4000 (16 KB). */
#define EAC_DL1_BUFFER_BYTES    0x2000  /* 8 KB == ~46 ms @ 44.1k S16 stereo */

/* -------------------------------------------------------------------------- */

static int eac_fd = -1;
static bool dl1_allocated  = false;
static bool route_active   = false;
static bool stream_running = false;

static pthread_t       worker_tid;
static pthread_mutex_t worker_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  worker_cv  = PTHREAD_COND_INITIALIZER;

static const void *cur_addr = NULL;
static size_t      cur_size = 0;
static volatile bool worker_run    = false;
static volatile bool worker_quit   = false;
static volatile bool worker_paused = false;

/* -------------------------------------------------------------------------- */
/* MEMIF lifecycle.                                                           */

static void eac_alloc_dl1(void)
{
    if (dl1_allocated || eac_fd < 0)
        return;
    if (ioctl(eac_fd, ALLOCATE_MEMIF_DL1, EAC_DL1_BUFFER_BYTES) < 0)
    {
        logf("eac ALLOC_DL1 %d: %s", EAC_DL1_BUFFER_BYTES, strerror(errno));
        return;
    }
    dl1_allocated = true;
}

static void eac_free_dl1(void)
{
    if (!dl1_allocated || eac_fd < 0)
        return;
    ioctl(eac_fd, FREE_MEMIF_DL1, 0);
    dl1_allocated = false;
}

static void eac_route_headphone(bool on)
{
    if (eac_fd < 0)
        return;
    if (on == route_active)
        return;
    int v = on ? 1 : 0;
    int cmd = on ? SET_HEADPHONE_ON : SET_HEADPHONE_OFF;
    if (ioctl(eac_fd, cmd, v) < 0)
        logf("eac route hp=%d: %s", on, strerror(errno));
    route_active = on;
}

static void stream_start(void)
{
    if (stream_running || eac_fd < 0)
        return;
    eac_alloc_dl1();
    eac_route_headphone(true);
    if (ioctl(eac_fd, START_MEMIF_TYPE, MEM_DL1) < 0)
        logf("eac START_MEMIF: %s", strerror(errno));
    stream_running = true;
}

static void stream_stop(void)
{
    if (!stream_running || eac_fd < 0)
        return;
    ioctl(eac_fd, STANDBY_MEMIF_TYPE, MEM_DL1);
    stream_running = false;
}

/* -------------------------------------------------------------------------- */
/* Worker thread: blocking write(2) loop pulling buffers via Rockbox's        */
/* pcm_play_dma_complete_callback() get-more interface.                       */

static void *worker_main(void *arg)
{
    (void)arg;

    while (!worker_quit)
    {
        pthread_mutex_lock(&worker_mtx);
        while (!worker_run && !worker_quit)
            pthread_cond_wait(&worker_cv, &worker_mtx);

        const void *addr = cur_addr;
        size_t      size = cur_size;
        cur_addr = NULL;
        cur_size = 0;
        pthread_mutex_unlock(&worker_mtx);

        if (worker_quit)
            break;

        while (size > 0 && !worker_quit)
        {
            while (worker_paused && !worker_quit)
                usleep(2000);

            ssize_t n = write(eac_fd, addr, size);
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                logf("eac write: %s", strerror(errno));
                break;
            }
            addr = (const uint8_t *)addr + n;
            size -= (size_t)n;
        }

        const void *next_addr = NULL;
        size_t      next_size = 0;
        bool more = pcm_play_dma_complete_callback(PCM_DMAST_OK,
                                                   &next_addr, &next_size);
        pthread_mutex_lock(&worker_mtx);
        if (more && next_addr && next_size)
        {
            cur_addr   = next_addr;
            cur_size   = next_size;
            worker_run = true;
        }
        else
        {
            worker_run = false;
        }
        pthread_mutex_unlock(&worker_mtx);
    }

    return NULL;
}

/* -------------------------------------------------------------------------- */
/* audiohw interface                                                          */

void audiohw_preinit(void)
{
    if (eac_fd >= 0)
        return;

    eac_fd = open("/dev/eac", O_RDWR);
    if (eac_fd < 0)
    {
        logf("open /dev/eac: %s", strerror(errno));
        return;
    }

    /* `AUD_RESTART` (0x1F) is the HAL's hardware-init handshake; the kernel
     * handler is a kitchen-sink clock+AFE reset. Best-effort — failure here
     * isn't fatal because most state is also set by ALLOCATE_MEMIF_DL1. */
    ioctl(eac_fd, AUD_RESTART, 0);
}

void audiohw_postinit(void)
{
}

void audiohw_close(void)
{
    pthread_mutex_lock(&worker_mtx);
    worker_quit = true;
    worker_run  = false;
    pthread_cond_signal(&worker_cv);
    pthread_mutex_unlock(&worker_mtx);
    pthread_join(worker_tid, NULL);

    stream_stop();
    eac_route_headphone(false);
    eac_free_dl1();
    if (eac_fd >= 0)
    {
        close(eac_fd);
        eac_fd = -1;
    }
}

void audiohw_set_frequency(int fsel)
{
    /* The HAL's primary output is locked to 44.1k S16_LE stereo (per
     * audio_policy.conf). The kernel takes the rate from AFE registers
     * configured during boot/codec init; sample-rate switching at runtime
     * goes through SET_ANAAFE_REG (0x4302). Pinned to 44.1k for v1. */
    (void)fsel;
}

void audiohw_set_volume(int vol_l, int vol_r)
{
    /* HW volume = MT6323 codec register write via SET_ANAAFE_REG (0x4302).
     * The exact register set is undocumented in the BSP source; need a
     * one-time on-device dump of the codec gain registers before we can
     * map a Rockbox dB value to register bits. Software volume only for
     * now (HAVE_SW_VOLUME_CONTROL in innioasisy1.h). */
    (void)vol_l;
    (void)vol_r;
}

void audiohw_mute(int mute)
{
    (void)mute;
}

/* -------------------------------------------------------------------------- */
/* pcm-internal interface                                                     */

void pcm_play_dma_init(void)
{
    worker_quit = false;
    if (pthread_create(&worker_tid, NULL, worker_main, NULL) != 0)
        panicf("pcm-y1mtk: pthread_create failed");
}

void pcm_play_dma_postinit(void)
{
}

void pcm_play_dma_start(const void *addr, size_t size)
{
    if (eac_fd < 0 || !addr || !size)
        return;

    stream_start();

    pthread_mutex_lock(&worker_mtx);
    cur_addr      = addr;
    cur_size      = size;
    worker_run    = true;
    worker_paused = false;
    pthread_cond_signal(&worker_cv);
    pthread_mutex_unlock(&worker_mtx);
}

void pcm_play_dma_stop(void)
{
    pthread_mutex_lock(&worker_mtx);
    worker_run = false;
    cur_addr   = NULL;
    cur_size   = 0;
    pthread_mutex_unlock(&worker_mtx);

    stream_stop();
}

void pcm_play_dma_pause(bool pause)
{
    worker_paused = pause;
}

size_t pcm_get_bytes_waiting(void)
{
    size_t s;
    pthread_mutex_lock(&worker_mtx);
    s = cur_size;
    pthread_mutex_unlock(&worker_mtx);
    return s;
}

const void *pcm_play_dma_get_peak_buffer(int *count)
{
    pthread_mutex_lock(&worker_mtx);
    const void *p = cur_addr;
    *count = (int)(cur_size / 4);   /* frames = bytes / (2 ch * 2 bytes) */
    pthread_mutex_unlock(&worker_mtx);
    return p;
}

void pcm_dma_apply_settings(void)
{
    /* Called on sample-rate / format change. Audio is locked to 44.1 / 16 /
     * stereo; nothing to do here unless we later wire SET_ANAAFE_REG. */
}
