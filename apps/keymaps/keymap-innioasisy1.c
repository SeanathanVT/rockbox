/*
 * Innioasis Y1 — keymap
 *
 * iPod-style click wheel:
 *   - MENU   (top)     navigates up / out of menus / cancel
 *   - SELECT (center)  OK / context (long-press)
 *   - PLAY   (bottom)  play/pause in WPS; soft power-off on long-press
 *   - LEFT   / RIGHT   skip prev/next in WPS; seek on long-press
 *   - SCROLL_BACK/FWD  list scroll; volume in WPS; setting inc/dec
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
    { ACTION_STD_OK,         BUTTON_SELECT | BUTTON_REL,    BUTTON_SELECT },
    { ACTION_STD_CONTEXT,    BUTTON_SELECT | BUTTON_REPEAT, BUTTON_SELECT },
    { ACTION_STD_CANCEL,     BUTTON_MENU | BUTTON_REL,      BUTTON_MENU },
    { ACTION_STD_MENU,       BUTTON_MENU | BUTTON_REPEAT,   BUTTON_MENU },
    { ACTION_STD_PREV,       BUTTON_SCROLL_BACK,            BUTTON_NONE },
    { ACTION_STD_NEXT,       BUTTON_SCROLL_FWD,             BUTTON_NONE },
    { ACTION_STD_PREVREPEAT, BUTTON_SCROLL_BACK|BUTTON_REPEAT, BUTTON_NONE },
    { ACTION_STD_NEXTREPEAT, BUTTON_SCROLL_FWD|BUTTON_REPEAT,  BUTTON_NONE },

    LAST_ITEM_IN_LIST
};

static const struct button_mapping button_context_wps[] =
{
    { ACTION_WPS_PLAY,        BUTTON_PLAY | BUTTON_REL,        BUTTON_PLAY },
    { ACTION_WPS_STOP,        BUTTON_PLAY | BUTTON_REPEAT,     BUTTON_PLAY },
    { ACTION_WPS_MENU,        BUTTON_MENU | BUTTON_REL,        BUTTON_MENU },
    { ACTION_WPS_BROWSE,      BUTTON_SELECT | BUTTON_REL,      BUTTON_SELECT },
    { ACTION_WPS_CONTEXT,     BUTTON_SELECT | BUTTON_REPEAT,   BUTTON_SELECT },
    { ACTION_WPS_SKIPPREV,    BUTTON_LEFT | BUTTON_REL,        BUTTON_LEFT },
    { ACTION_WPS_SKIPNEXT,    BUTTON_RIGHT | BUTTON_REL,       BUTTON_RIGHT },
    { ACTION_WPS_SEEKBACK,    BUTTON_LEFT | BUTTON_REPEAT,     BUTTON_NONE },
    { ACTION_WPS_SEEKFWD,     BUTTON_RIGHT | BUTTON_REPEAT,    BUTTON_NONE },
    { ACTION_WPS_STOPSEEK,    BUTTON_LEFT | BUTTON_REL,        BUTTON_LEFT | BUTTON_REPEAT },
    { ACTION_WPS_STOPSEEK,    BUTTON_RIGHT | BUTTON_REL,       BUTTON_RIGHT | BUTTON_REPEAT },
    { ACTION_WPS_VOLDOWN,     BUTTON_SCROLL_BACK,              BUTTON_NONE },
    { ACTION_WPS_VOLUP,       BUTTON_SCROLL_FWD,               BUTTON_NONE },

    LAST_ITEM_IN_LIST
};

static const struct button_mapping button_context_list[] =
{
    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD),
};

static const struct button_mapping button_context_tree[] =
{
    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_LIST),
};

static const struct button_mapping button_context_settings[] =
{
    { ACTION_SETTINGS_INC,       BUTTON_SCROLL_FWD,               BUTTON_NONE },
    { ACTION_SETTINGS_DEC,       BUTTON_SCROLL_BACK,              BUTTON_NONE },
    { ACTION_SETTINGS_INCREPEAT, BUTTON_SCROLL_FWD|BUTTON_REPEAT, BUTTON_NONE },
    { ACTION_SETTINGS_DECREPEAT, BUTTON_SCROLL_BACK|BUTTON_REPEAT, BUTTON_NONE },

    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD),
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
        case CONTEXT_YESNOSCREEN:   return button_context_yesno;
        default:                    return button_context_standard;
    }
}
