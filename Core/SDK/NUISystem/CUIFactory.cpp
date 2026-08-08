#include "CUIFactory.h"

void* (__thiscall* CUIFactory::OCreateComponent)(void*, unsigned int) = nullptr;
CUIFactory::OnComponentCreated CUIFactory::s_observer = nullptr;

void* __fastcall CUIFactory::HCreateComponent(void* _this, void* _EDX, unsigned int defId)
{
    void* component = OCreateComponent(_this, defId);

    if (s_observer && component)
        s_observer(_this, defId, component);

    return component;
}

void* CUIFactory::CreateComponent(void* container, unsigned int defId)
{
    if (!OCreateComponent || !container || !defId)
        return nullptr;

    return OCreateComponent(container, defId);
}

void CUIFactory::SetObserver(OnComponentCreated callback)
{
    s_observer = callback;
}

void CUIFactory::Hook()
{
    ADD_HOOK(0x0041D21B, HCreateComponent, OCreateComponent);
}
