/*
 * Innioasis Y1 — PCM driver for MediaTek MT6572 /dev/eac.
 *
 * Music playback is a blocking write(2) on /dev/eac; the kernel's AudDrv_write
 * copy_from_user's into the AFE DL1 ring buffer, and the AFE DMA engine pulls
 * from the same ring on its IRQ1 wakeup.
 *
 * AUD_RESTART zeros the AFE register block, so the driver re-programs
 * everything to the values stock HAL uses during music playback (captured
 * on-device via boot-test/afe_dump idle/playing diff 2026-05-23):
 *
 *   open /dev/eac
 *   AUD_RESTART
 *   ALLOCATE_MEMIF_DL1(ring_size)
 *   AUD_SET_CLOCK(1) + AUD_SET_ANA_CLOCK(1)
 *
 *   SET_AUDSYS_REG AFE_DAC_CON1          = 0x9        / 0xf
 *   SET_AUDSYS_REG AFE_IRQ_CON           = 0x90       / 0xf0
 *   SET_AUDSYS_REG AFE_IRQ_CNT1          = 0x800      / ~0
 *   SET_AUDSYS_REG AFE_IRQ_CON           = 0x1        / 0x1
 *   SET_AUDSYS_REG AFE_CONN1             = 0x00200000 / 0x00200000
 *   SET_AUDSYS_REG AFE_CONN2             = 0x40       / 0x40
 *   SET_AUDSYS_REG AFE_I2S_CON1          = 0x0909     / 0xffff
 *   SET_AUDSYS_REG AFE_ADDA_DL_SRC2_CON0 = 0x73001803 / ~0
 *   SET_AUDSYS_REG AFE_ADDA_UL_DL_CON0   = 0x1        / 0x1
 *   SET_AUDSYS_REG AFE_IRQ_MCU_EN        = 0x639      / 0xffff
 *   SET_ANAAFE_REG ANA 0x108             = 0x12bf     / 0xffff
 *   SET_ANAAFE_REG ANA 0x194             = 0x0074     / 0xffff
 *
 *   SET_SPEAKER_ON(1) + SET_HEADPHONE_ON(1)
 *   START_MEMIF_TYPE(MEM_DL1)
 *   SET_AUDSYS_REG AFE_DAC_CON0          = 0x3402     / 0x3402
 *
 *   write(2) loop ...
 *
 * KNOWN-INSUFFICIENT (2026-05-23): the above sequence runs cleanly but
 * produces no audible output on hardware.  Steady-state register values match
 * stock playback byte-for-byte, but the codec analog stage doesn't pass the
 * signal through.  Suspected missing piece: a temporal codec power-up
 * sequence (charge-pump -> settle -> DAC -> settle -> un-mute) that the
 * steady-state dump can't reveal.  Next investigation: strace mediaserver
 * during stock playback start to capture the write order + intermediate
 * codec values.  See docs/audio-stack.md "Open items still on the audio
 * path" for the full follow-up plan.
 */

#include "config.h"
#include "audio.h"
#include "audiohw.h"
#include "button.h"
#include "pcm.h"
#include "pcm-internal.h"
#include "pcm_sampr.h"
#include "pcm_sink.h"
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
#define SET_ANAAFE_REG          _AUD_IOWR(0x02, 4)  /* MT6323 codec write */
#define SET_SPEAKER_ON          _AUD_IOW (0xa1, 4)
#define SET_SPEAKER_OFF         _AUD_IOW (0xa2, 4)
#define SET_HEADPHONE_ON        _AUD_IOW (0xa4, 4)
#define SET_HEADPHONE_OFF       _AUD_IOW (0xa5, 4)

/* MEMIF_BUFFER_TYPE (AudDrv_Kernel.h) */
#define MEM_DL1                 0

