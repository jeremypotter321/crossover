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
 *
 * The factory does NOT take a definition pointer. It takes a definition *id*
 * -- the value at CUIDef+0x20 -- and resolves the definition itself through the
 * manager. Verified live: calling it with a captured (container, id) pair
 * returned a component whose vtable was CFrontEndScreen's, for the definition
 * UI_FRONTEND_MEDIA_PLAYER_ERROR.
 */
class CUIFactory
{
public:
    /* Build a component, exactly as the game does.
     *   container -- an object holding a definition manager at +0x64; the
     *                screen or list the component is being built for.
     *   defId     -- the definition id, i.e. CUIDef::id2 (+0x20). */
    static void* CreateComponent(void* container, unsigned int defId);

    /* Invoked for every component the game builds, after construction.
     * Returning is enough; the component is already attached by the caller. */
    typedef void (*OnComponentCreated)(void* container, unsigned int defId, void* component);
    static void SetObserver(OnComponentCreated callback);

    static void Hook();

private:
    static void* (__thiscall* OCreateComponent)(void*, unsigned int);
    static void* __fastcall HCreateComponent(void* _this, void* _EDX, unsigned int defId);

    static OnComponentCreated s_observer;
};
