/*
 * Innioasis Y1 — button_map() for hosted/button-devinput.c
 *
 * event0 (mtk-kpd) capabilities (confirmed via `getevent -p`):
 *   102 HOME, 103 UP, 105 LEFT, 106 RIGHT, 107 END, 108 DOWN,
 *   114 VOLDN, 115 VOLUP, 116 POWER, 139 MENU, 158 BACK,
 *   211/212/231/232 — MTK-reserved (unused)
 *
 *   PLAY button assignment is unconfirmed: KEY_PLAYPAUSE (164) is NOT in
 *   event0's capability list, so the physical PLAY emits one of the codes
 *   above. Most likely KEY_MENU (139) or KEY_END (107). Needs per-press
 *   capture to pin down (see open-questions.md #5).
 *
 * event1 (ACCDET / headset): 163 NEXT, 164 PLAYPAUSE, 165 PREV, 166 STOP
 * event4 (AVRCP / Bluetooth): media keys
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
        case KEY_MENU:           return BUTTON_PLAY;  /* PROVISIONAL: confirm via per-button getevent */
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