/* AFE register offsets (mediatek/platform/mt6572/kernel/drivers/sound/       */
/* AudDrv_Afe.h). Kernel-side range check accepts offsets up to 0x573.        */
#define AFE_DAC_CON0            0x010
#define AFE_DAC_CON1            0x014
#define AFE_CONN0               0x020
#define AFE_CONN1               0x024
#define AFE_CONN2               0x028
#define AFE_ADDA_DL_SRC2_CON0   0x108
#define AFE_ADDA_UL_DL_CON0     0x124
#define AFE_IRQ_MCU_EN          0x3C0
#define AFE_I2S_CON1            0x034
#define AFE_IRQ_CON             0x3A0
#define AFE_IRQ_CNT1            0x3AC

/* Ring buffer length. Kernel AUDDRV_DL1_MAX_BUFFER_LENGTH caps at 0x4000. */
#define EAC_DL1_BUFFER_BYTES    0x2000  /* ~46 ms @ 44.1k S16 stereo */

/* Sample-rate index in the MTK AFE SR table (44100 -> 9). */
#define EAC_SR_IDX_44100        9

/* IRQ1 period in samples — stock HAL uses 0x800 = 2048 samples (~46 ms at
 * 44.1k) per on-device afe_dump capture, not the SR/100 convention. */
#define EAC_IRQ1_PERIOD_44100   0x800

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

static void eac_route_analog(bool on)
{
    if (eac_fd < 0 || on == route_active)
        return;
    /* Kernel accdet does NOT auto-mute the inactive route: validated on
     * 2026-05-23 -- enabling both SET_SPEAKER_ON and SET_HEADPHONE_ON
     * makes the tone play out of both at once.  Userspace has to pick
     * the right amp based on the jack-detect sysfs.  button-y1.c's
     * headphones_inserted() reads /sys/class/switch/h2w/state. */
    if (on) {
        bool hp = headphones_inserted();
        if (ioctl(eac_fd, hp ? SET_SPEAKER_OFF   : SET_SPEAKER_ON,   hp ? 0 : 1) < 0)
            logf("eac spk: %s", strerror(errno));
        if (ioctl(eac_fd, hp ? SET_HEADPHONE_ON  : SET_HEADPHONE_OFF, hp ? 1 : 0) < 0)
            logf("eac hp: %s", strerror(errno));
    } else {
        if (ioctl(eac_fd, SET_SPEAKER_OFF,   0) < 0)
            logf("eac spk off: %s", strerror(errno));
        if (ioctl(eac_fd, SET_HEADPHONE_OFF, 0) < 0)
            logf("eac hp off: %s", strerror(errno));
    }
    route_active = on;
}

/* Re-route mid-stream when jack state changes.  Caller responsible for
 * detecting the transition (poll /sys/class/switch/h2w/state on the
 * worker thread or in a button-driver hook).  Both ioctls are cheap
 * (single SET on the amp HW) so we just blast both with the new state. */
void pcm_y1mtk_jack_reroute(void)
{
    if (eac_fd < 0 || !route_active)
        return;
    bool hp = headphones_inserted();
    ioctl(eac_fd, hp ? SET_SPEAKER_OFF   : SET_SPEAKER_ON,   hp ? 0 : 1);
    ioctl(eac_fd, hp ? SET_HEADPHONE_ON  : SET_HEADPHONE_OFF, hp ? 1 : 0);
}

/* MT6323 codec write via SET_ANAAFE_REG (0xC0044302). Same struct shape as
 * SET_AUDSYS_REG but targets the analog codec on MT6323 rather than the
 * digital AFE block. */
static int codec_write(uint32_t offset, uint32_t value, uint32_t mask)
{
    struct register_control r = { offset, value, mask };
    int rc = ioctl(eac_fd, SET_ANAAFE_REG, &r);
    if (rc < 0)
        logf("codec_write off=0x%x val=0x%x mask=0x%x: %s",
             offset, value, mask, strerror(errno));
    return rc;
}

