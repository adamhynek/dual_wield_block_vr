#include "common/IDebugLog.h"  // IDebugLog
#include "skse64_common/skse_version.h"  // RUNTIME_VERSION
#include "skse64/PluginAPI.h"  // SKSEInterface, PluginInfo
#include "skse64/GameRTTI.h"
#include "skse64/GameSettings.h"
#include "skse64/GameInput.h"
#include "skse64/GameVR.h"

#include <ShlObj.h>  // CSIDL_MYDOCUMENTS

#include "main.h"
#include "version.h"  // VERSION_VERSTRING, VERSION_MAJOR
#include "config.h"
#include "math_utils.h"


RelocPtr<float> g_havokWorldScale(0x15B78F4);
RelocPtr<float> g_fMeleeLinearVelocityThreshold_Blocking(0x1EAE518);
float g_vanillaBlockingVelocityOverride = 0.4f; // 0.4f is the game's default

PluginHandle g_pluginHandle = kPluginHandle_Invalid;
SKSEMessagingInterface *g_messaging = nullptr;
SKSEVRInterface *g_vrInterface = nullptr;

// Config parameters for blocking conditions
float maxSpeedEnter = 2;
float maxSpeedExit = 3;
float handForwardHmdDownEnter = -0.6;
float handForwardHmdDownExit = -0.6;
float handForwardHmdForwardEnter = 0.4;
float handForwardHmdForwardExit = 0.6;
float hmdToHandDistanceUpEnter = 0.35;
float hmdToHandDistanceUpExit = 0.35;
bool isShieldEnabled = false;

// Config parameters for unarmed blocking
float maxSpeedUnarmedEnter = 1;
float maxSpeedUnarmedExit = 1.5;
float handForwardHmdRightUnarmedEnter = 0.5;
float handForwardHmdRightUnarmedExit = 0.4;
float hmdToHandDistanceUpUnarmedEnter = 0.35;
float hmdToHandDistanceUpUnarmedExit = 0.35;

bool isLastUpdateValid = false;

const int numPrevIsBlocking = 5; // Should be an odd number
bool prevIsBlockings[numPrevIsBlocking];  // previous n IsBlocking animation values

const int blockCooldown = 30; // number of updates to ignore block start / stop
int lastBlockStartFrameCount = 0; // number of updates we should still wait before attempting to start blocking
int lastBlockStopFrameCount = 0; // number of updates we should still wait before attempting to stop blocking


TESForm * GetMainHandObject(Actor *actor)
{
	return actor->GetEquippedObject(false);
}

TESForm * GetOffHandObject(Actor *actor)
{
	return actor->GetEquippedObject(true);
}

bool IsTwoHanded(const TESObjectWEAP *weap)
{
	switch (weap->gameData.type) {
	case TESObjectWEAP::GameData::kType_CrossBow:
	case TESObjectWEAP::GameData::kType_TwoHandAxe:
	case TESObjectWEAP::GameData::kType_TwoHandSword:
		return true;
	default:
		return false;
	}
}

bool IsDualWielding(TESForm *mainHandItem, TESForm *offHandItem)
{
	// Unarmed is okay
	if (!mainHandItem && !offHandItem) return true;

	// If not unarmed, both hands need to have something
	if (!mainHandItem || !offHandItem) return false;

	// main hand has to be a weapon, and not two-handed
	TESObjectWEAP *mainWeapon = DYNAMIC_CAST(mainHandItem, TESForm, TESObjectWEAP);
	if (!mainWeapon || IsTwoHanded(mainWeapon)) return false;

	// offhand can be weapon or spell or torch, or shield if enabled
	if (!(offHandItem->IsWeapon() || offHandItem->formType == kFormType_Spell || offHandItem->formType == kFormType_Light
		|| (isShieldEnabled && offHandItem->formType == kFormType_Armor))) return false;

	return true;
}

