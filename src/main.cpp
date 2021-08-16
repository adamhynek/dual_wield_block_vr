#include "common/IDebugLog.h"  // IDebugLog
#include "skse64_common/skse_version.h"  // RUNTIME_VERSION
#include "skse64/PluginAPI.h"  // SKSEInterface, PluginInfo
#include "skse64/GameRTTI.h"
#include "skse64/GameSettings.h"

#include <ShlObj.h>  // CSIDL_MYDOCUMENTS

#include "main.h"
#include "version.h"  // VERSION_VERSTRING, VERSION_MAJOR
#include "config.h"
#include "MathUtils.h"

// Headers under api/ folder
#include "api/PapyrusVRAPI.h"
#include "api/VRManagerAPI.h"
#include "api/utils/OpenVRUtils.h"


namespace PapyrusVR
{

	static PluginHandle	g_pluginHandle = kPluginHandle_Invalid;
	static SKSEMessagingInterface *g_messaging = nullptr;

	PapyrusVRAPI *g_papyrusvr;
	PapyrusVR::VRManagerAPI *g_papyrusvrManager;
	OpenVRHookManagerAPI *g_openvrHook;

	TESObjectREFR *playerRef;
	VMClassRegistry *vmRegistry;

	// Config parameters for blocking conditions
	float maxSpeedEnter = 2;
	float maxSpeedExit = 3;
	float handForwardHmdUpEnter = -0.6;
	float handForwardHmdUpExit = -0.6;
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

	bool isLoaded = false;
	bool isLeftHanded = false;
	bool isLastUpdateValid = false;

	const int numPrevIsBlocking = 5; // Should be an odd number
	bool prevIsBlockings[numPrevIsBlocking];  // previous n IsBlocking animation values

	const int numPrevSpeeds = 2; // length of previous kept speeds
	float leftSpeeds[numPrevSpeeds]; // previous n speeds
	float rightSpeeds[numPrevSpeeds]; // previous n speeds

	const int blockCooldown = 30; // number of updates to ignore block start / stop
	int lastBlockStartFrameCount = 0; // number of updates we should still wait before attempting to start blocking
	int lastBlockStopFrameCount = 0; // number of updates we should still wait before attempting to stop blocking

