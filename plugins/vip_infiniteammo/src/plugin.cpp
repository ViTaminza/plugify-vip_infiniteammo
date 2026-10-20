// vip_infiniteammo — Plugify port of the CookiesVip module "[VIP] Infinite Ammo"
// (vip_infiniteammo.cpp), rewritten to the logic of the partiusfabaa module
// (ref/src/partius-vip-infiniteammo/VIP_InfiniteAmmo.cs).
//
// The old 1:1 port read the pack values as RESERVE ROUNDS and ran a 0.1 s state machine that
// clamped e.g. the "premium" pack to 6 rounds. The production config means MAGAZINES (the shipped
// vip_infiniteammo.ini says "Значения - количество ОБОЙМ в резерве", and the "god" pack is
// "бесконечные обоймы"), so the effect now follows the reference module:
//
//   * the "InfiniteReserveAmmo" feature (VIP_STRING, TOGGLABLE) keeps its value: the pack name
//     from the vip groups.ini. The toggle/display callbacks are unchanged.
//   * config <configs>/vip_infiniteammo/vip_infiniteammo.ini is unchanged: the
//     "InfiniteReserveAmmo" section holds packs, each with "default" and per-weapon magazine
//     counts.
//   * weapon_fire and weapon_reload (pre hooks): a VIP whose slot has the feature enabled and who
//     is alive gets the effect on the ACTIVE weapon at that moment:
//       N >= 999    -> m_iClip1 = the weapon's m_iMaxClip1 (infinite clip, reference's Type 1);
//       0 < N < 999 -> m_pReserveAmmo[0] = N * m_iMaxClip1 (reference's Type 2);
//       N <= 0      -> the weapon is excluded (this replaces the reference's DisabledGuns and is
//                      compatible with the existing config format).
//     Non-firearms are skipped (VData m_iMaxClip1 == 0: knives, grenades, ...).
//   * player_spawn: the same effect is applied once on the next frame (the original's settle
//     window) so a fresh loadout starts with the reserve target. There is no repeating timer and
//     no per-slot weapon tracking any more.
//
// Engine access (PITFALLS C): the active weapon comes from s2sdk::GetClientActiveWeapon, i.e. an
// entity HANDLE validated at the moment of use; the VData (a non-entity object) is read through
// GetEntData2 with the resolved offset; m_iClip1 and m_pReserveAmmo are read and written by
// handle with GetEntData/SetEntData, changeState=true (the equivalent of the original's
// SetStateChanged). m_pReserveAmmo is an INLINE int32[2], so [0] sits at the field offset itself.
// Every game-event handler re-checks the map state (events keep arriving through a changelevel)
// and every deferred callback re-validates the slot and the weapon when it runs.
//
// The pack config is parsed with pisex::KeyValues as before; a failed weapon VData lookup is
// logged once per map (the old code silently skipped the weapon).

#include <pisex/pisex.hpp>
#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include "ammo_logic.hpp"
#include "vip_api_compat.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <map>
#include <string>
#include <string_view>

namespace {

// The ported modules keep the original code's unqualified spellings (VIP_INT, TOGGLABLE,
// DISABLED, ...): the consumer wrapper declares them as plain enums in namespace vip.
using namespace vip;

IVIPApi* g_pVIPCore = &IVIPApi::Get();

constexpr int kMaxSlots = 64;
constexpr const char* kFeature = "InfiniteReserveAmmo";
constexpr const char* kConfigFile = "vip_infiniteammo.ini";
constexpr const char* kConfigSection = "InfiniteReserveAmmo";
constexpr const char* kWeaponClass = "CBasePlayerWeapon";
constexpr const char* kWeaponVDataClass = "CCSWeaponBaseVData";

void LogInfo(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Info, plg::source_location::current());
	}
}

void LogError(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Error, plg::source_location::current());
	}
}

using AmmoPack = vip_infiniteammo::AmmoPack;
using AmmoMode = vip_infiniteammo::AmmoMode;
using AmmoPlan = vip_infiniteammo::AmmoPlan;

std::map<std::string, AmmoPack> g_packs;

// --- schema offsets -----------------------------------------------------------------------------
// Resolved lazily (GetSchemaOffset segfaults before a map exists) and cached on success, like
// pisexcompat's own cache: -1 is retried on the next call. These are cold event paths (one shot
// per event), so the caching only avoids rebuilding the schema keys on every call.

int ReserveAmmoFieldOffset() {
	static int offset = -1;
	if (offset < 0) {
		offset = pisex::SchemaOffset(kWeaponClass, "m_pReserveAmmo");
	}
	return offset;
}