/* Phase 1 of the captured stock session: 17 SET_AUDSYS_REG writes that
 * configure the AFE digital path.  Verbatim from the 2026-05-23 trace at
 * /work/logs/mediaserver-eac-args.log (boot-test/y1_alive::test_audio
 * confirmed tone-audible with this exact sequence). */
static void eac_afe_config_phase1(void)
{
    if (afe_configured || eac_fd < 0)
        return;

    afe_write(0x3a0, 0x00000090, 0x000000f0);   /* IRQ_CON: trigger mode */
    afe_write(0x3ac, 0x00000800, 0xffffffff);   /* IRQ_CNT1: 2048-sample period */
    afe_write(0x3a0, 0x00000001, 0x00000001);   /* IRQ_CON: IRQ1 enable */
    afe_write(0x024, 0x00200000, 0x00200000);   /* CONN1: DL1 L route */
    afe_write(0x028, 0x00000040, 0x00000040);   /* CONN2: DL1 R route */
    afe_write(0x010, 0x00000400, 0x00000400);   /* DAC_CON0 step 1 (bit 10) */
    afe_write(0x260, 0x00000000, 0xffffffff);   /* zero -- captured */
    afe_write(0x264, 0x00000000, 0xffffffff);   /* zero -- captured */
    afe_write(0x108, 0x73001803, 0xffffffff);   /* ADDA_DL_SRC2_CON0 config */
    afe_write(0x10c, 0xf74f0000, 0xffffffff);   /* ADDA_DL_SRC2_CON1 config */
    afe_write(0x034, 0x00000908, 0x0000ffff);   /* I2S_CON1 config (no enable) */
    afe_write(0x108, 0x00000001, 0x00000001);   /* ADDA_DL_SRC2_CON0 enable */
    afe_write(0x034, 0x00000001, 0x00000001);   /* I2S_CON1 OUT enable */
    afe_write(0x108, 0x00000001, 0x00000001);   /* stock writes it twice */
    afe_write(0x124, 0x00000001, 0x00000001);   /* ADDA_UL_DL_CON0 */
    afe_write(0x4c4, 0x00000000, 0x00000010);   /* clear bit 4 (purpose unknown) */
    afe_write(0x010, 0x00000001, 0x00000001);   /* DAC_CON0 step 2: AFE enable */

    afe_configured = true;
}

/* Phase 3: 12 immediate + 2 spaced MT6323 codec writes that ramp the
 * analog path from idle to mid-volume.  Final 2 (volume ramp on 0x70a)
 * deferred to eac_codec_volume_ramp() after amp enable. */
static void eac_codec_ramp_phase3(void)
{
    codec_write(0x10c,  0x00000100, 0xffff0100);
    codec_write(0x4024, 0x00007330, 0xffffffff);
    codec_write(0x4002, 0x00000009, 0xffff000f);
    codec_write(0x4006, 0x00000304, 0xffffffff);
    codec_write(0x4008, 0x00000292, 0xffffffff);
    codec_write(0x4014, 0x00000001, 0xffff0001);
    codec_write(0x4016, 0x00000300, 0xffff0300);
    codec_write(0x4000, 0x00000001, 0xffff0001);
    codec_write(0x70c,  0x0000f7f2, 0xffffffff);
    codec_write(0x700,  0x00007000, 0xfffff000);
    codec_write(0x70a,  0x00000014, 0xffffffff);
    codec_write(0x708,  0x0000007c, 0xffffffff);
    /* Stock pauses ~16.5 ms here while DMA continues; the kernel ring
     * buffer absorbs it without a userspace sleep on our side. */
    codec_write(0x70c,  0x0000f5ba, 0xffffffff);
    codec_write(0x70a,  0x00002214, 0xffffffff);
}

/* Phase 5: 2-step volume ramp on ANA 0x70a, fires ~215 ms after the amp
 * routes are enabled. */
static void eac_codec_volume_ramp(void)
{
    codec_write(0x70a, 0x00003000, 0xffff7000);
    codec_write(0x70a, 0x00000300, 0xffff0700);
}

