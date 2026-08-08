#pragma once

/*
 * Fable's UI component vocabulary.
 *
 * The component factory (see CUIFactory) dispatches on a type id stored in the
 * definition at +0x3C. These are those ids, together with the object size the
 * factory allocates and the constructor it calls -- decoded from the factory's
 * jump table at 0x0041D7F8 and verified against live definitions.
 *
 * See docs/ui-system.md for how this was derived.
 */

enum EUIComponentType
{
    UI_SPRITE                   = 0x00,
    UI_MORPHING_SPRITE          = 0x01,
    UI_TABLE                    = 0x02,
    UI_MESH                     = 0x03,
    UI_INTERFACE                = 0x04,
    UI_CHANGING_STATE_COMPONENT = 0x05,
    UI_TEXT                     = 0x06,
    UI_MENU_ENTRY               = 0x07,
    UI_LIST                     = 0x08,
    UI_VIEWPORT                 = 0x09,
    UI_FRONT_END_SCREEN         = 0x0A,
    UI_FRONT_END_BUTTON         = 0x0B,
    UI_FRONT_END_LIST           = 0x0C,
    UI_SCROLLING_VIEWPORT       = 0x0D,
    UI_LIST_ARROW               = 0x0E,
    UI_SLIDER                   = 0x0F,
    UI_TEXT_SLIDER              = 0x10,
    UI_MOVIE                    = 0x11,
    UI_SWAPPING_STATE_COMPONENT = 0x12,
    UI_SCROLLING_COMPONENT      = 0x13,
    UI_TEXT_CONTAINER           = 0x14,
    UI_ZOOMING_COMPONENT        = 0x15,
    UI_COMPONENT_CONTAINER      = 0x16,
    UI_SPELL_CONTAINER          = 0x17,
    UI_SPELL_CONTAINER_LIST     = 0x18,
    UI_YES_NO                   = 0x19,
    UI_OK                       = 0x1A,
    UI_PARTICLE_EFFECT          = 0x1B,
    UI_CONTROLLER_DISCONNECT    = 0x1C,
    UI_DEFINITION_MANAGER       = 0x1D,   /* factory fallback, not a widget */
    UI_ICON_TEXT                = 0x1E,
    UI_DYNAMIC_LIST             = 0x1F,
    UI_MOUSE_CURSOR             = 0x20,
    UI_HOVERABLE                = 0x21,
    UI_CLICKABLE                = 0x22,
    UI_DRAGGABLE                = 0x23,
    UI_DRAGGABLE_INTO           = 0x24,
    UI_EDIT_BOX                 = 0x25,
    UI_NAV_BUTTON               = 0x26,
    UI_KEY_REDEFINER            = 0x27,
    UI_REDEFINER_LIST           = 0x28,
    UI_SCROLL_BAR               = 0x29,
    UI_SCROLL_BAR_OUTSIDE       = 0x2A,
    UI_SCROLLABLE_LIST          = 0x2B,

    UI_COMPONENT_TYPE_COUNT     = 0x2C
};

/* Object size and constructor per type, indexed by EUIComponentType. Useful
 * when constructing a component directly rather than through the factory. */
struct SUIComponentInfo
{
    unsigned int size;
    unsigned int constructor;
    const char*  name;
};

extern const SUIComponentInfo g_UIComponentInfo[UI_COMPONENT_TYPE_COUNT];
