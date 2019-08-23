#pragma once

#include "skse64_common/Relocation.h"
#include "skse64/PapyrusVM.h"
#include "skse64/GameReferences.h"


typedef bool(*_GetAnimationVariableBool)(VMClassRegistry* registry, UInt32 stackId, TESObjectREFR* obj, BSFixedString *asVariableName);
RelocAddr<_GetAnimationVariableBool> GetAnimationVariableBool(0x009CE880);

typedef void(*_DebugSendAnimationEvent)(VMClassRegistry* registry, UInt32 stackId, void* unk1, TESObjectREFR* objectRefr, BSFixedString const &animEvent);
RelocAddr<_DebugSendAnimationEvent> DebugSendAnimationEvent(0x009A7F40);
