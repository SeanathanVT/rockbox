/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2026 by Sean Halpin
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

/*
 * Innioasis Y1 — PCM driver for MediaTek MT6572 /dev/eac.
 *
 * Music playback is a blocking write(2) on /dev/eac; the kernel's AudDrv_write
 * copy_from_user's into the AFE DL1 ring buffer, and the AFE DMA engine pulls
 * from the same ring on its IRQ1 wakeup.
 *
 * AUD_RESTART zeros the AFE register block, so the driver re-programs
 * everything to the values the stock HAL uses during music playback:
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
 * Bring-up order: AFE config (eac_afe_config_phase1) -> START_MEMIF -> prime +
 * silence feed across the codec ramp/settle (eac_codec_ramp_phase3 + the feed
 * loop in stream_start) -> amps on -> volume ramp -> SR + DAC enable
 * (eac_afe_final_phase6).  The worker does a two-step handoff (cf. pcm-alsa.c):
 * it must drive both pcm_play_dma_complete_callback (get-more) and
 * pcm_play_dma_status_callback(PCM_DMAST_STARTED), or the mixer never mixes or
 * advances the channel.  audiohw_set_volume calls pcm_set_master_volume because
 * HAVE_SW_VOLUME_CONTROL does the attenuation in software (cf. dummy_codec.c).
 * Output routes to headphone-or-speaker by jack detect.  Sample rate is locked
 * to 44.1k (HW_SAMPR_CAPS = SAMPR_CAP_44; the DSP resamples, set_freq is a
 * no-op).
 */

/* Uncomment to route this file's logf() to DEBUGF/stderr in a -DDEBUG build
 * (logf.h no-ops logf() otherwise).  Left disabled in normal builds. */
/* #define LOGF_ENABLE */

#include "config.h"
#include "audio.h"
#include "audiohw.h"
#include "button.h"
#include "pcm.h"
#include "pcm-internal.h"
#include "pcm_sampr.h"
#include "pcm_sink.h"
#include "pcm_sw_volume.h"
#include "system.h"
#include "kernel.h"
#include "panic.h"
#include "logf.h"
#include "bluetooth/bt-client.h"

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

/* Ring buffer length. Kernel AUDDRV_DL1_MAX_BUFFER_LENGTH caps at 0x4000.
 * Use the max (~93 ms @ 44.1k S16 stereo): the worker's get-more does the
 * mixing under worker_mtx, which the cooperative threads also contend for via
 * pcm_play_lock, so each refill cycle leaves a gap where the ring drains
 * unfed.  The extra headroom over 0x2000 absorbs that jitter -- a too-small
 * ring underruns at the gaps and crackles. */
#define EAC_DL1_BUFFER_BYTES    0x4000

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
/* Recursive: this lock backs the pcm_sink .lock/.unlock ops, and pcm_play_lock()
 * (pcm.c) has no nesting counter -- the mixer holds it across mixer_start_pcm()
 * while pcm_play_data() re-takes it, so a non-recursive mutex self-deadlocks
 * before .play (sink_dma_start) is ever reached. */
static pthread_mutex_t worker_mtx = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static pthread_cond_t  worker_cv  = PTHREAD_COND_INITIALIZER;

static const void *cur_addr = NULL;
static size_t      cur_size = 0;
static volatile bool worker_run    = false;
static volatile bool worker_quit   = false;
static volatile bool worker_paused = false;
/* Cleared on each fresh session start (sink_dma_start).  mixer_start_pcm()
 * primes two frames of look-ahead before handing us the first buffer, so the
 * first get-more already has mixed data -- skip the mix-ahead status callback
 * exactly once after a (re)start to stay in phase with the double buffer. */
static bool mixer_primed = false;
/* Worker's view of the BT-output flag (driven automatically by the bt-client
 * pump on A2DP connect/disconnect; lives in bt-client), so the worker can
 * re-apply the amp route the moment routing changes mid-playback. */
static bool bt_routing = false;

/* Last per-channel centibel gain the core requested via audiohw_set_volume.
 * Applied to the local output, but forced to UNITY while BT is the active
 * output (apply_sw_volume): an A2DP sink renders at the absolute volume we
 * command it over AVRCP, so attenuating the stream here too would double-
 * attenuate (and slave Y1 playback to the sink's level). */
static int  sw_vol_l = PCM_MUTE_LEVEL;
static int  sw_vol_r = PCM_MUTE_LEVEL;
static void apply_sw_volume(void);

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