int ClipFieldOffset() {
	static int offset = -1;
	if (offset < 0) {
		offset = pisex::SchemaOffset(kWeaponClass, "m_iClip1");
	}
	return offset;
}

int MaxClipFieldOffset() {
	static int offset = -1;
	if (offset < 0) {
		offset = pisex::SchemaOffset(kWeaponVDataClass, "m_iMaxClip1");
	}
	return offset;
}

int WeaponChainOffset() {
	// pisex::SchemaChainOffset answers -1 for "the object itself is the entity" (and for a missing
	// binding). -2 is the local "not resolved yet" sentinel; 0 must never be used for a change
	// announcement (s2sdk would read the object's vtable as a chainer).
	static int offset = -2;
	if (offset == -2) {
		offset = pisex::SchemaChainOffset(kWeaponClass);
	}
	return offset;
}

// --- handle-based integer fields ----------------------------------------------------------------

bool ReadIntField(int32_t handle, int offset, int& out) {
	if (offset < 0 || !__s2sdk_GetEntData) {
		return false;
	}
	try {
		out = static_cast<int>(s2sdk::GetEntData(handle, offset, static_cast<int32_t>(sizeof(int32_t))));
		return true;
	} catch (...) {
		return false;
	}
}

// changeState=true is the s2sdk equivalent of the original's SetStateChanged: the engine sends the
// new value to the clients.
bool WriteIntField(int32_t handle, int offset, int value, int chainOffset) {
	if (offset < 0 || !__s2sdk_SetEntData) {
		return false;
	}
	try {
		s2sdk::SetEntData(handle, offset, value, static_cast<int32_t>(sizeof(int32_t)), /*changeState=*/true,
						  chainOffset);
		return true;
	} catch (...) {
		return false;
	}
}

// Entity HANDLES are int32 views of the engine's uint32 CEntityHandle: 0 is the world and -1 is
// INVALID_EHANDLE_INDEX; the sign means nothing (once the serial counter passes 65535 bit 31 is
// set). IsValidEntHandle also rejects a stale serial after the weapon was dropped and its slot
// reused.
bool IsUsableWeaponHandle(int32_t handle) {
	if (handle == 0 || handle == -1 || !__s2sdk_EntHandleToEntIndex || !__s2sdk_IsValidEntHandle) {
		return false;
	}
	if (pisex::EntHandleToEntIndex(handle) <= 0) {
		return false;
	}
	try {
		return s2sdk::IsValidEntHandle(handle);
	} catch (...) {
		return false;
	}
}

class InfiniteAmmoPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

private:
	void LoadConfig();
	void ApplyFromEvent(const pisex::IGameEvent* event);
	void ApplyToSlot(int iSlot);
	const AmmoPack* GetPlayerPack(int iSlot);
	void ResetPlayerState(int iSlot);
	void LogVdataFailureOnce(const std::string& weapon);

	// Menu toggle state, mirrored per slot exactly like the old implementation (the VIP core also
	// answers an empty feature string when the '<group>.InfiniteReserveAmmo' cookie is off).
	std::array<bool, kMaxSlots> m_enabled{};
	bool m_vdataFailureLogged = false;
};

InfiniteAmmoPlugin g_infiniteAmmoPlugin;

