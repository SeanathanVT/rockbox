/*
 * Innioasis Y1 — keymap
 *
 * Click wheel, matching the glyphs etched on the device:
 *   MENU   (top)     Back (short) / Menu (long)
 *   SELECT (center)  OK / context menu (long)
 *   PLAY   (bottom)  Play-Pause / Stop (long)
 *   LEFT  / RIGHT    prev / next arrows: skip track in WPS, move between
 *                    fields in settings, cursor in keyboard, back/enter in lists
 *   SCROLL back/fwd  list scroll; value inc/dec in settings; volume in WPS;
 *                    cursor up/down in keyboard
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "config.h"
#include "action.h"
#include "button.h"
#include "settings.h"

static const struct button_mapping button_context_standard[] =
{
    { ACTION_STD_OK,         BUTTON_SELECT | BUTTON_REL,        BUTTON_SELECT },
    { ACTION_STD_OK,         BUTTON_RIGHT,                      BUTTON_NONE },
    { ACTION_STD_CANCEL,     BUTTON_LEFT,                       BUTTON_NONE },
    { ACTION_STD_CANCEL,     BUTTON_MENU | BUTTON_REL,          BUTTON_MENU },
    { ACTION_STD_CONTEXT,    BUTTON_MENU | BUTTON_REPEAT,       BUTTON_MENU },
    { ACTION_STD_PREV,       BUTTON_SCROLL_BACK,                BUTTON_NONE },
    { ACTION_STD_NEXT,       BUTTON_SCROLL_FWD,                 BUTTON_NONE },
    { ACTION_STD_PREVREPEAT, BUTTON_SCROLL_BACK | BUTTON_REPEAT, BUTTON_NONE },
    { ACTION_STD_NEXTREPEAT, BUTTON_SCROLL_FWD | BUTTON_REPEAT,  BUTTON_NONE },

    LAST_ITEM_IN_LIST
};

static const struct button_mapping button_context_wps[] =
{
    { ACTION_WPS_PLAY,        BUTTON_PLAY | BUTTON_REL,        BUTTON_PLAY },
    { ACTION_WPS_STOP,        BUTTON_PLAY | BUTTON_REPEAT,     BUTTON_PLAY },
    { ACTION_WPS_HOTKEY,      BUTTON_SELECT | BUTTON_PLAY,     BUTTON_NONE },
    { ACTION_WPS_BROWSE,      BUTTON_MENU | BUTTON_REL,        BUTTON_MENU },
    { ACTION_WPS_CONTEXT,     BUTTON_MENU | BUTTON_REPEAT,     BUTTON_MENU },
    { ACTION_WPS_MENU,        BUTTON_SELECT | BUTTON_REL,      BUTTON_SELECT },
    { ACTION_WPS_QUICKSCREEN, BUTTON_SELECT | BUTTON_REPEAT,   BUTTON_SELECT },
    { ACTION_WPS_SKIPPREV,    BUTTON_LEFT | BUTTON_REL,        BUTTON_LEFT },
    { ACTION_WPS_SKIPNEXT,    BUTTON_RIGHT | BUTTON_REL,       BUTTON_RIGHT },
    { ACTION_WPS_SEEKBACK,    BUTTON_LEFT | BUTTON_REPEAT,     BUTTON_NONE },
    { ACTION_WPS_SEEKFWD,     BUTTON_RIGHT | BUTTON_REPEAT,    BUTTON_NONE },
    { ACTION_WPS_STOPSEEK,    BUTTON_LEFT | BUTTON_REL,        BUTTON_LEFT | BUTTON_REPEAT },
    { ACTION_WPS_STOPSEEK,    BUTTON_RIGHT | BUTTON_REL,       BUTTON_RIGHT | BUTTON_REPEAT },
    { ACTION_WPS_VOLDOWN,     BUTTON_SCROLL_BACK,              BUTTON_NONE },
    { ACTION_WPS_VOLDOWN,     BUTTON_SCROLL_BACK | BUTTON_REPEAT, BUTTON_NONE },
    { ACTION_WPS_VOLUP,       BUTTON_SCROLL_FWD,               BUTTON_NONE },
    { ACTION_WPS_VOLUP,       BUTTON_SCROLL_FWD | BUTTON_REPEAT,  BUTTON_NONE },

    LAST_ITEM_IN_LIST
};

static const struct button_mapping button_context_tree[] =
{
    { ACTION_TREE_WPS,    BUTTON_PLAY | BUTTON_REL,    BUTTON_PLAY },
    { ACTION_TREE_STOP,   BUTTON_PLAY | BUTTON_REPEAT, BUTTON_PLAY },
    { ACTION_TREE_HOTKEY, BUTTON_SELECT | BUTTON_PLAY, BUTTON_NONE },

    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD),
};

static const struct button_mapping button_context_list[] =
{
    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD),
};

static const struct button_mapping button_context_settings[] =
{
    { ACTION_SETTINGS_INC,       BUTTON_SCROLL_FWD,                 BUTTON_NONE },
    { ACTION_SETTINGS_DEC,       BUTTON_SCROLL_BACK,                BUTTON_NONE },
    { ACTION_SETTINGS_INCREPEAT, BUTTON_SCROLL_FWD | BUTTON_REPEAT, BUTTON_NONE },
    { ACTION_SETTINGS_DECREPEAT, BUTTON_SCROLL_BACK | BUTTON_REPEAT, BUTTON_NONE },
    { ACTION_STD_PREV,           BUTTON_LEFT,                       BUTTON_NONE },
    { ACTION_STD_PREVREPEAT,     BUTTON_LEFT | BUTTON_REPEAT,       BUTTON_NONE },
    { ACTION_STD_NEXT,           BUTTON_RIGHT,                      BUTTON_NONE },
    { ACTION_STD_NEXTREPEAT,     BUTTON_RIGHT | BUTTON_REPEAT,      BUTTON_NONE },

    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD),
};

static const struct button_mapping button_context_keyboard[] =
{
    { ACTION_KBD_LEFT,         BUTTON_LEFT,                        BUTTON_NONE },
    { ACTION_KBD_LEFT,         BUTTON_LEFT | BUTTON_REPEAT,        BUTTON_NONE },
    { ACTION_KBD_RIGHT,        BUTTON_RIGHT,                       BUTTON_NONE },
    { ACTION_KBD_RIGHT,        BUTTON_RIGHT | BUTTON_REPEAT,       BUTTON_NONE },
    { ACTION_KBD_UP,           BUTTON_SCROLL_BACK,                 BUTTON_NONE },
    { ACTION_KBD_UP,           BUTTON_SCROLL_BACK | BUTTON_REPEAT, BUTTON_NONE },
    { ACTION_KBD_DOWN,         BUTTON_SCROLL_FWD,                  BUTTON_NONE },
    { ACTION_KBD_DOWN,         BUTTON_SCROLL_FWD | BUTTON_REPEAT,  BUTTON_NONE },
    { ACTION_KBD_SELECT,       BUTTON_SELECT,                      BUTTON_NONE },
    { ACTION_KBD_DONE,         BUTTON_PLAY,                        BUTTON_NONE },
    { ACTION_KBD_ABORT,        BUTTON_MENU | BUTTON_REL,           BUTTON_MENU },
    { ACTION_KBD_MORSE_INPUT,  BUTTON_MENU | BUTTON_REPEAT,        BUTTON_MENU },
    { ACTION_KBD_MORSE_SELECT, BUTTON_SELECT | BUTTON_REL,         BUTTON_NONE },

    LAST_ITEM_IN_LIST
};

static const struct button_mapping button_context_quickscreen[] =
{
    { ACTION_QS_TOP,     BUTTON_MENU,                        BUTTON_NONE },
    { ACTION_QS_TOP,     BUTTON_MENU | BUTTON_REPEAT,        BUTTON_NONE },
    { ACTION_QS_DOWN,    BUTTON_PLAY,                        BUTTON_NONE },
    { ACTION_QS_DOWN,    BUTTON_PLAY | BUTTON_REPEAT,        BUTTON_NONE },
    { ACTION_QS_LEFT,    BUTTON_LEFT,                        BUTTON_NONE },
    { ACTION_QS_LEFT,    BUTTON_LEFT | BUTTON_REPEAT,        BUTTON_NONE },
    { ACTION_QS_RIGHT,   BUTTON_RIGHT,                       BUTTON_NONE },
    { ACTION_QS_RIGHT,   BUTTON_RIGHT | BUTTON_REPEAT,       BUTTON_NONE },
    { ACTION_STD_CANCEL, BUTTON_SELECT | BUTTON_REL,         BUTTON_SELECT },
    { ACTION_QS_VOLDOWN, BUTTON_SCROLL_BACK,                 BUTTON_NONE },
    { ACTION_QS_VOLDOWN, BUTTON_SCROLL_BACK | BUTTON_REPEAT, BUTTON_NONE },
    { ACTION_QS_VOLUP,   BUTTON_SCROLL_FWD,                  BUTTON_NONE },
    { ACTION_QS_VOLUP,   BUTTON_SCROLL_FWD | BUTTON_REPEAT,  BUTTON_NONE },

    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD),
};

static const struct button_mapping button_context_pitchscreen[] =
{
    { ACTION_PS_INC_SMALL,      BUTTON_SCROLL_FWD,                  BUTTON_NONE },
    { ACTION_PS_INC_BIG,        BUTTON_SCROLL_FWD | BUTTON_REPEAT,  BUTTON_NONE },
    { ACTION_PS_DEC_SMALL,      BUTTON_SCROLL_BACK,                 BUTTON_NONE },
    { ACTION_PS_DEC_BIG,        BUTTON_SCROLL_BACK | BUTTON_REPEAT, BUTTON_NONE },
    { ACTION_PS_NUDGE_LEFT,     BUTTON_LEFT,                        BUTTON_NONE },
    { ACTION_PS_NUDGE_LEFTOFF,  BUTTON_LEFT | BUTTON_REL,           BUTTON_NONE },
    { ACTION_PS_NUDGE_RIGHT,    BUTTON_RIGHT,                       BUTTON_NONE },
    { ACTION_PS_NUDGE_RIGHTOFF, BUTTON_RIGHT | BUTTON_REL,          BUTTON_NONE },
    { ACTION_PS_TOGGLE_MODE,    BUTTON_PLAY,                        BUTTON_NONE },
    { ACTION_PS_EXIT,           BUTTON_MENU,                        BUTTON_NONE },
    { ACTION_PS_RESET,          BUTTON_SELECT | BUTTON_REPEAT,      BUTTON_SELECT },

    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD),
};

static const struct button_mapping button_context_bmark[] =
{
    { ACTION_BMS_DELETE, BUTTON_MENU | BUTTON_REPEAT, BUTTON_MENU },

    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_LIST),
};

static const struct button_mapping button_context_yesno[] =
{
    { ACTION_YESNO_ACCEPT, BUTTON_SELECT | BUTTON_REL, BUTTON_SELECT },

    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD),
};

const struct button_mapping *get_context_mapping(int context)
{
    switch (context)
    {
        case CONTEXT_STD:           return button_context_standard;
        case CONTEXT_WPS:           return button_context_wps;
        case CONTEXT_LIST:          return button_context_list;
        case CONTEXT_TREE:
        case CONTEXT_MAINMENU:      return button_context_tree;
        case CONTEXT_SETTINGS:
        case CONTEXT_SETTINGS_EQ:
        case CONTEXT_SETTINGS_COLOURCHOOSER:
        case CONTEXT_SETTINGS_TIME:
        case CONTEXT_SETTINGS_RECTRIGGER: return button_context_settings;
        case CONTEXT_KEYBOARD:
        case CONTEXT_MORSE_INPUT:   return button_context_keyboard;
        case CONTEXT_QUICKSCREEN:   return button_context_quickscreen;
        case CONTEXT_PITCHSCREEN:   return button_context_pitchscreen;
        case CONTEXT_BOOKMARKSCREEN: return button_context_bmark;
        case CONTEXT_YESNOSCREEN:   return button_context_yesno;
        default:                    return button_context_standard;
    }
}
