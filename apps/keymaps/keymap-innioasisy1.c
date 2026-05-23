/*
 * Innioasis Y1 — keymap
 *
 * Provisional mapping. The Y1 has a 5-button capacitive nav row + side power button.
 * Treat PLAY as the "OK / center", BACK as the "menu / cancel", HOME as the "context",
 * PREV/NEXT as list scrolling and (with REPEAT) seek in WPS.
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
    { ACTION_STD_OK,         BUTTON_PLAY | BUTTON_REL,    BUTTON_PLAY },
    { ACTION_STD_CONTEXT,    BUTTON_PLAY | BUTTON_REPEAT, BUTTON_PLAY },
    { ACTION_STD_CANCEL,     BUTTON_BACK | BUTTON_REL,    BUTTON_BACK },
    { ACTION_STD_MENU,       BUTTON_HOME | BUTTON_REL,    BUTTON_HOME },
    { ACTION_STD_PREV,       BUTTON_PREV,                 BUTTON_NONE },
    { ACTION_STD_NEXT,       BUTTON_NEXT,                 BUTTON_NONE },
    { ACTION_STD_PREVREPEAT, BUTTON_PREV | BUTTON_REPEAT, BUTTON_NONE },
    { ACTION_STD_NEXTREPEAT, BUTTON_NEXT | BUTTON_REPEAT, BUTTON_NONE },

    LAST_ITEM_IN_LIST
};

static const struct button_mapping button_context_wps[] =
{
    { ACTION_WPS_PLAY,     BUTTON_PLAY | BUTTON_REL,        BUTTON_PLAY },
    { ACTION_WPS_CONTEXT,  BUTTON_PLAY | BUTTON_REPEAT,     BUTTON_NONE },
    { ACTION_WPS_MENU,     BUTTON_HOME | BUTTON_REL,        BUTTON_NONE },
    { ACTION_WPS_BROWSE,   BUTTON_BACK | BUTTON_REL,        BUTTON_BACK },
    { ACTION_WPS_SEEKBACK, BUTTON_PREV | BUTTON_REPEAT,     BUTTON_NONE },
    { ACTION_WPS_SEEKFWD,  BUTTON_NEXT | BUTTON_REPEAT,     BUTTON_NONE },
    { ACTION_WPS_STOPSEEK, BUTTON_PREV | BUTTON_REL,        BUTTON_PREV | BUTTON_REPEAT },
    { ACTION_WPS_STOPSEEK, BUTTON_NEXT | BUTTON_REL,        BUTTON_NEXT | BUTTON_REPEAT },
    { ACTION_WPS_SKIPPREV, BUTTON_PREV | BUTTON_REL,        BUTTON_NONE },
    { ACTION_WPS_SKIPNEXT, BUTTON_NEXT | BUTTON_REL,        BUTTON_NONE },

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
    { ACTION_SETTINGS_INC,       BUTTON_NEXT,                 BUTTON_NONE },
    { ACTION_SETTINGS_DEC,       BUTTON_PREV,                 BUTTON_NONE },
    { ACTION_SETTINGS_INCREPEAT, BUTTON_NEXT | BUTTON_REPEAT, BUTTON_NONE },
    { ACTION_SETTINGS_DECREPEAT, BUTTON_PREV | BUTTON_REPEAT, BUTTON_NONE },

    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD),
};

static const struct button_mapping button_context_yesno[] =
{
    { ACTION_YESNO_ACCEPT, BUTTON_PLAY | BUTTON_REL, BUTTON_PLAY },

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
