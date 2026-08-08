#pragma once

#include "../Utils/Hook.h"
#include "UITypes.h"

/*
 * CUIDef -- the definition a UI component is built from (vtable 0x01259F8C).
 *
 * Every UI element in Fable is data: a named definition, looked up by hash,
 * that the component factory turns into a live object. Offsets below were
 * captured from real definitions at runtime; see docs/ui-system.md.
 */
class CUIDef
{
public:
    char pad_0000[0x08];        /* +0x00 vtable, +0x04 refcount            */
    void* manager;              /* +0x08 owning definition manager         */
    char pad_000C[0x08];        /* +0x0C, +0x10 parent / sibling links     */
    unsigned int id;            /* +0x14 per-definition id                 */
    char pad_0018[0x04];        /* +0x18                                   */
    void* manager_alias;        /* +0x1C same pointer as `manager`         */
    unsigned int id2;           /* +0x20 second per-definition id          */
    char pad_0024[0x18];        /* +0x24 .. +0x38                          */
    unsigned int type;          /* +0x3C EUIComponentType                  */

    EUIComponentType GetType() const { return (EUIComponentType)type; }

    /* The definition-manager global at 0x013B879C reads NULL from an injected
     * thread, so this -- present on every definition -- is the reliable
     * handle to the manager. */
    void* GetManager() const { return manager; }
};

static_assert(sizeof(void*) == 4, "Fable is a 32-bit target");