/* Phase 6: SR + DAC enable bit -- the last two AFE writes in the stock
 * session.  Setting SR before this point breaks the bring-up. */
static void eac_afe_final_phase6(void)
{
    afe_write(0x014, EAC_SR_IDX_44100, 0xf);     /* DAC_CON1: SR = 44.1k */
    afe_write(0x010, 0x00000002, 0x00000002);    /* DAC_CON0: DL DAC enable */
}

/* Codec ramp-down captured from the stock session teardown 2026-05-23.
 * Companion to eac_codec_ramp_phase3.  Resets MT6323 gain, mutes the
 * analog path, drops the codec DAC enable.  Pop-free shutdown depends
 * on running this before any AFE clear. */
static void eac_codec_rampdown(void)
{
    codec_write(0x70a, 0x00000014, 0xffffffff);
    codec_write(0x70c, 0x0000f7f2, 0xffffffff);
    codec_write(0x708, 0x00000000, 0xffffffff);
    codec_write(0x700, 0x00000000, 0xffff1000);
    codec_write(0x70c, 0x000037e2, 0xffffffff);
    codec_write(0x4000, 0x00000000, 0xffff0001);   /* codec DAC disable */
    codec_write(0x10a, 0x00000100, 0xffff0100);
    codec_write(0x10a, 0x00000100, 0xffff0100);    /* stock writes it twice */
}

/* Full AFE teardown captured from the stock session 2026-05-23.  Order
 * matches stock; 0x4c4 bit 4 is SET here (cleared in eac_afe_config_phase1
 * -- it's a mute/hold gate). */
static void eac_deconfigure_afe(void)
{
    if (!afe_configured || eac_fd < 0)
        return;
    afe_write(0x024, 0x00000000, 0x00200000);    /* clear CONN1 DL1 route */
    afe_write(0x028, 0x00000000, 0x00000040);    /* clear CONN2 DL1 route */
    afe_write(0x010, 0x00000000, 0x00000400);    /* clear DAC_CON0 bit 10 */
    afe_write(0x108, 0x00000000, 0x00000001);    /* clear DL_SRC2_CON0 enable */
    afe_write(0x034, 0x00000000, 0x00000001);    /* clear I2S_CON1 OUT enable */
    afe_write(0x108, 0x00000000, 0x00000001);    /* (twice, like start) */
    afe_write(0x124, 0x00000000, 0x00000001);    /* clear UL_DL_CON0 */
    afe_write(0x4c4, 0x00000010, 0x00000010);    /* SET bit 4 (mute/hold gate) */
    afe_write(0x010, 0x00000000, 0x00000002);    /* clear DAC_CON0 DL DAC bit */
    afe_write(0x3a0, 0x00000000, 0x00000001);    /* clear IRQ1 enable */
    afe_write(0x010, 0x00000000, 0x00000001);    /* clear DAC_CON0 AFE enable */
    afe_configured = false;
}

/* DL1 fetch bit (AFE_DAC_CON0 bit 1) toggle for pause/resume.  All other
 * DAC_CON0 bits stay set across a pause so the codec doesn't lose its
 * power-up state. */
static void eac_dl1_enable(bool on)
{
    if (eac_fd < 0 || on == dl1_running)
        return;
    afe_write(0x010, on ? 0x00000002 : 0x00000000, 0x00000002);
    dl1_running = on;
}

/* Push a zero-filled chunk into the DL1 ring so the kernel DMA engine
 * has data to fetch the instant the codec un-mutes.  Stock HAL does this
 * implicitly via its first AudioFlinger buffer copy ~43 ms after
 * START_MEMIF; we do it explicitly here.  One IRQ1-period worth of
 * silence (2048 stereo samples) is enough to bridge to the worker. */
