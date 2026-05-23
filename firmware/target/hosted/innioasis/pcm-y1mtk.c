/*
 * Innioasis Y1 — PCM driver for MediaTek MT6572 /dev/eac.
 *
 * The kernel-side audio is the MTK "EAC" character device (`misc 10:46`,
 * magic 'C'). Music playback is a blocking write(2) on /dev/eac; the kernel's
 * AudDrv_write copy_from_user's into the AFE DL1 ring buffer, and the AFE
 * DMA engine pulls from the same ring on its IRQ1 wakeup.
 *
 * The wrinkle: AUD_RESTART zeros AFE_DAC_CON0/CON1/IRQ_CON. Skipping the
 * register reprogram leaves the DAC clock-gated and produces silence. The
 * full open path matches the MTK HAL (libaudio.primary.default.so):
 *
 *   open /dev/eac
 *   AUD_RESTART
 *   ALLOCATE_MEMIF_DL1(ring_size)
 *   AUD_SET_CLOCK(1) + AUD_SET_ANA_CLOCK(1)     # codec/DAC enable side
 *   SET_AUDSYS_REG AFE_DAC_CON1  = 9 / 0xf      # 44.1k sample-rate index
 *   SET_AUDSYS_REG AFE_IRQ_CON   = 0x90 / 0xf0  # IRQ1 sample-rate bits
 *   SET_AUDSYS_REG AFE_IRQ_CNT1  = period / ~0  # 10 ms (441 samples)
 *   SET_AUDSYS_REG AFE_IRQ_CON   = 1 / 1        # IRQ1 enable
 *   SET_AUDSYS_REG AFE_CONN[6]                  # I05/I06 -> O03/O04 route
 *   SET_AUDSYS_REG AFE_I2S_CON1  = config       # 16-bit, master, normal
 *   SET_AUDSYS_REG AFE_I2S_CON1  = 1 / 1        # I2S OUT enable
 *   SET_HEADPHONE_ON(1)
 *   START_MEMIF_TYPE(MEM_DL1)
 *   SET_AUDSYS_REG AFE_DAC_CON0  = 2 / 2        # DL1 memif fetch enable (bit 1)
 *   write(2) loop ...
 *
 * Pause keeps the alloc + route + clocks; toggles DAC fetch bit + STANDBY.
 * See docs/audio-stack.md for the full RE.
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
/* MTK AUD_DRV ioctl numbers (mediatek/platform/mt6572/kernel/drivers/sound/  */
/* AudDrv_ioctl.h — magic 'C'). Most args pass by value as unsigned long; the */
/* SET_AUDSYS_REG family takes a pointer to Register_Control.                 */

#define AUD_DRV_IOC_MAGIC       'C'
#define _AUD_IOWR(nr, sz)       (((3U)<<30) | ((sz)<<16) | (AUD_DRV_IOC_MAGIC<<8) | (nr))
#define _AUD_IOW(nr, sz)        (((1U)<<30) | ((sz)<<16) | (AUD_DRV_IOC_MAGIC<<8) | (nr))

#define SET_AUDSYS_REG          _AUD_IOWR(0x00, 4)  /* arg = Register_Control* */
#define GET_AUDSYS_REG          _AUD_IOWR(0x01, 4)
#define ALLOCATE_MEMIF_DL1      _AUD_IOWR(0x10, 4)  /* arg = ring bytes        */
#define FREE_MEMIF_DL1          _AUD_IOWR(0x11, 4)
#define AUD_RESTART             _AUD_IOWR(0x1F, 4)
#define START_MEMIF_TYPE        _AUD_IOWR(0x20, 4)  /* arg = MEMIF_BUFFER_TYPE */
#define STANDBY_MEMIF_TYPE      _AUD_IOWR(0x21, 4)
#define AUD_SET_CLOCK           _AUD_IOWR(0x51, 4)
#define AUD_SET_ANA_CLOCK       _AUD_IOWR(0x55, 4)
#define SET_HEADPHONE_ON        _AUD_IOW (0xa4, 4)
#define SET_HEADPHONE_OFF       _AUD_IOW (0xa5, 4)

/* MEMIF_BUFFER_TYPE (AudDrv_Kernel.h) */
#define MEM_DL1                 0

/* AFE register offsets (mediatek/platform/mt6572/kernel/drivers/sound/       */
/* AudDrv_Afe.h). Kernel-side range check accepts offsets up to 0x573.        */
#define AFE_DAC_CON0            0x010
#define AFE_DAC_CON1            0x014
#define AFE_CONN0               0x020
#define AFE_I2S_CON1            0x034
#define AFE_IRQ_CON             0x3A0
#define AFE_IRQ_CNT1            0x3AC

/* Ring buffer length. Kernel AUDDRV_DL1_MAX_BUFFER_LENGTH caps at 0x4000. */
#define EAC_DL1_BUFFER_BYTES    0x2000  /* ~46 ms @ 44.1k S16 stereo */

/* Sample-rate index in the MTK AFE SR table (44100 -> 9). */
#define EAC_SR_IDX_44100        9

