#pragma once

#include "../Utils/Hook.h"
#include "UITypes.h"
#include "CUIStateDef.h"

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
    unsigned int def_id;        /* +0x20 id the factory is called with     */
    char pad_0024[0x18];        /* +0x24 .. +0x38                          */
    unsigned int type;          /* +0x3C EUIComponentType                  */

    /* std::vector<CUIStateDef>: one record per visual state. This, not the
     * scalars above, is where a component's appearance is authored. */
    CUIStateDef* states_begin;  /* +0x40                                   */
    CUIStateDef* states_end;    /* +0x44                                   */
    CUIStateDef* states_cap;    /* +0x48 == states_end; exactly sized       */

    char pad_004C[0x0C];        /* +0x4C .. +0x54                          */
    float x;                    /* +0x58 position, seen 30.0 on a sprite    */
    float y;                    /* +0x5C position, seen 250.0 on a sprite   */

    EUIComponentType GetType() const { return (EUIComponentType)type; }

    /* The definition-manager global at 0x013B879C reads NULL from an injected
     * thread, so this -- present on every definition -- is the reliable
     * handle to the manager. */
    void* GetManager() const { return manager; }

    /* Pass this to CUIFactory::CreateComponent to build this component. */
    unsigned int GetDefId() const { return def_id; }

    unsigned int GetStateCount() const
    {
        if (!states_begin || !states_end) return 0;
        return (unsigned int)(states_end - states_begin);
    }

    CUIStateDef* GetState(unsigned int index)
    {
        return (index < GetStateCount()) ? &states_begin[index] : 0;
    }
};

static_assert(sizeof(void*) == 4, "Fable is a 32-bit target");
