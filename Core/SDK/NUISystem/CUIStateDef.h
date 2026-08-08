#pragma once

/*
 * CUIStateDef -- one visual state of a UI component (vtable 0x0125871C).
 *
 * A CUIDef owns a std::vector<CUIStateDef> at +0x40; a component has one record
 * per visual state (hover, pressed, disabled...). Observed live: CSprite 1
 * state, CText 2-4, CFrontEndScreen 8.
 *
 * Offsets come from diffing state [0] across definitions of the same type. The
 * field *names* below are inferred from value shape and are not yet proven by
 * writing to them -- treat them as a map, not gospel. See docs/ui-system.md.
 */

class CUIStateDef
{
public:
    char pad_0000[0x24];        /* +0x00 vtable, +0x04 refcount, ...        */
    int  offset;                /* +0x24 signed offset (-256, -160, -80...) */
    char pad_0028[0x04];        /* +0x28 alpha, seen 255                    */
    char pad_002C[0x10];        /* +0x2C .. +0x38                           */
    unsigned int asset_id;      /* +0x3C banked graphic id (273, 50)        */
    unsigned int asset_id2;     /* +0x40 second id (34, 7)                  */
    char pad_0044[0x04];        /* +0x44                                    */
    float coord_a;              /* +0x48 coordinate / extent                */
    float coord_b;              /* +0x4C coordinate / extent                */
    float channels[6];          /* +0x50 .. +0x64 colour / scale, mostly 1  */
    char pad_0068[0x14];        /* +0x68 .. +0x7B                           */
};

#define UI_STATE_DEF_SIZE 0x7C  /* sizeof(CUIStateDef), verified live */
