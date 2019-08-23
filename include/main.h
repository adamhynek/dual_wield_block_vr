#pragma once

#include "skse64_common/Relocation.h"
#include "skse64/PapyrusVM.h"
#include "skse64/GameReferences.h"


typedef bool(*_GetAnimationVariableBool)(VMClassRegistry* registry, UInt32 stackId, TESObjectREFR* obj, BSFixedString *asVariableName);
extern RelocAddr<_GetAnimationVariableBool> GetAnimationVariableBool;
