/*
 * Innioasis Y1 — button_map() for hosted/button-devinput.c
 *
 * event0 (mtk-kpd) capability list (getevent -p, 2026-05-23):
 *   102 HOME, 103 UP, 105 LEFT, 106 RIGHT, 107 END, 108 DOWN,
 *   114 VOLDN, 115 VOLUP, 116 POWER, 139 MENU, 158 BACK,
 *   211 FOCUS, 212 CAMERA, 231 CALL, 232 REPLY
 *
 * Stock /system/usr/keylayout/mtk-kpd.kl translations:
 *   102 HOME, 105/106 DPAD_LEFT/RIGHT, 116 POWER, 158 BACK,
 *   232 DPAD_CENTER  <-- this is the Y1's center / OK / PLAY key
 *
 * event1 (ACCDET / headset): 163 NEXT, 164 PLAYPAUSE, 165 PREV, 166 STOP
 * event4 (AVRCP / Bluetooth): media keys
 *
 * Per-button getevent capture still pending to confirm KEY_REPLY emits on
 * physical PLAY press (vs KEY_MENU as the fallback).
 */

#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
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
        case KEY_REPLY:          return BUTTON_PLAY;  /* 232 -> DPAD_CENTER per mtk-kpd.kl */
        case KEY_MENU:           return BUTTON_PLAY;  /* fallback if PLAY actually emits 139 */
        case KEY_POWER:          return BUTTON_POWER;
        /* Headset / AVRCP remote keys */
        case KEY_PLAYPAUSE:      return BUTTON_PLAY;
        case KEY_NEXTSONG:       return BUTTON_NEXT;
        case KEY_PREVIOUSSONG:   return BUTTON_PREV;
        default:                 return 0;
    }
}

bool headphones_inserted(void)
{
    /* /sys/class/switch/h2w/state: "0" = unplugged, "1" = plugged.
     * Confirmed present on stock Y1. */
    int fd = open("/sys/class/switch/h2w/state", O_RDONLY);
    if (fd < 0) return false;
    char c = '0';
    read(fd, &c, 1);
    close(fd);
    return c != '0';
}