/* Output routing.  The kernel accdet does NOT auto-mute the inactive route
 * (enabling both amps plays out of both at once), so exactly one amp is
 * energised.  The headphone-vs-speaker choice is owned by
 * Rockbox's HAVE_SPEAKER framework: audio_enable_speaker() resolves the
 * "Speaker" setting (Off/On/Auto) against the jack state and calls
 * audiohw_enable_speaker(), which records want_speaker here.  button.c polls
 * headphones_inserted() and posts SYS_PHONE_{UN,}PLUGGED, so hot-plug while
 * playing re-routes automatically. */
static bool want_speaker = false;

/* Apply want_speaker to the amps.  No-op unless a stream is live -- the amps
 * are powered only between eac_route_analog(true) and (false) so a stopped
 * player doesn't hiss or pop. */
static void eac_apply_route(void)
{
    if (eac_fd < 0 || !route_active)
        return;
    /* When BT output is selected the audio goes to the speaker over A2DP;
     * the eac DMA keeps running (it paces the worker at the 44.1k hardware
     * clock and feeds the BT ring), but both local amps stay off so nothing
     * comes out of the on-board speaker/headphone. */
    if (bt_client_is_output()) {
        ioctl(eac_fd, SET_SPEAKER_OFF,   0);
        ioctl(eac_fd, SET_HEADPHONE_OFF, 0);
        return;
    }
    if (want_speaker) {
        ioctl(eac_fd, SET_HEADPHONE_OFF, 0);
        ioctl(eac_fd, SET_SPEAKER_ON,    1);
    } else {
        ioctl(eac_fd, SET_SPEAKER_OFF,   0);
        ioctl(eac_fd, SET_HEADPHONE_ON,  1);
    }
}

