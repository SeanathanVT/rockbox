/*
 * Innioasis Y1 — button-target.h
 *
 * Physical layout (preliminary — verify each on-device with getevent):
 *   - 5 capacitive nav buttons across bottom bezel: LEFT, RIGHT, HOME, BACK, PLAY
 *   - 1 physical power button on side
 *   - Headphone-jack 3-button remote (PLAY_PAUSE, NEXT, PREV) via ACCDET
 *   - Bluetooth AVRCP keys via /dev/input/event4
 *
 * The touchscreen lives on /dev/input/event2 and is currently unused by the port.
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
