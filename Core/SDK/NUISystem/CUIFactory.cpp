#include "CUIFactory.h"

void* (__thiscall* CUIFactory::OCreateComponent)(void*, CUIDef*) = nullptr;
CUIFactory::OnComponentCreated CUIFactory::s_observer = nullptr;

void* __fastcall CUIFactory::HCreateComponent(void* _this, void* _EDX, CUIDef* def)
{
    void* component = OCreateComponent(_this, def);

    if (s_observer && component)
        s_observer(_this, def, component);

    return component;
}

void* CUIFactory::CreateComponent(void* owner, CUIDef* def)
{
    if (!OCreateComponent || !def)
        return nullptr;

    return OCreateComponent(owner, def);
}

void CUIFactory::SetObserver(OnComponentCreated callback)
{
    s_observer = callback;
}

void CUIFactory::Hook()
{
    ADD_HOOK(0x0041D21B, HCreateComponent, OCreateComponent);
}
