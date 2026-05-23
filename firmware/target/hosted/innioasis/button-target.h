/*
 * Innioasis Y1 — button-target.h
 *
 * Physical layout (iPod-style click wheel; verified via
 * y1-platform/tools/capture-input.sh, 2026-05-23):
 *
 *   Physical              evdev node            keycode
 *   --------              ----------            -------
 *   Top button (menu)     /dev/input/event2     KEY_BACK         (158)
 *   Center button         /dev/input/event0     KEY_REPLY        (232)
 *   Bottom button (play)  /dev/input/event2     KEY_PLAYPAUSE    (164)
 *   Left button (prev)    /dev/input/event2     KEY_PREVIOUSSONG (165)
 *   Right button (next)   /dev/input/event2     KEY_NEXTSONG     (163)
 *   Wheel CCW (one tick)  /dev/input/event2     KEY_LEFT         (105)
 *   Wheel CW  (one tick)  /dev/input/event2     KEY_RIGHT        (106)
 *
 * Each wheel "click" is one paired DOWN/UP of KEY_LEFT or KEY_RIGHT.
 * Half a rotation = ~4 ticks. button-devinput.c's HAVE_SCROLLWHEEL path
 * counts press+release pairs and posts one BUTTON_SCROLL_* per tick.
 *
 * No physical power button. KEY_POWER (116) is in the mtk-kpd capability
 * list but is not emitted by any user-reachable control.
 *
 * Auxiliary inputs (unchanged):
 *   - Headphone 3-button remote (PLAY_PAUSE, NEXT, PREV) via /dev/input/event1 (ACCDET)
 *   - Bluetooth AVRCP keys via /dev/input/event4
 *   - Touchscreen on /dev/input/event2 (ABS axes, 480x360) -- unused by this port
 */

#ifndef _BUTTON_TARGET_H_
#define _BUTTON_TARGET_H_

#include <stdbool.h>
#include "config.h"

/* Click-wheel buttons */
#define BUTTON_MENU         0x00000001  /* top */
#define BUTTON_SELECT       0x00000002  /* center */
#define BUTTON_PLAY         0x00000004  /* bottom */
#define BUTTON_LEFT         0x00000008  /* left, previous track */
#define BUTTON_RIGHT        0x00000010  /* right, next track */

/* Wheel ticks (one event per click) */
#define BUTTON_SCROLL_BACK  0x00000020  /* wheel CCW */
#define BUTTON_SCROLL_FWD   0x00000040  /* wheel CW */

#define BUTTON_MAIN         (BUTTON_MENU|BUTTON_SELECT|BUTTON_PLAY| \
                             BUTTON_LEFT|BUTTON_RIGHT| \
                             BUTTON_SCROLL_BACK|BUTTON_SCROLL_FWD)

/* Software power-off: long-press of PLAY (no hardware power button). */
#define POWEROFF_BUTTON BUTTON_PLAY
#define POWEROFF_COUNT  30

int button_map(int keycode);
bool headphones_inserted(void);

#endif /* _BUTTON_TARGET_H_ */