// True if _the game_ decided to block, not us (i.e. 1 handed exclusive block, 2 handed block, or shield is blocking)
bool IsBlockingInternal(Actor *actor)
{
	return (actor->actorState.flags08 >> 8) & 1;
}

// Get the mode of the IsBlocking value over the last few frames. This is needed because when you block a hit, it goes to 0 for 1 frame, then back to 1.
bool GetIsBlockingMode()
{
	PlayerCharacter *player = *g_thePlayer;
	static BSFixedString s_IsBlocking("IsBlocking");
	bool isBlocking; get_vfunc<_IAnimationGraphManagerHolder_GetAnimationVariableBool>(&player->animGraphHolder, 0x12)(&player->animGraphHolder, s_IsBlocking, isBlocking);

	int numTrue = (int)isBlocking;
	int numFalse = (int)(!isBlocking);
	for (int i = numPrevIsBlocking - 1; i >= 1; i--) {
		prevIsBlockings[i] = prevIsBlockings[i - 1];
		if (prevIsBlockings[i]) numTrue++;
		else numFalse++;
	}
	prevIsBlockings[0] = isBlocking;
	return (numTrue >= numFalse);
}

// Returns 2 if we should start blocking, 1 if we should stop blocking, 0 if no effect
int GetHandBlockingStatus(NiTransform &hmdPose, NiTransform &handPose, float handSpeed, bool isBlocking)
{
	NiPoint3 hmdPosition = hmdPose.pos;
	NiMatrix33 &hmdRot = hmdPose.rot;

	NiPoint3 handPosition = handPose.pos;
	NiMatrix33 &handRot = handPose.rot;

	NiPoint3 handForward = ForwardVector(handRot);
	NiPoint3 hmdForward = ForwardVector(hmdRot);
	NiPoint3 hmdDown = -UpVector(hmdRot);

	float handForwardDotWithHmdDown = DotProduct(handForward, hmdDown); // > 0 means the sword is pointing down
	float handForwardDotWithHmdForward = DotProduct(handForward, hmdForward); // close to 0 means the sword is not pointing towards or away from the hmd

	NiPoint3 hmdToHand = (handPosition - hmdPosition) * *g_havokWorldScale; // Vector pointing from the hmd to the hand, in meters
	float hmdToHandVerticalDistance = DotProduct(hmdDown, hmdToHand); // Distance of hand away from hmd along hmd up axis

	if (handSpeed <= maxSpeedEnter &&
		handForwardDotWithHmdDown >= handForwardHmdDownEnter &&
		abs(handForwardDotWithHmdForward) <= handForwardHmdForwardEnter &&
		abs(hmdToHandVerticalDistance) <= hmdToHandDistanceUpEnter) {

		if (!isBlocking) {
			// Start blocking
			return 2;
		}
	}
	else if (handSpeed > maxSpeedExit ||
		handForwardDotWithHmdDown < handForwardHmdDownExit ||
		abs(handForwardDotWithHmdForward) > handForwardHmdForwardExit ||
		abs(hmdToHandVerticalDistance) > hmdToHandDistanceUpExit) {

		if (isBlocking) {
			// Stop blocking
			return 1;
		}
	}
	return 0;
}

