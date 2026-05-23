/*
 * Innioasis Y1 — PCM driver for MediaTek MT6572 /dev/eac.
 *
 * The kernel-side audio is the MTK "EAC" (Extended Audio Control) char device
 * — `misc 10:46`, magic 'C' ioctls. Music playback is plain blocking write(2);
 * the kernel driver DMAs to I2S internally. The path documented here was
 * verified by RE of `/system/lib/libaudio.primary.default.so` (2026-05-23 —
 * see `docs/audio-stack.md` for the full ioctl table and call-site trace).
 *
 * Bringup sequence (replicates AudioMTKStreamOut::write prologue):
 *   open("/dev/eac", O_RDWR)
 *   ioctl(eac_fd, 0xc004431f, &dummy)      // HardwareInit handshake
 *   ioctl(eac_fd, 0xc0044320, &attr)       // configure mem-IF DL1 (sample rate, channels, bitwidth)
 *   ioctl(eac_fd, 0xc00443e1, &on=1)       // enable mem-IF block 0
 *   ioctl(eac_fd, 0xc00443e0, &on=1)       // start AFE downlink
 *
 * Hot loop:
 *   write(eac_fd, pcm_buf, len)            // blocking, returns bytes consumed
 *
 * Teardown:
 *   ioctl(eac_fd, 0xc00443e1, &on=0)
 *   ioctl(eac_fd, 0xc00443e0, &on=0)
 *   close(eac_fd)
 *
 * Outstanding RE gap: the 22-byte attribute struct passed to ioctl 0x4320 is
 * only partially mapped. Known field offsets `{ u8 channels, u8 bitwidth,
 * u16 _pad, u32 samplerate, ... }`. The trailing 14 bytes are zeroed here
 * pending an on-device strace; if `ioctl(0x4320)` returns EINVAL on hardware,
 * dump mediaserver's invocation to recover the missing fields.
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
/* MTK /dev/eac ioctl numbers (decoded from libaudio.primary.default.so).     */

#define EAC_HARDWARE_INIT       0xc004431f  /* _IOWR('C', 0x1f, int)         */
#define EAC_SET_MEMIF_ATTR      0xc0044320  /* _IOWR('C', 0x20, void *attr)  */
#define EAC_SET_AFE_ON          0xc00443e0  /* _IOWR('C', 0xe0, int on)      */
#define EAC_SET_MEMIF_ENABLE    0xc00443e1  /* _IOWR('C', 0xe1, int on)      */

/* 22-byte attribute struct (PARTIAL — see file header). */
struct eac_memif_attr {
    uint8_t  channels;       /* +0  */
    uint8_t  bitwidth;       /* +1  — 16 or 32 */
    uint16_t _pad2;          /* +2  */
    uint32_t samplerate;     /* +4  */
    uint8_t  unknown_8[4];   /* +8  — observed strb-initialised in HAL */
    uint8_t  unknown_12;     /* +12 — strb */
    uint8_t  _pad13[3];      /* +13 */
    uint32_t unknown_16;     /* +16 — str-initialised */
    uint8_t  unknown_20[3];  /* +20 — strb each */
    uint8_t  _pad23;
} __attribute__((packed));

/* -------------------------------------------------------------------------- */

static int eac_fd = -1;
static bool eac_initialised = false;

static pthread_t   worker_tid;
static pthread_mutex_t worker_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  worker_cv  = PTHREAD_COND_INITIALIZER;

static const void *cur_addr = NULL;
static size_t      cur_size = 0;
static volatile bool worker_run     = false;
static volatile bool worker_quit    = false;
static volatile bool worker_paused  = false;
static volatile bool stream_running = false;

static unsigned int current_sample_rate = 44100;

/* -------------------------------------------------------------------------- */

static void apply_memif_attr(unsigned int rate)
{
    struct eac_memif_attr attr;
    memset(&attr, 0, sizeof attr);
    attr.channels   = 2;
    attr.bitwidth   = 16;
    attr.samplerate = rate;
    if (ioctl(eac_fd, EAC_SET_MEMIF_ATTR, &attr) < 0)
        logf("eac SET_MEMIF_ATTR: %s", strerror(errno));
}

static void stream_start(void)
{
    if (stream_running)
        return;
    apply_memif_attr(current_sample_rate);

    int on = 1;
    if (ioctl(eac_fd, EAC_SET_MEMIF_ENABLE, &on) < 0)
        logf("eac SET_MEMIF_ENABLE: %s", strerror(errno));
    if (ioctl(eac_fd, EAC_SET_AFE_ON, &on) < 0)
        logf("eac SET_AFE_ON: %s", strerror(errno));

    stream_running = true;
}

static void stream_stop(void)
{
    if (!stream_running)
        return;
    int off = 0;
    ioctl(eac_fd, EAC_SET_MEMIF_ENABLE, &off);
    ioctl(eac_fd, EAC_SET_AFE_ON, &off);
    stream_running = false;
}

/* -------------------------------------------------------------------------- */
/* Worker thread: consumes buffers via blocking write(2) on /dev/eac.         */

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
            cur_addr = next_addr;
            cur_size = next_size;
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
    if (eac_initialised)
        return;

    eac_fd = open("/dev/eac", O_RDWR);
    if (eac_fd < 0)
    {
        logf("open /dev/eac: %s", strerror(errno));
        return;
    }

    int dummy = 0;
    if (ioctl(eac_fd, EAC_HARDWARE_INIT, &dummy) < 0)
        logf("eac HARDWARE_INIT: %s", strerror(errno));

    eac_initialised = true;
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

    if (eac_fd >= 0)
    {
        stream_stop();
        close(eac_fd);
        eac_fd = -1;
    }
    eac_initialised = false;
}

void audiohw_set_frequency(int fsel)
{
    /* fsel is a Rockbox sample-rate index; resolve to Hz via pcm_sampr_type[]
     * — but the HAL is locked to 44.1 kHz so we honour that until /dev/eac is
     * verified to accept other rates. */
    (void)fsel;
    current_sample_rate = 44100;
}

void audiohw_set_volume(int vol_l, int vol_r)
{
    /* Software volume only for now; HW volume goes via AudioAnalogReg ioctls
     * (0x4302/0x4303) which write to the MT6323 codec's volume registers. The
     * exact codec register set is undocumented; deferred until a one-time
     * register dump on device. */
    (void)vol_l;
    (void)vol_r;
}

void audiohw_mute(int mute)
{
    /* No HW mute path RE'd. Software path mutes upstream. */
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
    cur_addr = addr;
    cur_size = size;
    worker_run = true;
    worker_paused = false;
    pthread_cond_signal(&worker_cv);
    pthread_mutex_unlock(&worker_mtx);
}

void pcm_play_dma_stop(void)
{
    pthread_mutex_lock(&worker_mtx);
    worker_run = false;
    cur_addr = NULL;
    cur_size = 0;
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
    /* Rockbox calls this when sample rate / format changes. We re-apply the
     * mem-IF attr on the next stream_start; nothing to do here unless the
     * stream is already running and we need a live re-configure. */
}