plg::PluginResult InfiniteAmmoPlugin::OnPluginStart() {
	try {
		// Never fatal: only logs when the s2sdk bindings are missing.
		pisex::Init("vip_infiniteammo");

		LoadConfig();
		for (int i = 0; i < kMaxSlots; ++i) {
			ResetPlayerState(i);
		}

		g_pVIPCore->VIP_RegisterFeature(kFeature, VIP_STRING, TOGGLABLE, nullptr,
										[this](int iSlot, const char* /*szFeature*/, VIP_ToggleState /*eOldStatus*/,
											   VIP_ToggleState& eNewStatus) {
											if (iSlot >= 0 && iSlot < kMaxSlots) {
												m_enabled[static_cast<std::size_t>(iSlot)] = (eNewStatus == ENABLED);
											}
											return false;
										},
										[](int iSlot, const char* szFeature) {
											const char* szPack = g_pVIPCore->VIP_GetClientFeatureString(iSlot, kFeature);
											char buffer[128];
											if (szPack != nullptr && szPack[0] != '\0') {
												std::snprintf(buffer, sizeof(buffer), "%s [%s]",
															  g_pVIPCore->VIP_GetTranslate(szFeature), szPack);
											} else {
												std::snprintf(buffer, sizeof(buffer), "%s",
															  g_pVIPCore->VIP_GetTranslate(szFeature));
											}
											return std::string(buffer);
										});

		g_pVIPCore->VIP_OnClientLoaded([this](int iSlot, bool /*bIsVIP*/) { ResetPlayerState(iSlot); });
		g_pVIPCore->VIP_OnClientDisconnect([this](int iSlot, bool /*bIsVIP*/) { ResetPlayerState(iSlot); });
		g_pVIPCore->VIP_OnPlayerSpawn([this](int iSlot, int /*iTeam*/, bool bIsVIP) {
			ResetPlayerState(iSlot);
			if (!bIsVIP) {
				return;
			}
			// The loadout needs a frame to settle (the original waited through NextFrame too). The
			// callback re-validates everything when it runs: it may land after a map change.
			pisex::NextFrame([this, iSlot]() { ApplyToSlot(iSlot); });
		});

		// The reference module's two triggers (EventWeaponFire / EventWeaponReload). pisex's
		// HookEvent is the Pisex pre hook (s2sdk HookMode::Pre); Shutdown() removes it.
		pisex::HookEvent("weapon_fire",
						 [this](const char*, pisex::IGameEvent* event, bool) { ApplyFromEvent(event); });
		pisex::HookEvent("weapon_reload",
						 [this](const char*, pisex::IGameEvent* event, bool) { ApplyFromEvent(event); });

		// "log once per map" for a failed weapon VData lookup: a fresh map gets a fresh chance (and
		// the log line is not repeated for every shot in between).
		pisex::RegisterMapStartCallback([this](const char*) { m_vdataFailureLogged = false; });

		LogInfo("vip_infiniteammo: ready");
	} catch (const std::exception& ex) {
		LogError(std::string("vip_infiniteammo: OnPluginStart failed: ") + ex.what());
	} catch (...) {
		LogError("vip_infiniteammo: OnPluginStart failed with an unknown exception");
	}
	return {};
}

plg::PluginResult InfiniteAmmoPlugin::OnPluginUpdate(std::chrono::milliseconds /*dt*/) {
	pisex::Poll();
	return {};
}

plg::PluginResult InfiniteAmmoPlugin::OnPluginEnd() {
	g_pVIPCore->VIP_OnClientLoadedUnregister();
	g_pVIPCore->VIP_OnClientDisconnectUnregister();
	g_pVIPCore->VIP_OnPlayerSpawnUnregister();
	// The vip API has no feature unregister: re-registering the feature without callbacks
	// replaces the core's entry with null delegates, so the core keeps no pointer into this
	// library after it is unloaded (same value/feature type: the menu entry is unchanged).
	g_pVIPCore->VIP_RegisterFeature(kFeature, VIP_STRING, TOGGLABLE);
	// Also removes the event hooks, the map-start callback and the next-frame queue.
	pisex::Shutdown();
	return {};
}

void InfiniteAmmoPlugin::LoadConfig() {
	g_packs.clear();
	pisex::KeyValues kv(kConfigSection);
	if (!kv.LoadFromFile(kConfigFile)) {
		LogError(std::string("vip_infiniteammo: failed to load ") + kConfigFile + " - no packs, the module stays idle");
		return;
	}
	FOR_EACH_SUBKEY(&kv, pPackKey) {
		AmmoPack pack;
		pack.name = pPackKey->GetName();
		FOR_EACH_VALUE(pPackKey, pValue) {
			const char* szName = pValue->GetName();
			const int value = pValue->GetInt(nullptr, 0);
			if (std::string_view(szName) == "default") {
				pack.defaultMagazines = value;
			} else {
				pack.weaponOverrides[std::string(szName)] = value;
			}
		}
		if (pack.defaultMagazines != 0 || !pack.weaponOverrides.empty()) {
			g_packs[pack.name] = pack;
		}
	}
	LogInfo("vip_infiniteammo: loaded " + std::to_string(g_packs.size()) + " pack(s)");
}

const AmmoPack* InfiniteAmmoPlugin::GetPlayerPack(int iSlot) {
	if (!g_pVIPCore->VIP_IsClientVIP(iSlot)) {
		return nullptr;
	}
	const char* szPackName = g_pVIPCore->VIP_GetClientFeatureString(iSlot, kFeature);
	if (szPackName == nullptr || szPackName[0] == '\0') {
		return nullptr;
	}
	const auto it = g_packs.find(szPackName);
	return it != g_packs.end() ? &it->second : nullptr;
}

void InfiniteAmmoPlugin::ResetPlayerState(int iSlot) {
	if (iSlot < 0 || iSlot >= kMaxSlots) {
		return;
	}
	m_enabled[static_cast<std::size_t>(iSlot)] = true;
}