static void eac_route_analog(bool on)
{
    if (eac_fd < 0 || on == route_active)
        return;
    if (on) {
        route_active = true;
        eac_apply_route();
    } else {
        ioctl(eac_fd, SET_SPEAKER_OFF,   0);
        ioctl(eac_fd, SET_HEADPHONE_OFF, 0);
        route_active = false;
    }
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
 * configure the AFE digital path.  Verbatim from the stock HAL's
 * music-playback trace. */
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

/* Codec ramp-down captured from the stock session teardown.
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

/* Full AFE teardown captured from the stock session.  Order matches stock;
 * 0x4c4 bit 4 is SET here (cleared in eac_afe_config_phase1 -- it's a
 * mute/hold gate). */
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
/* Write one IRQ1-period (2048 stereo frames, ~46 ms) of silence to the DL1
 * ring, blocking until the DAC drains enough space.  Used both to prime and
 * to keep the ring fed during the codec settle window. */
static ssize_t eac_write_silence(void)
{
    static const uint8_t silence[2048 * 4] = { 0 };
    ssize_t n = write(eac_fd, silence, sizeof silence);
    if (n != (ssize_t)sizeof silence)
        logf("eac silence short write n=%zd: %s", n, strerror(errno));
    return n;
}

static void eac_prime_dma(void)
{
    /* Fill the whole ring with silence so the DMA starts with a full buffer
     * of cushion the instant the codec un-mutes. */
    for (int i = 0; i < EAC_DL1_BUFFER_BYTES / (2048 * 4); i++)
        eac_write_silence();
}

static void stream_start(void)
{
    if (stream_running || eac_fd < 0)
        return;

    /* Cold init (idempotent). */
    eac_alloc_dl1();
    eac_clocks_hold(true);

    /* Mirror the captured stock session-start order verbatim. */
    eac_afe_config_phase1();

    if (ioctl(eac_fd, START_MEMIF_TYPE, MEM_DL1) < 0)
        logf("eac START_MEMIF: %s", strerror(errno));
    eac_prime_dma();

    eac_codec_ramp_phase3();
    eac_route_analog(true);            /* Phase 4: amps LAST */

    /* Keep the DMA fed with silence through the codec settle window rather
     * than sleeping.  The analog power-up needs data clocking out of I2S
     * continuously; a single prime + usleep starves the ring and the codec
     * never passes the signal.  Each blocking write paces against the DAC, so
     * this also supplies the ~215 ms the ramp expects (5 x ~46 ms).  The
     * worker isn't feeding yet -- it's signaled only after stream_start
     * returns. */
    for (int i = 0; i < 5; i++)
        eac_write_silence();
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

        /* BT output: mirror this mixed buffer (44.1k S16 stereo, same format
         * as the ring) into the daemon's PCM ring for SBC/A2DP.  The eac write
         * below still runs to pace the loop at the hardware clock, but the
         * local amps are muted via eac_apply_route().  bt_client_pcm_write
         * drops frames if the ring is full, so it can't stall the worker. */
        bool bt_out = bt_client_is_output();
        if (bt_out != bt_routing) {
            bt_routing = bt_out;
            eac_apply_route();          /* mute local amps on, restore off */
            apply_sw_volume();          /* unity to the sink while BT-routed; restore local gain on exit */
        }
        if (bt_out && addr && size)
            bt_client_pcm_write((const int16_t *) addr, size / 4);

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
        /* Hold the sink lock across the mixer calls.  These run the
         * mixer/pcmbuf code that the cooperative threads also touch under
         * pcm_play_lock (which maps to this same recursive worker_mtx); run
         * from this raw pthread without the lock they race them. */
        pthread_mutex_lock(&worker_mtx);
        /* Two-step handoff, mirroring the reference hosted driver (pcm-alsa.c):
         *   1. status callback (PCM_DMAST_STARTED) -> mixer_buffer_callback,
         *      which mixes the next frame and ADVANCES the playback channel
         *      (this is what consumes pcmbuf, so it drives the playhead);
         *   2. complete callback -> mixer_pcm_callback, which only hands back
         *      the frame the status callback just prepared.
         * Calling only (2) -- as this driver did -- replays the primed frame
         * forever: silent output, no pcmbuf consumption, playhead stuck at
         * 0:00.  Skip the mix-ahead once after a (re)start because
         * mixer_start_pcm() already primed two frames of look-ahead. */
        if (final_status >= PCM_DMAST_OK) {
            if (mixer_primed)
                pcm_play_dma_status_callback(PCM_DMAST_STARTED);
            else
                mixer_primed = true;
        }
        bool more = pcm_play_dma_complete_callback(final_status,
                                                   &next_addr, &next_size);
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

    /* Sensible default before the HAVE_SPEAKER framework first resolves the
     * route at settings-apply: speaker when nothing is plugged, headphones
     * otherwise (matches "Auto"). */
    want_speaker = !headphones_inserted();
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

/* Push the effective gain to the PCM SW scaler.  Local output uses the core's
 * requested centibel level; BT output streams at unity so the sink's AVRCP
 * absolute volume is the only attenuation (no double-attenuate). */
static void apply_sw_volume(void)
{
    enum { Y1_MUTE_CB = -730 };   /* keep in sync with y1mtk_codec.h VOLUME min */
    if (bt_routing) {
        pcm_set_master_volume(0, 0);   /* 0 cb = 0 dB = unity, full-scale PCM */
        return;
    }
    pcm_set_master_volume(sw_vol_l <= Y1_MUTE_CB ? PCM_MUTE_LEVEL : sw_vol_l,
                          sw_vol_r <= Y1_MUTE_CB ? PCM_MUTE_LEVEL : sw_vol_r);
}

void audiohw_set_volume(int vol_l, int vol_r)
{
    /* No usable HW volume: the MT6323 gain registers (SET_ANAAFE_REG
     * 0xc0044302) aren't in the BSP source, so attenuation is done in software
     * (HAVE_SW_VOLUME_CONTROL).  This MUST forward the per-channel centibel
     * level to the PCM SW scaler -- without it the scaler's factor stays 0 and
     * ALL output is silenced (the "playhead moves but no audio" fault).
     * Mirrors firmware/drivers/audio/dummy_codec.c.  The VOLUME setting floor
     * (-73 dB -> -730 cb, y1mtk_codec.h) maps to a true digital mute. */
    sw_vol_l = vol_l;
    sw_vol_r = vol_r;
    apply_sw_volume();
}

void audiohw_mute(int mute)
{
    (void)mute;
}

#ifdef HAVE_SPEAKER
void audiohw_enable_speaker(bool on)
{
    /* Called by audio_enable_speaker() with the resolved Speaker-setting +
     * jack decision: on => speaker, off => headphone.  Record it and apply
     * immediately if a stream is live (eac_apply_route no-ops otherwise). */
    want_speaker = on;
    eac_apply_route();
}
#endif

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
    mixer_primed  = false;   /* re-prime in phase with mixer_start_pcm's look-ahead */
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
