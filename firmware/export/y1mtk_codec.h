/*
 * Innioasis Y1 — audiohw definitions for the (currently stub) /dev/eac driver.
 *
 * See docs/audio-stack.md and docs/audio-re-progress.md for the live RE.
 * Until the MTK ioctl protocol on /dev/eac is mapped, pcm-y1mtk.c is a stub
 * that lets the build link but produces no sound.
 */

#ifndef __Y1MTK_CODEC_H__
#define __Y1MTK_CODEC_H__

#define AUDIOHW_CAPS 0

/* Software-volume target: actual hardware-volume control on Y1 is presumably
 * an /dev/eac ioctl (TBD during RE). Until then, volume is software-only. */
AUDIOHW_SETTING(VOLUME, "dB", 0, 1, -73, 6, -25)

#endif /* __Y1MTK_CODEC_H__ */
