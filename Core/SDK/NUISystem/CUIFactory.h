#pragma once

#include "../Utils/Hook.h"
#include "CUIDef.h"

/*
 * The UI component factory.
 *
 * A single function at 0x0041D21B builds every UI component in the game. It
 * reads the type from the definition at +0x3C, indexes a jump table, allocates
 * the per-type size with the game allocator (0x00BFEA1A -- already hooked by
 * the SDK) and calls that type's constructor.
 *
 * Hooking it gives two things at once: a trampoline for creating natively
 * constructed components ourselves, and a place to observe or extend a screen
 * while it is being built -- which is the only point at which components can be
 * added, since the drawn set is fixed at construction.
 */
class CUIFactory
{
public:
    /* Build a component from a definition, exactly as the game does. */
    static void* CreateComponent(void* owner, CUIDef* def);

    /* Invoked for every component the game builds, after construction.
     * Returning is enough; the component is already attached by the caller. */
    typedef void (*OnComponentCreated)(void* owner, CUIDef* def, void* component);
    static void SetObserver(OnComponentCreated callback);

    static void Hook();

private:
    static void* (__thiscall* OCreateComponent)(void*, CUIDef*);
    static void* __fastcall HCreateComponent(void* _this, void* _EDX, CUIDef* def);

    static OnComponentCreated s_observer;
};