// Returns 2 if we should start blocking, 1 if we should stop blocking, 0 if no effect
int GetHandBlockingStatusUnarmed(NiTransform &hmdPose, NiTransform &handPose, float handSpeed, bool isBlocking, bool isLeft)
{
	NiPoint3 hmdPosition = hmdPose.pos;
	NiMatrix33 &hmdRot = hmdPose.rot;

	NiPoint3 handPosition = handPose.pos;
	NiMatrix33 &handRot = handPose.rot;

	NiPoint3 handForward = ForwardVector(handRot);
	NiPoint3 hmdUp = UpVector(hmdRot);
	NiPoint3 hmdRight = RightVector(hmdRot);

	float handForwardDotWithHmdOutwards = DotProduct(handForward, hmdRight);
	if (isLeft) handForwardDotWithHmdOutwards *= -1.f;

	NiPoint3 hmdToHand = (handPosition - hmdPosition) * *g_havokWorldScale; // Vector pointing from the hmd to the hand, in meters
	float hmdToHandVerticalDistance = DotProduct(hmdUp, hmdToHand); // Distance of hand away from hmd along hmd up axis

	if (handSpeed <= maxSpeedUnarmedEnter &&
		handForwardDotWithHmdOutwards >= handForwardHmdRightUnarmedEnter &&
		abs(hmdToHandVerticalDistance) <= hmdToHandDistanceUpUnarmedEnter) {

		if (!isBlocking) {
			// Start blocking
			return 2;
		}
	}
	else if (handSpeed > maxSpeedUnarmedExit ||
		handForwardDotWithHmdOutwards < handForwardHmdRightUnarmedExit ||
		abs(hmdToHandVerticalDistance) > hmdToHandDistanceUpUnarmedExit) {

		if (isBlocking) {
			// Stop blocking
			return 1;
		}
	}
	return 0;
}

float g_rightHandSpeed = 0.f;
float g_leftHandSpeed = 0.f;

void UpdateHandSpeeds(vr_src::TrackedDevicePose_t *pGamePoseArray, uint32_t unGamePoseArrayCount)
{
	if (!g_openVR) return;

	BSOpenVR *openVR = *g_openVR;
	if (!openVR) return;

	vr_src::IVRSystem *vrSystem = openVR->vrSystem;
	if (!vrSystem) return;

	const vr_src::TrackedDeviceIndex_t hmdIndex = vr_src::k_unTrackedDeviceIndex_Hmd;
	const vr_src::TrackedDeviceIndex_t rightIndex = vrSystem->GetTrackedDeviceIndexForControllerRole(vr_src::ETrackedControllerRole::TrackedControllerRole_RightHand);
	const vr_src::TrackedDeviceIndex_t leftIndex = vrSystem->GetTrackedDeviceIndexForControllerRole(vr_src::ETrackedControllerRole::TrackedControllerRole_LeftHand);

	if (unGamePoseArrayCount <= hmdIndex || !vrSystem->IsTrackedDeviceConnected(hmdIndex)) return;

	vr_src::TrackedDevicePose_t &hmdPose = pGamePoseArray[hmdIndex];
	if (!hmdPose.bDeviceIsConnected || !hmdPose.bPoseIsValid || hmdPose.eTrackingResult != vr_src::ETrackingResult::TrackingResult_Running_OK) return;

	bool isRightConnected = vrSystem->IsTrackedDeviceConnected(rightIndex);
	bool isLeftConnected = vrSystem->IsTrackedDeviceConnected(leftIndex);

	for (int i = hmdIndex + 1; i < unGamePoseArrayCount; i++) {
		if (i == rightIndex && isRightConnected) {
			vr_src::TrackedDevicePose_t &pose = pGamePoseArray[i];
			if (pose.bDeviceIsConnected && pose.bPoseIsValid && pose.eTrackingResult == vr_src::ETrackingResult::TrackingResult_Running_OK) {
				NiPoint3 openvrVelocity = { pose.vVelocity.v[0], pose.vVelocity.v[1], pose.vVelocity.v[2] };
				g_rightHandSpeed = VectorLengthSquared(openvrVelocity);
			}
		}
		else if (i == leftIndex && isLeftConnected) {
			vr_src::TrackedDevicePose_t &pose = pGamePoseArray[i];
			if (pose.bDeviceIsConnected && pose.bPoseIsValid && pose.eTrackingResult == vr_src::ETrackingResult::TrackingResult_Running_OK) {
				NiPoint3 openvrVelocity = { pose.vVelocity.v[0], pose.vVelocity.v[1], pose.vVelocity.v[2] };
				g_leftHandSpeed = VectorLengthSquared(openvrVelocity);
			}
		}
	}
}

