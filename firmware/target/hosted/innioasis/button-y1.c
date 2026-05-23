/*
 * Innioasis Y1 — button_map() for hosted/button-devinput.c
 *
 * Verified keycodes (from kernel dmesg `mtk-tpd: apt32 ctrl report Linux keycode`
 * + getevent on a live device):
 *   105 KEY_LEFT       — capacitive LEFT
 *   106 KEY_RIGHT      — capacitive RIGHT
 *   158 KEY_BACK       — capacitive BACK
 *
 * Inferred from stock mtk-kpd.kl + AVRCP.kl (verify per-button on-device):
 *   102 KEY_HOME       — capacitive HOME (center)
 *   164 KEY_PLAYPAUSE  — capacitive PLAY (also headset + AVRCP via event4)
 *   116 KEY_POWER      — physical power button (/dev/input/event0)
 *   163 KEY_NEXTSONG   — headset / AVRCP only
 *   165 KEY_PREVIOUSSONG — headset / AVRCP only
 */

#include <linux/input.h>
#include "button.h"
#include "button-target.h"

int button_map(int keycode)
{
    switch (keycode)
    {
        case KEY_LEFT:           return BUTTON_PREV;
        case KEY_RIGHT:          return BUTTON_NEXT;
        case KEY_HOME:           return BUTTON_HOME;
        case KEY_BACK:           return BUTTON_BACK;
        case KEY_PLAYPAUSE:      return BUTTON_PLAY;
        case KEY_POWER:          return BUTTON_POWER;
        /* Headset / AVRCP remote keys — map to the same actions */
        case KEY_NEXTSONG:       return BUTTON_NEXT;
        case KEY_PREVIOUSSONG:   return BUTTON_PREV;
        default:                 return 0;
    }
}

bool headphones_inserted(void)
{
    /* TODO: read /sys/class/switch/h2w/state once on-device path is confirmed.
     * Stock /system/lib/libaudio.primary.default.so references this sysfs node. */
    return true;
}