void InfiniteAmmoPlugin::LogVdataFailureOnce(const std::string& weapon) {
	if (m_vdataFailureLogged) {
		return;
	}
	m_vdataFailureLogged = true;
	LogError("vip_infiniteammo: weapon VData lookup failed for '" + weapon +
			 "' - the weapon is left alone (logged once per map)");
}

void InfiniteAmmoPlugin::ApplyFromEvent(const pisex::IGameEvent* event) {
	if (event == nullptr || !event->IsValid()) {
		return;
	}
	// Game events are delivered through a changelevel as data; from OnMapEnd to the next map start
	// every client/entity query (including the active weapon below) would crash s2sdk.
	if (!pisex::IsGameReady()) {
		return;
	}
	const int iSlot = event->GetPlayerSlot("userid");
	if (iSlot < 0 || iSlot >= kMaxSlots) {
		return;
	}
	ApplyToSlot(iSlot);
}

void InfiniteAmmoPlugin::ApplyToSlot(int iSlot) {
	if (iSlot < 0 || iSlot >= kMaxSlots) {
		return;
	}
	// Timers, next-frame callbacks and events can run after a map change: revalidate everything
	// here, at the moment of use (PITFALLS B).
	if (!pisex::IsGameReady()) {
		return;
	}
	if (!m_enabled[static_cast<std::size_t>(iSlot)]) {
		return;
	}
	// Cheap guards first: only a living player can have an active weapon, and only a VIP with the
	// feature can have a pack. Bots pass IsAlive but never pass the VIP check.
	if (!pisex::IsAlive(iSlot)) {
		return;
	}
	const AmmoPack* pack = GetPlayerPack(iSlot);
	if (pack == nullptr) {
		return;
	}
	if (!__s2sdk_GetClientActiveWeapon) {
		return;
	}

	int32_t handle = -1;
	try {
		// The active weapon at this moment (m_pWeaponServices -> m_hActiveWeapon), as a handle.
		handle = s2sdk::GetClientActiveWeapon(iSlot);
	} catch (...) {
		return;
	}
	if (!IsUsableWeaponHandle(handle)) {
		return; // no active weapon, or the handle went stale
	}

	try {
		const int weaponIndex = pisex::EntHandleToEntIndex(handle);
		if (weaponIndex <= 0) {
			return;
		}
		const std::string classname = pisex::GetEntityClassname(weaponIndex);
		if (classname.empty()) {
			return;
		}

		if (!__s2sdk_GetWeaponVData || !__s2sdk_GetEntData2) {
			return;
		}
		void* vdata = s2sdk::GetWeaponVData(handle);
		if (vdata == nullptr) {
			LogVdataFailureOnce(classname);
			return;
		}
		// The VData is not an entity: its fields are read through the pointer variant, resolved in
		// this same tick as the weapon handle validation above.
		const int maxClipOffset = MaxClipFieldOffset();
		if (maxClipOffset < 0) {
			LogVdataFailureOnce(std::string(kWeaponVDataClass) + "::m_iMaxClip1");
			return;
		}
		const int maxClip =
			static_cast<int>(s2sdk::GetEntData2(vdata, maxClipOffset, static_cast<int32_t>(sizeof(int32_t))));

		const AmmoPlan plan = vip_infiniteammo::PlanAmmo(*pack, classname, maxClip);
		if (plan.mode == AmmoMode::Excluded) {
			return; // not a firearm, or the pack excludes this weapon (N <= 0)
		}

		// A weapon is the networkable entity itself, so the chain offset is -1 ("the object itself
		// is the entity"). 0 would make s2sdk read the vtable as a chainer.
		const int chainOffset = WeaponChainOffset();
		if (plan.mode == AmmoMode::InfiniteClip) {
			const int offset = ClipFieldOffset();
			int current = 0;
			if (offset < 0 || !ReadIntField(handle, offset, current)) {
				return;
			}
			if (current != plan.value) {
				WriteIntField(handle, offset, plan.value, chainOffset);
			}
			return;
		}

		// m_pReserveAmmo is an inline int32[2]: element [0] is at the field offset.
		const int offset = ReserveAmmoFieldOffset();
		int current = 0;
		if (offset < 0 || !ReadIntField(handle, offset, current)) {
			return;
		}
		if (current != plan.value) {
			WriteIntField(handle, offset, plan.value, chainOffset);
		}
	} catch (const std::exception& ex) {
		LogError("vip_infiniteammo: slot " + std::to_string(iSlot) + " failed: " + ex.what());
	} catch (...) {
		LogError("vip_infiniteammo: slot " + std::to_string(iSlot) + " failed with an unknown exception");
	}
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_infiniteAmmoPlugin)
