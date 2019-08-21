#include "common/IDebugLog.h"  // IDebugLog
#include "skse64_common/skse_version.h"  // RUNTIME_VERSION
#include "skse64/PluginAPI.h"  // SKSEInterface, PluginInfo
#include "skse64/GameAPI.h"

#include <ShlObj.h>  // CSIDL_MYDOCUMENTS

#include "version.h"  // VERSION_VERSTRING, VERSION_MAJOR
#include "console.h"
#include "MathUtils.h"

// Headers under api/ folder
#include "api/PapyrusVRAPI.h"
#include "api/VRManagerAPI.h"
#include "api/utils/OpenVRUtils.h"


namespace PapyrusVR
{

	static PluginHandle	g_pluginHandle = kPluginHandle_Invalid;
	static SKSEMessagingInterface *g_messaging = nullptr;
	static SKSEPapyrusInterface *g_papyrus = nullptr;

	PapyrusVRAPI *g_papyrusvr;
	PapyrusVR::VRManagerAPI *g_papyrusvrManager;

	bool isLoaded = false;
	bool isBlocking = false;
	bool isLeftHanded = false;
	bool isLastUpdateValid = false;

	const int numPrevSpeeds = 5; // length of previous kept speeds
	float leftSpeeds[numPrevSpeeds]; // previous n speeds
	float rightSpeeds[numPrevSpeeds]; // previous n speeds

	const int blockCooldown = 10; // number of updates to ignore block start / stop
	int lastBlockStartFrameCount = 0; // number of updates we should still wait before attempting to start blocking
	int lastBlockStopFrameCount = 0; // number of updates we should still wait before attempting to stop blocking

	extern "C" {

		// Papyrus function to periodically set whether we are actually blocking (can't get anim variables using skse)
		void SetIsBlockingDualWield(StaticFunctionTag *base, bool block)
		{
			isBlocking = block;
		}

		TESForm * GetMainHandObject(Actor *actor)
		{
			return actor->GetEquippedObject(false);
		}

		TESForm * GetOffHandObject(Actor *actor)
		{
			return actor->GetEquippedObject(true);
		}

		// Returns 2 if we should start blocking, 1 if we should stop blocking, 0 if no effect
		int GetHandBlockingStatus(TrackedDevicePose *hmdPose, TrackedDevicePose *handPose, float *speeds)
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
			for (int i = 1; i < numPrevSpeeds; i++) {
				speeds[i] = speeds[i - 1];
				if (speeds[i] > maxSpeed) maxSpeed = speeds[i];
			}
			speeds[0] = speed;

			if (maxSpeed <= 0.02 && handForwardDotWithHmdUp >= -0.5 && abs(handForwardDotWithHmdForward) <= 0.4 && abs(hmdToHandVerticalDistance) <= 0.35) {
				if (!isBlocking) {
					// Start blocking
					return 2;
				}
			}
			else if (maxSpeed > 0.06 || handForwardDotWithHmdUp < -0.5 || abs(handForwardDotWithHmdForward) > 0.6 || abs(hmdToHandVerticalDistance) > 0.35) {
				if (isBlocking) {
					// Stop blocking
					return 1;
				}
			}
			return 0;
		}

		bool IsDualWielding(TESForm *mainHandItem, TESForm *offHandItem)
		{
			// must have 2 actual items equipped
			if (!mainHandItem || !offHandItem) return false;

			// main hand has to be a weapon, offhand can be weapon or spell
			if (!mainHandItem->IsWeapon() || !(offHandItem->IsWeapon() || offHandItem->formType == kFormType_Spell)) return false;

			return true;
		}

		// Called on each update (about 90-100 calls per second)