void StartBlocking(Actor *actor)
{
	static BSFixedString s_blockStart("blockStart");
	get_vfunc<_IAnimationGraphManagerHolder_NotifyAnimationGraph>(&actor->animGraphHolder, 0x1)(&actor->animGraphHolder, s_blockStart);
	_MESSAGE("Start block");
}

void StopBlocking(Actor *actor)
{
	static BSFixedString s_blockStop("blockStop");
	get_vfunc<_IAnimationGraphManagerHolder_NotifyAnimationGraph>(&actor->animGraphHolder, 0x1)(&actor->animGraphHolder, s_blockStop);
	_MESSAGE("Stop block");
}

bool WaitPosesCB(vr_src::TrackedDevicePose_t *pRenderPoseArray, uint32_t unRenderPoseArrayCount, vr_src::TrackedDevicePose_t *pGamePoseArray, uint32_t unGamePoseArrayCount)
{
	if (lastBlockStartFrameCount > 0)
		lastBlockStartFrameCount--;
	if (lastBlockStopFrameCount > 0)
		lastBlockStopFrameCount--;

	PlayerCharacter *player = *g_thePlayer;
	if (!player || !player->GetNiNode() || !player->actorState.IsWeaponDrawn()) return true;

	if (IsInMenuMode(nullptr, 0)) return true;

	NiPointer<NiAVObject> hmdNode = player->unk3F0[PlayerCharacter::Node::kNode_HmdNode];
	if (!hmdNode) return true;

	NiPointer<NiAVObject> rightWand = player->unk3F0[PlayerCharacter::Node::kNode_RightWandNode];
	if (!rightWand) return true;

	NiPointer<NiAVObject> leftWand = player->unk3F0[PlayerCharacter::Node::kNode_LeftWandNode];
	if (!leftWand) return true;

	bool wasLastUpdateValid = isLastUpdateValid;
	isLastUpdateValid = false;

	TESForm *mainHandItem = GetMainHandObject(player);
	TESForm *offHandItem = GetOffHandObject(player);
	if(!IsDualWielding(mainHandItem, offHandItem)) {
		// If we switched weapons away from dual wielding, cancel existing block state
		if (wasLastUpdateValid) {
			StopBlocking(player);
		}
		return true;
	}

	UpdateHandSpeeds(pGamePoseArray, unGamePoseArrayCount);

	// Check if the player is blocking
	bool isBlocking = GetIsBlockingMode();

	bool isLeftHanded = *g_leftHandedMode;

	NiAVObject *mainWand = isLeftHanded ? leftWand : rightWand;
	NiAVObject *offhandWand = isLeftHanded ? rightWand : leftWand;
	float mainWandSpeed = isLeftHanded ? g_leftHandSpeed : g_rightHandSpeed;
	float offhandWandSpeed = isLeftHanded ? g_rightHandSpeed : g_leftHandSpeed;

	int mainHandBlockStatus = 0;
	if (!mainHandItem) { // Unarmed
		mainHandBlockStatus = GetHandBlockingStatusUnarmed(hmdNode->m_worldTransform, mainWand->m_worldTransform, mainWandSpeed, isBlocking, isLeftHanded);
	}
	else { // Weapon
		mainHandBlockStatus = GetHandBlockingStatus(hmdNode->m_worldTransform, mainWand->m_worldTransform, mainWandSpeed, isBlocking);
	}

	int offHandBlockStatus = 0;
	if (!offHandItem) { // Unarmed
		offHandBlockStatus = GetHandBlockingStatusUnarmed(hmdNode->m_worldTransform, offhandWand->m_worldTransform, offhandWandSpeed, isBlocking, !isLeftHanded);
	}
	else if (offHandItem->IsWeapon() || offHandItem->formType == kFormType_Light) { // Weapon / torch are the same case
		offHandBlockStatus = GetHandBlockingStatus(hmdNode->m_worldTransform, offhandWand->m_worldTransform, offhandWandSpeed, isBlocking);
	}
	else if (offHandItem->formType == kFormType_Armor) { // Offhand is a shield
		if (IsBlockingInternal(player)) {
			offHandBlockStatus = 0;
		}
		else {
			if (isBlocking) {
				offHandBlockStatus = 1;
			}
		}
	}
	else {
		offHandBlockStatus = 1; // Offhand not being a weapon/shield (so spell) means it should say to stop blocking
	}

	if (mainHandBlockStatus == 2 || offHandBlockStatus == 2) { // Either hand is in blocking position
		if (lastBlockStartFrameCount <= 0) { // Do not try to block more than once every n updates
			// Start blocking
			StartBlocking(player);
			lastBlockStartFrameCount = blockCooldown;
		}
	}
	else if (mainHandBlockStatus == 1 && offHandBlockStatus == 1) { // Both hands are not blocking
		if (lastBlockStopFrameCount <= 0) { // Do not try to block more than once every n updates
			// Stop blocking
			StopBlocking(player);
			lastBlockStopFrameCount = blockCooldown;
		}
	}
	isLastUpdateValid = true;

	return true;
}


