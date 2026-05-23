/*
 * Innioasis Y1 — button-target.h
 *
 * Physical layout (iPod-style click wheel):
 *   - Top button    -- MENU / back
 *   - Center button -- SELECT
 *   - Bottom button -- PLAY / pause / stop
 *   - Left button   -- previous track
 *   - Right button  -- next track
 *   - Capacitive ring around the cardinals -- scroll wheel
 *     (list scroll; volume in WPS)
 *   - No physical power button. KEY_POWER (116) is in the mtk-kpd capability
 *     list and stock keylayout, but how/whether it emits on this unit is TBD.
 *   - Headphone-jack 3-button remote (PLAY_PAUSE, NEXT, PREV) via ACCDET
 *   - Bluetooth AVRCP keys via /dev/input/event4
 *
 * The bit definitions below still reflect the earlier (incorrect) 5-button
 * bezel model; they get replaced in lockstep with button-y1.c +
 * keymap-innioasisy1.c once the getevent capture from tools/capture-input.sh
 * (in y1-platform) confirms which evdev codes the wheel and cardinals emit.
 */

#ifndef _BUTTON_TARGET_H_
#define _BUTTON_TARGET_H_

#include <stdbool.h>
#include "config.h"

/* Main unit buttons */
#define BUTTON_POWER    0x00000001
#define BUTTON_BACK     0x00000002
#define BUTTON_HOME     0x00000004
#define BUTTON_PLAY     0x00000008
#define BUTTON_PREV     0x00000010
#define BUTTON_NEXT     0x00000020
#define BUTTON_MAIN     0x0000003f

/* Software power-off via long-press of POWER */
#define POWEROFF_BUTTON BUTTON_POWER
#define POWEROFF_COUNT  25

int button_map(int keycode);

#endif /* _BUTTON_TARGET_H_ */