static void eac_prime_dma(void)
{
    static const uint8_t silence[2048 * 4] = { 0 };
    ssize_t n = write(eac_fd, silence, sizeof silence);
    if (n != (ssize_t)sizeof silence)
        logf("eac prime_dma short write n=%zd: %s", n, strerror(errno));
}

static void stream_start(void)
{
    if (stream_running || eac_fd < 0)
        return;

    /* Cold init (idempotent). */
    eac_alloc_dl1();
    eac_clocks_hold(true);

    /* Mirror the captured stock session-start order verbatim.  The
     * smoke test in boot-test/y1_alive.c::test_audio uses the same
     * sequence and confirmed tone-audible 2026-05-23. */
    eac_afe_config_phase1();

    if (ioctl(eac_fd, START_MEMIF_TYPE, MEM_DL1) < 0)
        logf("eac START_MEMIF: %s", strerror(errno));
    eac_prime_dma();

    eac_codec_ramp_phase3();
    eac_route_analog(true);            /* Phase 4: amps LAST */

    /* Stock waits ~215 ms here while data flows before the volume
     * ramp; on our side DMA is being fed by the worker by the time we
     * get here, so a short usleep is sufficient. */
    usleep(215000);
    eac_codec_volume_ramp();

    eac_afe_final_phase6();
    dl1_running    = true;
    stream_running = true;
}

static void stream_stop(bool teardown)
{
    if (!stream_running || eac_fd < 0)
        return;

    if (teardown) {
        /* Full close: mirror the captured stock teardown.  Order:
         * codec ramp-down -> AFE clear -> amp routes off -> STANDBY
         * -> clocks off -> DL1 free.  The codec ramp + AFE 0x4c4 bit 4
         * "mute/hold gate" set are the difference between a quiet stop
         * and a pop-on-stop. */
        eac_codec_rampdown();
        eac_deconfigure_afe();
        eac_route_analog(false);
        ioctl(eac_fd, STANDBY_MEMIF_TYPE, MEM_DL1);
        eac_clocks_hold(false);
        eac_free_dl1();
    } else {
        /* Light pause: leave codec + amp + AFE config in place so
         * resume is cheap.  Just stop fetching from the ring. */
        eac_dl1_enable(false);
        ioctl(eac_fd, STANDBY_MEMIF_TYPE, MEM_DL1);
    }
    stream_running = false;
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
/* pcm_sink interface — entry points the core calls through builtin_pcm_sink. */

static void sink_dma_init(void)
{
    audiohw_preinit();

    worker_quit = false;
    if (pthread_create(&worker_tid, NULL, worker_main, NULL) != 0)
        panicf("pcm-y1mtk: pthread_create failed");
}

static void sink_dma_postinit(void)
{
    audiohw_postinit();
}

static void sink_lock(void)
{
    pthread_mutex_lock(&worker_mtx);
}

static void sink_unlock(void)
{
    pthread_mutex_unlock(&worker_mtx);
}

static void sink_set_freq(uint16_t freq)
{
    audiohw_set_frequency(freq);
}

static void sink_dma_start(const void *addr, size_t size)
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

static void sink_dma_stop(void)
{
    pthread_mutex_lock(&worker_mtx);
    worker_run = false;
    cur_addr   = NULL;
    cur_size   = 0;
    pthread_mutex_unlock(&worker_mtx);

    /* Soft stop — keep alloc/route/clocks, just disarm DAC fetch. */
    stream_stop(false);
}

struct pcm_sink builtin_pcm_sink = {
    .caps = {
        .samprs       = hw_freq_sampr,
        .num_samprs   = HW_NUM_FREQ,
        .default_freq = HW_FREQ_DEFAULT,
    },
    .ops = {
        .init     = sink_dma_init,
        .postinit = sink_dma_postinit,
        .set_freq = sink_set_freq,
        .lock     = sink_lock,
        .unlock   = sink_unlock,
        .play     = sink_dma_start,
        .stop     = sink_dma_stop,
    },
};
