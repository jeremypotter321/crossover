#pragma once

#include <iostream>

#include <MinHook/include/MinHook.h>

#include "BaseClass.h"
#include "CharString.h"
#include "DefString.h"
#include "DefStringTable.h"
#include "DefinitionManager.h"
#include "3DVector.h"
#include "Game.h"
#include "MainGameComponent.h"
#include "GamePlayerInterface.h"
#include "World.h"
#include "WorldMap.h"
#include "PlayerManager.h"
#include "Player.h"
#include "Thing.h"
#include "ThingPhysical.h"
#include "ThingGameObject.h"
#include "ThingCreatureBase.h"
#include "ThingPlayerCreature.h"
#include "ThingPlayerCreatureInit.h"
#include "CoopSpirit.h"
#include "GameEvent.h"
#include "PhysicsBase.h"
#include "PhysicsControlled.h"
#include "PhysicsStandard.h"
#include "RightHandedSet.h"

#include "../NUISystem/UITypes.h"
#include "../NUISystem/CUIDef.h"
#include "../NUISystem/CUIFactory.h"

class SDK
{
public:
	SDK();
	static SDK& GetInstance();
	static void* GameMalloc(unsigned int size);
	static C3DVector* GOverridePlayerStartPos;

private:
	static void* (__cdecl* OGameMalloc)(unsigned int size);
	static void* __cdecl HGameMalloc(unsigned int size);
};