// Listener for SKSE Messages
void OnSKSEMessage(SKSEMessagingInterface::Message* msg)
{
	if (msg) {
		if (msg->type == SKSEMessagingInterface::kMessage_PreLoadGame) {
		}
		else if (msg->type == SKSEMessagingInterface::kMessage_PostLoadGame || msg->type == SKSEMessagingInterface::kMessage_NewGame) {
		}
		else if (msg->type == SKSEMessagingInterface::kMessage_DataLoaded) {
			*g_fMeleeLinearVelocityThreshold_Blocking = g_vanillaBlockingVelocityOverride;
		}
	}
}

void ShowErrorBox(const char *errorString)
{
	int msgboxID = MessageBox(
		NULL,
		(LPCTSTR)errorString,
		(LPCTSTR)"PLANCK Fatal Error",
		MB_ICONERROR | MB_OK | MB_TASKMODAL
	);
}

void ShowErrorBoxAndLog(const char *errorString)
{
	_ERROR(errorString);
	ShowErrorBox(errorString);
}

bool ReadConfigOptions()
{
	// Dual wield settings

	if (!DualWieldBlockVR::GetConfigOptionFloat("Settings", "VanillaBlockingVelocityOverride", &g_vanillaBlockingVelocityOverride)) return false;

	if (!DualWieldBlockVR::GetConfigOptionFloat("DualWield", "MaxSpeedEnter", &maxSpeedEnter)) return false;
	if (!DualWieldBlockVR::GetConfigOptionFloat("DualWield", "MaxSpeedExit", &maxSpeedExit)) return false;

	if (!DualWieldBlockVR::GetConfigOptionFloat("DualWield", "HandForwardDotWithHmdDownEnter", &handForwardHmdDownEnter)) return false;
	if (!DualWieldBlockVR::GetConfigOptionFloat("DualWield", "HandForwardDotWithHmdDownExit", &handForwardHmdDownExit)) return false;

	if (!DualWieldBlockVR::GetConfigOptionFloat("DualWield", "HandForwardDotWithHmdForwardEnter", &handForwardHmdForwardEnter)) return false;
	if (!DualWieldBlockVR::GetConfigOptionFloat("DualWield", "HandForwardDotWithHmdForwardExit", &handForwardHmdForwardExit)) return false;

	if (!DualWieldBlockVR::GetConfigOptionFloat("DualWield", "HmdToHandVerticalDistanceEnter", &hmdToHandDistanceUpEnter)) return false;
	if (!DualWieldBlockVR::GetConfigOptionFloat("DualWield", "HmdToHandVerticalDistanceExit", &hmdToHandDistanceUpExit)) return false;

	if (!DualWieldBlockVR::GetConfigOptionBool("DualWield", "EnableShield", &isShieldEnabled)) return false;

	// Unarmed settings
	if (!DualWieldBlockVR::GetConfigOptionFloat("Unarmed", "MaxSpeedEnter", &maxSpeedUnarmedEnter)) return false;
	if (!DualWieldBlockVR::GetConfigOptionFloat("Unarmed", "MaxSpeedExit", &maxSpeedUnarmedExit)) return false;

	if (!DualWieldBlockVR::GetConfigOptionFloat("Unarmed", "HandForwardDotWithHmdRightEnter", &handForwardHmdRightUnarmedEnter)) return false;
	if (!DualWieldBlockVR::GetConfigOptionFloat("Unarmed", "HandForwardDotWithHmdRightExit", &handForwardHmdRightUnarmedExit)) return false;

	if (!DualWieldBlockVR::GetConfigOptionFloat("Unarmed", "HmdToHandVerticalDistanceEnter", &hmdToHandDistanceUpUnarmedEnter)) return false;
	if (!DualWieldBlockVR::GetConfigOptionFloat("Unarmed", "HmdToHandVerticalDistanceExit", &hmdToHandDistanceUpUnarmedExit)) return false;

	return true;
}


