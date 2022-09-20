#pragma once

#include "skse64_common/Relocation.h"
#include "skse64/PapyrusVM.h"
#include "skse64/GameReferences.h"


typedef bool(*_IsInMenuMode)(VMClassRegistry* registry, UInt32 stackId);
RelocAddr<_IsInMenuMode> IsInMenuMode(0x009F32A0);

typedef bool(*_IAnimationGraphManagerHolder_NotifyAnimationGraph)(IAnimationGraphManagerHolder *_this, const BSFixedString &a_eventName); // 01
typedef bool(*_IAnimationGraphManagerHolder_GetAnimationVariableInt)(IAnimationGraphManagerHolder *_this, const BSFixedString &a_variableName, SInt32 &a_out); // 11
typedef bool(*_IAnimationGraphManagerHolder_GetAnimationVariableBool)(IAnimationGraphManagerHolder *_this, const BSFixedString &a_variableName, bool &a_out); // 12


inline UInt64 *get_vtbl(void *object) { return *((UInt64 **)object); }

template<typename T>
inline T get_vfunc(void *object, UInt64 index) {
	UInt64 *vtbl = get_vtbl(object);
	return (T)(vtbl[index]);
}