	extern "C" {

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
			bool isBlocking = GetAnimationVariableBool(vmRegistry, 0, playerRef, BSFixedString("IsBlocking"));

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
		int GetHandBlockingStatus(TrackedDevicePose *hmdPose, TrackedDevicePose *handPose, float *speeds, bool isBlocking)
		{
			Quaternion hmdQuat = OpenVRUtils::GetRotation(&(hmdPose->mDeviceToAbsoluteTracking));
			Vector3 hmdPosition = OpenVRUtils::GetPosition(&(hmdPose->mDeviceToAbsoluteTracking));
			Matrix34 hmdMatrix = OpenVRUtils::CreateRotationMatrix(&hmdQuat);

			Quaternion handQuat = OpenVRUtils::GetRotation(&(handPose->mDeviceToAbsoluteTracking));
			Vector3 handPosition = OpenVRUtils::GetPosition(&(handPose->mDeviceToAbsoluteTracking));
			Matrix34 handMatrix = OpenVRUtils::CreateRotationMatrix(&handQuat);

			Vector3 handForward(handMatrix.m[0][2], handMatrix.m[1][2], handMatrix.m[2][2]); // third column of hand matrix - the direction the sword points in
			Vector3 hmdForward(hmdMatrix.m[0][2], hmdMatrix.m[1][2], hmdMatrix.m[2][2]); // third column of hmd matrix
			Vector3 hmdUp(hmdMatrix.m[0][1], hmdMatrix.m[1][1], hmdMatrix.m[2][1]); // second column of hmd matrix
			float handForwardDotWithHmdUp = MathUtils::VectorDotProduct(handForward, hmdUp); // > 0 means the sword is pointing down
			float handForwardDotWithHmdForward = MathUtils::VectorDotProduct(handForward, hmdForward); // close to 0 means the sword is not pointing towards or away from the hmd

			Vector3 hmdToHand = handPosition - hmdPosition; // Vector pointing from the hmd to the hand
			float hmdToHandVerticalDistance = MathUtils::VectorDotProduct(hmdUp, hmdToHand); // Distance of hand away from hmd along hmd up axis

			// Compute max speed over the last few
			float speed = handPose->vVelocity.lengthSquared();
			float maxSpeed = speed;
			for (int i = numPrevSpeeds - 1; i >= 1; i--) {
				speeds[i] = speeds[i - 1];
				if (speeds[i] > maxSpeed) maxSpeed = speeds[i];
			}
			speeds[0] = speed;

			if (maxSpeed <= maxSpeedEnter &&
				handForwardDotWithHmdUp >= handForwardHmdUpEnter &&
				abs(handForwardDotWithHmdForward) <= handForwardHmdForwardEnter &&
				abs(hmdToHandVerticalDistance) <= hmdToHandDistanceUpEnter) {

				if (!isBlocking) {
					// Start blocking
					return 2;
				}
			}
			else if (maxSpeed > maxSpeedExit ||
				handForwardDotWithHmdUp < handForwardHmdUpExit ||
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
		int GetHandBlockingStatusUnarmed(TrackedDevicePose *hmdPose, TrackedDevicePose *handPose, float *speeds, bool isBlocking)
		{
			Quaternion hmdQuat = OpenVRUtils::GetRotation(&(hmdPose->mDeviceToAbsoluteTracking));
			Vector3 hmdPosition = OpenVRUtils::GetPosition(&(hmdPose->mDeviceToAbsoluteTracking));
			Matrix34 hmdMatrix = OpenVRUtils::CreateRotationMatrix(&hmdQuat);

			Quaternion handQuat = OpenVRUtils::GetRotation(&(handPose->mDeviceToAbsoluteTracking));
			Vector3 handPosition = OpenVRUtils::GetPosition(&(handPose->mDeviceToAbsoluteTracking));
			Matrix34 handMatrix = OpenVRUtils::CreateRotationMatrix(&handQuat);

			Vector3 handForward(handMatrix.m[0][2], handMatrix.m[1][2], handMatrix.m[2][2]); // third column of hand matrix - the direction the sword points in
			Vector3 hmdUp(hmdMatrix.m[0][1], hmdMatrix.m[1][1], hmdMatrix.m[2][1]); // second column of hmd matrix
			Vector3 hmdRight(hmdMatrix.m[0][0], hmdMatrix.m[1][0], hmdMatrix.m[2][0]); // first column of hmd matrix
			float handForwardDotWithHmdRight = MathUtils::VectorDotProduct(handForward, hmdRight);

			Vector3 hmdToHand = handPosition - hmdPosition; // Vector pointing from the hmd to the hand
			float hmdToHandVerticalDistance = MathUtils::VectorDotProduct(hmdUp, hmdToHand); // Distance of hand away from hmd along hmd up axis

			// Compute max speed over the last few
			float speed = handPose->vVelocity.lengthSquared();
			float maxSpeed = speed;
			for (int i = numPrevSpeeds - 1; i >= 1; i--) {
				speeds[i] = speeds[i - 1];
				if (speeds[i] > maxSpeed) maxSpeed = speeds[i];
			}
			speeds[0] = speed;

			if (maxSpeed <= maxSpeedUnarmedEnter &&
				abs(handForwardDotWithHmdRight) >= handForwardHmdRightUnarmedEnter &&
				abs(hmdToHandVerticalDistance) <= hmdToHandDistanceUpUnarmedEnter) {

				if (!isBlocking) {
					// Start blocking
					return 2;
				}
			}
			else if (maxSpeed > maxSpeedUnarmedExit ||
				abs(handForwardDotWithHmdRight) < handForwardHmdRightUnarmedExit ||
				abs(hmdToHandVerticalDistance) > hmdToHandDistanceUpUnarmedExit) {

				if (isBlocking) {
					// Stop blocking
					return 1;
				}
			}
			return 0;
		}

		bool FillDevicePoses(TrackedDevicePose *&hmdPose, TrackedDevicePose *&mainHandPose, TrackedDevicePose *&offHandPose)
		{
			vr::IVRSystem *vrSystem = g_openvrHook->GetVRSystem();
			if (!vrSystem) return false;

			if (!vrSystem->IsTrackedDeviceConnected(vr::k_unTrackedDeviceIndex_Hmd)) return false;
			if (!vrSystem->IsTrackedDeviceConnected(vrSystem->GetTrackedDeviceIndexForControllerRole(vr::ETrackedControllerRole::TrackedControllerRole_RightHand))) return false;
			if (!vrSystem->IsTrackedDeviceConnected(vrSystem->GetTrackedDeviceIndexForControllerRole(vr::ETrackedControllerRole::TrackedControllerRole_LeftHand))) return false;

			hmdPose = g_papyrusvrManager->GetHMDPose();
			mainHandPose = isLeftHanded ? g_papyrusvrManager->GetLeftHandPose() : g_papyrusvrManager->GetRightHandPose();
			offHandPose = isLeftHanded ? g_papyrusvrManager->GetRightHandPose() : g_papyrusvrManager->GetLeftHandPose();
			if (!hmdPose || !mainHandPose || !offHandPose ) return false;
			if (!hmdPose->bDeviceIsConnected || hmdPose->eTrackingResult != ETrackingResult::TrackingResult_Running_OK || !hmdPose->bPoseIsValid) return false;
			if (!mainHandPose->bDeviceIsConnected || mainHandPose->eTrackingResult != ETrackingResult::TrackingResult_Running_OK || !mainHandPose->bPoseIsValid) return false;
			if (!offHandPose->bDeviceIsConnected || offHandPose->eTrackingResult != ETrackingResult::TrackingResult_Running_OK || !offHandPose->bPoseIsValid) return false;

			return true;
		}

		// Called on each update (about 90-100 calls per second)
		void OnPoseUpdate(float DeltaTime)
		{
			if (lastBlockStartFrameCount > 0)
				lastBlockStartFrameCount--;
			if (lastBlockStopFrameCount > 0)
				lastBlockStopFrameCount--;

			if (!isLoaded || !g_thePlayer) return;

			if (IsInMenuMode(vmRegistry, 0)) return;

			PlayerCharacter *pc = *g_thePlayer;
			if (!pc || !pc->actorState.IsWeaponDrawn()) return;

			bool wasLastUpdateValid = isLastUpdateValid;
			isLastUpdateValid = false;

			TESForm *mainHandItem = GetMainHandObject(pc);
			TESForm *offHandItem = GetOffHandObject(pc);
			if(!IsDualWielding(mainHandItem, offHandItem)) {
				// If we switched weapons away from dual wielding, cancel existing block state
				if (wasLastUpdateValid) {
					DebugSendAnimationEvent(vmRegistry, 0, nullptr, playerRef, BSFixedString("blockStop"));
					_MESSAGE("Stop block");
				}
				return;
			}

			TrackedDevicePose *hmdPose, *mainHandPose, *offHandPose;
			if (!FillDevicePoses(hmdPose, mainHandPose, offHandPose)) return;

			// Check if the player is blocking
			bool isBlocking = GetIsBlockingMode();

			int mainHandBlockStatus = 0;
			if (!mainHandItem) { // Unarmed
				mainHandBlockStatus = GetHandBlockingStatusUnarmed(hmdPose, mainHandPose, rightSpeeds, isBlocking);
			}
			else { // Weapon
				mainHandBlockStatus = GetHandBlockingStatus(hmdPose, mainHandPose, rightSpeeds, isBlocking);
			}

			int offHandBlockStatus = 0;
			if (!offHandItem) { // Unarmed
				offHandBlockStatus = GetHandBlockingStatusUnarmed(hmdPose, offHandPose, leftSpeeds, isBlocking);
			}
			else if (offHandItem->IsWeapon() || offHandItem->formType == kFormType_Light) { // Weapon / torch are the same case
				offHandBlockStatus = GetHandBlockingStatus(hmdPose, offHandPose, leftSpeeds, isBlocking);
			}
			else if (offHandItem->formType == kFormType_Armor) { // Offhand is a shield
				if (IsBlockingInternal(pc)) {
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
					DebugSendAnimationEvent(vmRegistry, 0, nullptr, playerRef, BSFixedString("blockStart"));
					_MESSAGE("Start block");
					lastBlockStartFrameCount = blockCooldown;
				}
			}
			else if (mainHandBlockStatus == 1 && offHandBlockStatus == 1) { // Both hands are not blocking
				if (lastBlockStopFrameCount <= 0) { // Do not try to block more than once every n updates
					// Stop blocking
					DebugSendAnimationEvent(vmRegistry, 0, nullptr, playerRef, BSFixedString("blockStop"));
					_MESSAGE("Stop block");
					lastBlockStopFrameCount = blockCooldown;
				}
			}
			isLastUpdateValid = true;
		}

		// Listener for PapyrusVR Messages
		void OnPapyrusVRMessage(SKSEMessagingInterface::Message* msg)
		{
			if (msg) {
				if (msg->type == kPapyrusVR_Message_Init && msg->data) {
					_MESSAGE("PapyrusVR Init Message recived with valid data, registering for pose update callback");
					g_papyrusvr = (PapyrusVRAPI*)msg->data;
					g_papyrusvrManager = g_papyrusvr->GetVRManager();
					g_openvrHook = g_papyrusvr->GetOpenVRHook();
					if (!g_papyrusvrManager) {
						_MESSAGE("Could not get PapyrusVRManager");
						return;
					}
					if (!g_openvrHook) {
						_MESSAGE("Could not get OpenVRHook");
						return;
					}
					// Registers for PoseUpdates
					g_papyrusvrManager->RegisterVRUpdateListener(OnPoseUpdate);
				}
			}
		}

		// Listener for SKSE Messages
		void OnSKSEMessage(SKSEMessagingInterface::Message* msg)
		{
			if (msg) {
				if (msg->type == SKSEMessagingInterface::kMessage_PostLoad) {
					_MESSAGE("SKSE PostLoad recived, registering for SkyrimVRTools messages");
					g_messaging->RegisterListener(g_pluginHandle, "SkyrimVRTools", OnPapyrusVRMessage);
				}
				else if (msg->type == SKSEMessagingInterface::kMessage_PreLoadGame) {
					_MESSAGE("SKSE PreLoadGame message received");
					isLoaded = false;
				}
				else if (msg->type == SKSEMessagingInterface::kMessage_PostLoadGame || msg->type == SKSEMessagingInterface::kMessage_NewGame) {
					_MESSAGE("SKSE PostLoadGame or NewGame message received, type: %d", msg->type);
					isLoaded = true;
					Setting	* isLeftHandedSetting = GetINISetting("bLeftHandedMode:VRInput");
					isLeftHanded = (bool)isLeftHandedSetting->data.u8;
					playerRef = DYNAMIC_CAST(LookupFormByID(0x14), TESForm, TESObjectREFR);
					vmRegistry = (*g_skyrimVM)->GetClassRegistry();
				}
			}
		}

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

		bool ReadConfigOptions()
		{
			// Dual wield settings
			if (!DualWieldBlockVR::GetConfigOptionFloat("DualWield", "MaxSpeedEnter", &maxSpeedEnter)) return false;
			if (!DualWieldBlockVR::GetConfigOptionFloat("DualWield", "MaxSpeedExit", &maxSpeedExit)) return false;

			if (!DualWieldBlockVR::GetConfigOptionFloat("DualWield", "HandForwardDotWithHmdDownEnter", &handForwardHmdUpEnter)) return false;
			if (!DualWieldBlockVR::GetConfigOptionFloat("DualWield", "HandForwardDotWithHmdDownExit", &handForwardHmdUpExit)) return false;

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

		bool SKSEPlugin_Load(const SKSEInterface * skse)
		{	// Called by SKSE to load this plugin
			_MESSAGE("DualWieldBlockVR loaded");

			// Registers for SKSE Messages (PapyrusVR probably still need to load, wait for SKSE message PostLoad)
			_MESSAGE("Registering for SKSE messages");
			g_messaging = (SKSEMessagingInterface*)skse->QueryInterface(kInterface_Messaging);
			g_messaging->RegisterListener(g_pluginHandle, "SKSE", OnSKSEMessage);

			if (ReadConfigOptions()) {
				_MESSAGE("Successfully read config parameters");
			}
			else {
				_WARNING("[WARNING] Failed to read config options. Using defaults instead.");
			}

			for (int i = 0; i < numPrevIsBlocking; i++) {
				prevIsBlockings[i] = false;
			}

			for (int i = 0; i < numPrevSpeeds; i++) {
				leftSpeeds[i] = 0;
				rightSpeeds[i] = 0;
			}

			// wait for PapyrusVR init (during PostPostLoad SKSE Message)

			return true;
		}
	};

}