extern "C" {
	bool SKSEPlugin_Query(const SKSEInterface* skse, PluginInfo* info)
	{
		gLog.OpenRelative(CSIDL_MYDOCUMENTS, "\\My Games\\Skyrim VR\\SKSE\\DualWieldBlockVR.log");
		gLog.SetPrintLevel(IDebugLog::kLevel_DebugMessage);
		gLog.SetLogLevel(IDebugLog::kLevel_DebugMessage);

		_MESSAGE("DualWieldBlockVR v%s", DWBVR_VERSION_VERSTRING);

		info->infoVersion = PluginInfo::kInfoVersion;
		info->name = "DualWieldBlockVR";
		info->version = DWBVR_VERSION_MAJOR;

		g_pluginHandle = skse->GetPluginHandle();

		if (skse->isEditor) {
			_FATALERROR("[FATAL ERROR] Loaded in editor, marking as incompatible!\n");
			return false;
		}
		else if (skse->runtimeVersion != RUNTIME_VR_VERSION_1_4_15) {
			_FATALERROR("[FATAL ERROR] Unsupported runtime version %08X!\n", skse->runtimeVersion);
			return false;
		}

		return true;
	}

	bool SKSEPlugin_Load(const SKSEInterface * skse)
	{	// Called by SKSE to load this plugin
		_MESSAGE("DualWieldBlockVR loaded");

		// Registers for SKSE Messages (PapyrusVR probably still need to load, wait for SKSE message PostLoad)
		_MESSAGE("Registering for SKSE messages");
		g_messaging = (SKSEMessagingInterface*)skse->QueryInterface(kInterface_Messaging);
		g_messaging->RegisterListener(g_pluginHandle, "SKSE", OnSKSEMessage);

		g_vrInterface = (SKSEVRInterface *)skse->QueryInterface(kInterface_VR);
		if (!g_vrInterface) {
			ShowErrorBoxAndLog("[CRITICAL] Couldn't get SKSE VR interface. You probably have an outdated SKSE version.");
			return false;
		}
		g_vrInterface->RegisterForPoses(g_pluginHandle, 11, WaitPosesCB);

		if (ReadConfigOptions()) {
			_MESSAGE("Successfully read config parameters");
		}
		else {
			_WARNING("[WARNING] Failed to read config options. Using defaults instead.");
		}

		for (int i = 0; i < numPrevIsBlocking; i++) {
			prevIsBlockings[i] = false;
		}

		// wait for PapyrusVR init (during PostPostLoad SKSE Message)

		return true;
	}
};