		void OnPoseUpdate(float DeltaTime)
		{
			bool wasLastUpdateValid = isLastUpdateValid;
			isLastUpdateValid = false;
			if (lastBlockStartFrameCount > 0)
				lastBlockStartFrameCount--;
			if (lastBlockStopFrameCount > 0)
				lastBlockStopFrameCount--;

			if (!isLoaded || !g_thePlayer) return;

			PlayerCharacter *pc = *g_thePlayer;
			if (!pc || !pc->actorState.IsWeaponDrawn()) return;

			TESForm *mainHandItem = GetMainHandObject(pc);
			TESForm *offHandItem = GetOffHandObject(pc);
			if(!IsDualWielding(mainHandItem, offHandItem)) {
				// If we switched weapons away from dual wielding, cancel existing block state
				if (wasLastUpdateValid) {
					CSkyrimConsole::RunCommand("player.sendanimevent blockStop");
					_MESSAGE("Stop block");
					isBlocking = false;
				}
				return;
			}

			TrackedDevicePose *hmdPose = g_papyrusvrManager->GetHMDPose();
			TrackedDevicePose *mainHandPose = isLeftHanded ? g_papyrusvrManager->GetLeftHandPose() : g_papyrusvrManager->GetRightHandPose();
			TrackedDevicePose *offHandPose = isLeftHanded ? g_papyrusvrManager->GetRightHandPose() : g_papyrusvrManager->GetLeftHandPose();
			if (!hmdPose || !hmdPose->bPoseIsValid || !mainHandPose || !mainHandPose->bPoseIsValid || !offHandPose || !offHandPose->bPoseIsValid) return;

			int mainHandBlockStatus = GetHandBlockingStatus(hmdPose, mainHandPose, rightSpeeds);

			int offHandBlockStatus = 0;
			if (offHandItem->IsWeapon()) {
				offHandBlockStatus = GetHandBlockingStatus(hmdPose, offHandPose, leftSpeeds);
			}

			if (mainHandBlockStatus == 2 || offHandBlockStatus == 2) { // Either hand is in blocking position
				if (lastBlockStartFrameCount <= 0) { // Do not try to block more than once every n updates
					// Call console to start blocking
					CSkyrimConsole::RunCommand("player.sendanimevent blockStart");
					_MESSAGE("Start block");
					isBlocking = true;
					lastBlockStartFrameCount = blockCooldown;
				}
			}
			else if (mainHandBlockStatus == 1 && (!offHandItem->IsWeapon() || offHandBlockStatus == 1)) { // Either both hands are not blocking, or just main hand and offhand is not a weapon
				if (lastBlockStopFrameCount <= 0) { // Do not try to block more than once every n updates
					// Call console to stop blocking
					CSkyrimConsole::RunCommand("player.sendanimevent blockStop");
					_MESSAGE("Stop block");
					isBlocking = false;
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
					if (g_papyrusvrManager) {
						// Registers for PoseUpdates
						g_papyrusvrManager->RegisterVRUpdateListener(OnPoseUpdate);
					}
					else {
						_MESSAGE("Could not get PapyrusVRManager");
					}
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

		bool RegisterFuncs(VMClassRegistry* registry)
		{
			_MESSAGE("Registering functions");
			registry->RegisterFunction(
				new NativeFunction1<StaticFunctionTag, void, bool>(
					"SetIsBlockingDualWield",
					"DualWieldBlockVR",
					SetIsBlockingDualWield,
					registry)
			);
			return true;
		}

		bool SKSEPlugin_Load(const SKSEInterface * skse)
		{	// Called by SKSE to load this plugin
			_MESSAGE("DualWieldBlockVR loaded");

			// Registers for SKSE Messages (PapyrusVR probably still need to load, wait for SKSE message PostLoad)
			_MESSAGE("Registering for SKSE messages");
			g_messaging = (SKSEMessagingInterface*)skse->QueryInterface(kInterface_Messaging);
			g_messaging->RegisterListener(g_pluginHandle, "SKSE", OnSKSEMessage);

			g_papyrus = (SKSEPapyrusInterface *)skse->QueryInterface(kInterface_Papyrus);
			g_papyrus->Register(RegisterFuncs);

			for (int i = 0; i < numPrevSpeeds; i++) {
				leftSpeeds[i] = 0;
				rightSpeeds[i] = 0;
			}

			// wait for PapyrusVR init (during PostPostLoad SKSE Message)

			return true;
		}
	};

}