/* IRQ1 period in samples — MTK convention is sample_rate/100 = 10 ms tick. */
#define EAC_IRQ1_PERIOD_44100   441

/* SET_AUDSYS_REG payload (struct copy-in from userspace). */
struct register_control {
    uint32_t offset;
    uint32_t value;
    uint32_t mask;
};

/* -------------------------------------------------------------------------- */

static int eac_fd = -1;
static bool dl1_allocated  = false;
static bool route_active   = false;
static bool clocks_held    = false;
static bool afe_configured = false;
static bool dl1_running    = false;
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
/* AFE register writes via SET_AUDSYS_REG.                                    */

static int afe_write(uint32_t offset, uint32_t value, uint32_t mask)
{
    struct register_control r = { offset, value, mask };
    int rc = ioctl(eac_fd, SET_AUDSYS_REG, &r);
    if (rc < 0)
        logf("afe_write off=0x%x val=0x%x mask=0x%x: %s",
             offset, value, mask, strerror(errno));
    return rc;
}

/* -------------------------------------------------------------------------- */
/* Lifecycle helpers — each idempotent on the *_active flag.                  */

static void eac_alloc_dl1(void)
{
    if (dl1_allocated || eac_fd < 0)
        return;
    if (ioctl(eac_fd, ALLOCATE_MEMIF_DL1, EAC_DL1_BUFFER_BYTES) < 0) {
        logf("eac ALLOC_DL1: %s", strerror(errno));
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

static void eac_clocks_hold(bool on)
{
    if (eac_fd < 0 || on == clocks_held)
        return;
    /* AUD_SET_ANA_CLOCK gates the codec analog path and (per BSP source) sets
     * AFE_DAC_CON0 bit 0 (DAC engine enable) + the MT6323 ADDA blocks. We
     * rely on the kernel handler to take care of those; the driver doesn't
     * write them directly. */
    ioctl(eac_fd, AUD_SET_CLOCK,     on ? 1 : 0);
    ioctl(eac_fd, AUD_SET_ANA_CLOCK, on ? 1 : 0);
    clocks_held = on;
}

static void eac_route_headphone(bool on)
{
    if (eac_fd < 0 || on == route_active)
        return;
    int cmd = on ? SET_HEADPHONE_ON : SET_HEADPHONE_OFF;
    if (ioctl(eac_fd, cmd, on ? 1 : 0) < 0)
        logf("eac route hp=%d: %s", on, strerror(errno));
    route_active = on;
}

static void eac_configure_afe_44k_stereo(void)
{
    if (afe_configured || eac_fd < 0)
        return;

    /* Sample rate index for DL1. */
    afe_write(AFE_DAC_CON1, EAC_SR_IDX_44100, 0xf);

    /* IRQ1 — sample-rate bits then period then enable. The enable must be
     * a separate write because the kernel's IRQ-on path latches off the
     * enable bit transition. */
    afe_write(AFE_IRQ_CON,  EAC_SR_IDX_44100 << 4, 0xf0);
    afe_write(AFE_IRQ_CNT1, EAC_IRQ1_PERIOD_44100, 0xffffffff);
    afe_write(AFE_IRQ_CON,  0x1, 0x1);

    /* AFE_CONN routing matrix: wire I05/I06 (DL1 L/R) -> O03/O04 (DAC L/R).
     * The HAL builds these from a (in,out) connection table at runtime;
     * the exact bit positions need an on-device GET_AUDSYS_REG dump to
     * confirm against Y1 silicon. Standard MT6572 layout (per BSP source
     * AudDrv_Afe.c::SetinputConnection) is two bits per (in,out) pair in
     * the AFE_CONNx register block starting at 0x020. Placeholder values
     * here program the canonical DL1->DAC L/R route; if no sound after
     * smoke-test, dump the stock state under Android playback and patch.
     * TODO(hw): confirm AFE_CONN[0..3] values. */
    afe_write(AFE_CONN0 + 0x4, 0x00000001, 0x00000001); /* I05 -> O03 */
    afe_write(AFE_CONN0 + 0x8, 0x00000004, 0x00000004); /* I06 -> O04 */

    /* AFE_I2S_CON1 — 16-bit, master, normal phase, then enable.
     * Bit layout (from MTK AFE convention):
     *   [31..16] reserved
     *   [15..12] sample-rate (= SR_IDX, 9 for 44.1k)
     *   [11..8]  reserved
     *   [7..6]   I2S format (00 = I2S, 01 = LJ, 10 = RJ)
     *   [5]      WLEN: 0 = 16-bit, 1 = 32-bit
     *   [4]      I2S phase: 0 = normal
     *   [3]      I2S master: 1 = master
     *   [2..1]   reserved
     *   [0]      I2S OUT enable (set in a second write so config latches first)
     */
    uint32_t i2s_cfg =
          (EAC_SR_IDX_44100 << 12)
        | (0 << 6)   /* I2S format */
        | (0 << 5)   /* 16-bit WLEN */
        | (0 << 4)   /* normal phase */
        | (1 << 3);  /* master */
    afe_write(AFE_I2S_CON1, i2s_cfg, 0xffff);
    afe_write(AFE_I2S_CON1, 0x1, 0x1);

    afe_configured = true;
}

static void eac_deconfigure_afe(void)
{
    if (!afe_configured || eac_fd < 0)
        return;
    /* Tear down in reverse — disable I2S OUT, disable IRQ1. Leave route
     * matrix; kernel zeros on next AUD_RESTART. */
    afe_write(AFE_I2S_CON1, 0x0, 0x1);
    afe_write(AFE_IRQ_CON,  0x0, 0x1);
    afe_configured = false;
}

static void eac_dl1_enable(bool on)
{
    if (eac_fd < 0 || on == dl1_running)
        return;
    /* AFE_DAC_CON0 bit 1 = DL1 memif fetch enable. The bit 0 below it
     * (DAC engine enable) is kernel-managed via AUD_SET_ANA_CLOCK. */
    afe_write(AFE_DAC_CON0, on ? 0x2 : 0x0, 0x2);
    dl1_running = on;
}

static void stream_start(void)
{
    if (stream_running || eac_fd < 0)
        return;
    eac_alloc_dl1();
    eac_clocks_hold(true);
    eac_configure_afe_44k_stereo();
    eac_route_headphone(true);
    if (ioctl(eac_fd, START_MEMIF_TYPE, MEM_DL1) < 0)
        logf("eac START_MEMIF: %s", strerror(errno));
    eac_dl1_enable(true);
    stream_running = true;
}

static void stream_stop(bool teardown)
{
    if (!stream_running || eac_fd < 0)
        return;
    eac_dl1_enable(false);
    ioctl(eac_fd, STANDBY_MEMIF_TYPE, MEM_DL1);
    stream_running = false;
    if (!teardown)
        return;
    /* Full close — drop AFE config, route, clocks, alloc. */
    eac_deconfigure_afe();
    eac_route_headphone(false);
    eac_clocks_hold(false);
    eac_free_dl1();
}

/* -------------------------------------------------------------------------- */
/* Worker thread — blocking write(2) loop. The AFE IRQ wakes us via the       */
/* kernel's wait_event_interruptible_timeout when ring space frees up.        */

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

        enum pcm_dma_status final_status = PCM_DMAST_OK;

        while (size > 0 && !worker_quit)
        {
            while (worker_paused && !worker_quit)
                usleep(2000);

            ssize_t n = write(eac_fd, addr, size);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                logf("eac write: %s", strerror(errno));
                final_status = PCM_DMAST_ERR_DMA;
                break;
            }
            addr = (const uint8_t *)addr + n;
            size -= (size_t)n;
        }

        const void *next_addr = NULL;
        size_t      next_size = 0;
        bool more = pcm_play_dma_complete_callback(final_status,
                                                   &next_addr, &next_size);
        pthread_mutex_lock(&worker_mtx);
        if (more && next_addr && next_size) {
            cur_addr   = next_addr;
            cur_size   = next_size;
            worker_run = true;
        } else {
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
    if (eac_fd < 0) {
        logf("open /dev/eac: %s", strerror(errno));
        return;
    }

    /* AUD_RESTART is the HAL's hardware-init handshake — a kitchen-sink
     * clock+AFE reset that zeros AFE_DAC_CON0/CON1/IRQ_CON. The driver
     * relies on this clean slate and reprograms everything in stream_start. */
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

    stream_stop(true);
    if (eac_fd >= 0) {
        close(eac_fd);
        eac_fd = -1;
    }
}

void audiohw_set_frequency(int fsel)
{
    /* Primary output is locked to 44.1k S16_LE stereo. Runtime rate
     * switching would require writing AFE_DAC_CON1/AFE_IRQ_CON/AFE_I2S_CON1
     * with the new SR index and AFE_IRQ_CNT1 with the new period. */
    (void)fsel;
}

void audiohw_set_volume(int vol_l, int vol_r)
{
    /* HW volume = MT6323 codec writes via SET_ANAAFE_REG (0xc0044302).
     * Register set is not in the BSP source; needs a one-time on-device
     * dump under a volume-slider drag. Software volume only for now
     * (HAVE_SW_VOLUME_CONTROL in innioasisy1.h). */
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

    /* Soft stop — keep alloc/route/clocks, just disarm DAC fetch. */
    stream_stop(false);
}

void pcm_play_dma_pause(bool pause)
{
    /* Pause toggles only the DL1 fetch bit + memif standby. Keeps the
     * route, clocks and AFE config primed for fast resume. */
    worker_paused = pause;
    if (pause) {
        eac_dl1_enable(false);
        if (eac_fd >= 0)
            ioctl(eac_fd, STANDBY_MEMIF_TYPE, MEM_DL1);
    } else if (stream_running) {
        if (eac_fd >= 0)
            ioctl(eac_fd, START_MEMIF_TYPE, MEM_DL1);
        eac_dl1_enable(true);
    }
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
    /* Sample-rate / format change. Locked to 44.1/16/stereo for now. */
}
