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
//   * player_spawn: the same effect is applied once after the loadout settles (the original's
//     settle window) so a fresh spawn starts with the reserve target. There is no repeating timer
//     and no per-slot weapon tracking any more.
//
// Pure s2sdk, no pisexcompat (docs/PLUGIN_STANDARD.md §7). Every safety rule the old wrappers
// enforced lives in this plugin and in its own src/ headers:
//   * guards.hpp — map gate (PITFALLS B1/B2), IsAlive (PITFALLS C2), the entity-handle filter
//     (PITFALLS C1/C2), the map-gated SchemaOffset/SchemaChainOffset cache (PITFALLS C8) and the
//     guarded int32 field read/write;
//   * schema_cache.hpp — the offset cache policy that guards.hpp drives;
//   * kv1.hpp / config.hpp — the self-contained Valve KeyValues reader that replaced
//     pisex::KeyValues, and the pack table built from it;
//   * ammo_logic.hpp — the engine-free target computation (N magazines -> N * m_iMaxClip1 rounds,
//     N >= 999 -> infinite clip, N <= 0 -> excluded), shared with the unit test.
//
// Port notes:
//   * Map game (B1/B2): the OnMapStart/OnMapEnd listeners are registered in OnPluginStart and
//     removed in OnPluginEnd; g_mapReady is latched by OnMapStart and by the first OnPluginUpdate
//     of a plugin loaded into a running map, and dropped by OnMapEnd. No client, entity or schema
//     call is made while the gate is closed. OnMapStart also clears the once-per-map VData log
//     latch, OnMapEnd/OnPluginEnd throw the pending spawn queue away.
//   * The active weapon comes from s2sdk::GetClientActiveWeapon, i.e. an entity HANDLE validated
//     at the moment of use: not 0/-1, and s2sdk::IsValidEntHandle() must agree (a stale serial
//     after the weapon was dropped is rejected). The class name is read with
//     s2sdk::GetEntityClassname(handle) - the s2sdk 3.0 export takes the handle itself, so the old
//     EntHandleToEntIndex + index->classname round trip is gone.
//   * The VData (a non-entity object) is read through GetEntData2 with the resolved offset;
//     m_iClip1 and m_pReserveAmmo are read and written by handle with GetEntData/SetEntData,
//     changeState=true (the equivalent of the original's SetStateChanged). m_pReserveAmmo is an
//     INLINE int32[2], so [0] sits at the field offset itself (PITFALLS C5: never dereference it
//     as a pointer). Every game-event handler re-checks the map state (events keep arriving
//     through a changelevel) and every deferred apply re-validates slot, feature, pawn and weapon
//     when it runs.
//   * The original's NextFrame-after-spawn is a 64-slot pending flag table flushed from
//     OnPluginUpdate: no s2sdk timer/next-frame callback can fire during a changelevel, and the
//     flush only happens once the map gate is open (PITFALLS B4).
//   * A failed weapon VData lookup is logged once per map (the old code silently skipped the
//     weapon); OnMapStart resets that latch.
//   * OnPluginStart never returns Failed (PITFALLS A1: that stops every Plugify plugin) and
//     OnPluginEnd removes every hook/listener registered here, so `plg reload` is safe (A7).

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <vip_api.hpp>

#include "ammo_logic.hpp"
#include "common.hpp"
#include "config.hpp"
#include "guards.hpp"
#include "kv1.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

// The ported modules keep the original code's unqualified spellings (VIP_INT, TOGGLABLE,
// DISABLED, ...): the consumer wrapper declares them as plain enums in namespace vip.
using namespace vip;

// Short spelling of the plugin's own namespace (guards, config, ammo logic).
namespace ia = vip_infiniteammo;

using AmmoPack = vip_infiniteammo::AmmoPack;
using AmmoMode = vip_infiniteammo::AmmoMode;
using AmmoPlan = vip_infiniteammo::AmmoPlan;

IVIPApi* g_pVIPCore = &IVIPApi::Get();

// The pack table loaded from vip_infiniteammo.ini (pack name -> magazines per weapon).
std::map<std::string, AmmoPack> g_packs;

// The vip core may already be going away on a full unload: a throwing unregister must not skip the
// remaining registrations, the library has to leave nothing behind (PITFALLS A7).
template <typename Callback>
void GuardCoreCall(Callback&& callback) {
	try {
		callback();
	} catch (...) {
	}
}

class InfiniteAmmoPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

	// The shared "weapon_fire"/"weapon_reload" pre-hook. s2sdk only accepts plain function
	// pointers, so this is a static member that forwards to the plugin instance.
	static s2sdk::ResultType OnWeaponEvent(const plg::string& name, void* event, bool dontBroadcast);

	// Map-lifecycle hooks of guards.hpp: OnMapStart resets the once-per-map log latch, OnMapEnd
	// drops the pending spawn queue (both only touch plugin state, never a client).
	static void OnMapStartObserver();
	static void OnMapEndObserver();

private:
	void LoadConfig();
	void ApplyFromEvent(void* event);
	void ApplyToSlot(int iSlot);
	void FlushPendingSpawns();
	void ClearPendingSpawns();
	const AmmoPack* GetPlayerPack(int iSlot);
	void ResetPlayerState(int iSlot);
	void LogVdataFailureOnce(const std::string& weapon);

	// Menu toggle state, mirrored per slot exactly like the old implementation (the VIP core also
	// answers an empty feature string when the '<group>.InfiniteReserveAmmo' cookie is off).
	std::array<bool, ia::kMaxSlots> m_enabled{};
	// PITFALLS B4: the original applied the effect through NextFrame right after a spawn. s2sdk
	// next-frame callbacks survive a changelevel, so the slot is only queued here and flushed by
	// OnPluginUpdate once g_mapReady is true; ApplyToSlot re-validates everything at that moment.
	std::array<bool, ia::kMaxSlots> m_pendingSpawnSlots{};
	bool m_vdataFailureLogged = false;
	bool m_weaponFireHooked = false;
	bool m_weaponReloadHooked = false;
};

InfiniteAmmoPlugin g_infiniteAmmoPlugin;

void InfiniteAmmoPlugin::OnMapStartObserver() {
	// "Log once per map" for a failed weapon VData lookup: a fresh map gets a fresh chance (and the
	// line is not repeated for every shot in between).
	g_infiniteAmmoPlugin.m_vdataFailureLogged = false;
}

void InfiniteAmmoPlugin::OnMapEndObserver() {
	// PITFALLS B4: a queue entry must not survive into the next map and apply the old spawn's
	// effect to whoever occupies the slot by then.
	g_infiniteAmmoPlugin.ClearPendingSpawns();
}

s2sdk::ResultType InfiniteAmmoPlugin::OnWeaponEvent(const plg::string& /*name*/, void* event,
													bool /*dontBroadcast*/) {
	try {
		g_infiniteAmmoPlugin.ApplyFromEvent(event);
	} catch (const std::exception& ex) {
		ia::LogError(std::string(ia::kLogTag) + ": weapon event failed: " + ex.what());
	} catch (...) {
		ia::LogError(std::string(ia::kLogTag) + ": weapon event failed with an unknown exception");
	}
	// A Pre hook must answer Continue on every path: a result >= Handled frees the event for every
	// other plugin on the server (the original's listener returned false for the same reason).
	return s2sdk::ResultType::Continue;
}

plg::PluginResult InfiniteAmmoPlugin::OnPluginStart() {
	try {
		// PITFALLS B1/B2: the map gate owns every client/entity/schema call this plugin makes. It
		// registers the s2sdk map listeners (safe here: no client or entity is touched) and the two
		// per-map hooks below. Nothing is fatal: a missing binding only logs and keeps the gate
		// shut.
		ia::SetMapLifecycleCallbacks(&InfiniteAmmoPlugin::OnMapStartObserver,
									 &InfiniteAmmoPlugin::OnMapEndObserver);
		ia::InitMapGate();

		m_vdataFailureLogged = false;
		ClearPendingSpawns();

		LoadConfig();
		for (int iSlot = 0; iSlot < ia::kMaxSlots; ++iSlot) {
			ResetPlayerState(iSlot);
		}

		g_pVIPCore->VIP_RegisterFeature(
			ia::kFeature, VIP_STRING, TOGGLABLE, nullptr,
			[this](int iSlot, const char* /*szFeature*/, VIP_ToggleState /*eOldStatus*/,
				   VIP_ToggleState& eNewStatus) {
				if (ia::IsSlotInRange(iSlot)) {
					m_enabled[static_cast<std::size_t>(iSlot)] = (eNewStatus == ENABLED);
				}
				return false;
			},
			[](int iSlot, const char* szFeature) {
				const char* szPack = g_pVIPCore->VIP_GetClientFeatureString(iSlot, ia::kFeature);
				char buffer[128];
				if (szPack != nullptr && szPack[0] != '\0') {
					std::snprintf(buffer, sizeof(buffer), "%s [%s]", g_pVIPCore->VIP_GetTranslate(szFeature),
								  szPack);
				} else {
					std::snprintf(buffer, sizeof(buffer), "%s", g_pVIPCore->VIP_GetTranslate(szFeature));
				}
				return std::string(buffer);
			});

		g_pVIPCore->VIP_OnClientLoaded([this](int iSlot, bool /*bIsVIP*/) { ResetPlayerState(iSlot); });
		g_pVIPCore->VIP_OnClientDisconnect([this](int iSlot, bool /*bIsVIP*/) { ResetPlayerState(iSlot); });
		g_pVIPCore->VIP_OnPlayerSpawn([this](int iSlot, int /*iTeam*/, bool bIsVIP) {
			// Nothing may unwind into the vip core's spawn dispatch.
			try {
				ResetPlayerState(iSlot);
				if (!bIsVIP || !ia::IsSlotInRange(iSlot)) {
					return;
				}
				// The loadout needs a frame to settle (the original waited through NextFrame too).
				// The queue is flushed by OnPluginUpdate; the apply path re-validates the slot, the
				// feature, the pawn and the weapon when it finally runs.
				m_pendingSpawnSlots[static_cast<std::size_t>(iSlot)] = true;
			} catch (const std::exception& ex) {
				ia::LogError(std::string(ia::kLogTag) + ": OnPlayerSpawn failed: " + ex.what());
			} catch (...) {
				ia::LogError(std::string(ia::kLogTag) + ": OnPlayerSpawn failed with an unknown exception");
			}
		});

		// PITFALLS C12/C13: the reference module's two triggers (EventWeaponFire/EventWeaponReload)
		// are pre hooks; both are removed in OnPluginEnd. A failed hook only disables that trigger.
		if (__s2sdk_HookEvent == nullptr) {
			ia::LogError(std::string(ia::kLogTag) +
						 ": the s2sdk event bindings are unavailable - infinite ammo is inactive");
		} else {
			if (s2sdk::HookEvent("weapon_fire", &InfiniteAmmoPlugin::OnWeaponEvent,
								 s2sdk::HookMode::Pre) == s2sdk::EventHookError::Okay) {
				m_weaponFireHooked = true;
			} else {
				ia::LogError(std::string(ia::kLogTag) + ": hooking 'weapon_fire' failed - fire is not covered");
			}
			if (s2sdk::HookEvent("weapon_reload", &InfiniteAmmoPlugin::OnWeaponEvent,
								 s2sdk::HookMode::Pre) == s2sdk::EventHookError::Okay) {
				m_weaponReloadHooked = true;
			} else {
				ia::LogError(std::string(ia::kLogTag) +
							 ": hooking 'weapon_reload' failed - reload is not covered");
			}
		}

		ia::LogInfo(std::string(ia::kLogTag) + ": ready");
	} catch (const std::exception& ex) {
		ia::LogError(std::string(ia::kLogTag) + ": OnPluginStart failed: " + ex.what());
	} catch (...) {
		ia::LogError(std::string(ia::kLogTag) + ": OnPluginStart failed with an unknown exception");
	}
	return {}; // never Failed: that stops every plugify plugin (PITFALLS A1)
}

plg::PluginResult InfiniteAmmoPlugin::OnPluginUpdate(std::chrono::milliseconds /*dt*/) {
	try {
		// Opens the gate on the first update after a mid-map load; after OnMapEnd only a new
		// OnMapStart opens it again (PITFALLS B1/B2).
		ia::PollMapGate();
		if (!ia::IsMapReady()) {
			return {};
		}
		FlushPendingSpawns();
	} catch (const std::exception& ex) {
		ia::LogError(std::string(ia::kLogTag) + ": OnPluginUpdate failed: " + ex.what());
	} catch (...) {
		ia::LogError(std::string(ia::kLogTag) + ": OnPluginUpdate failed with an unknown exception");
	}
	return {};
}

plg::PluginResult InfiniteAmmoPlugin::OnPluginEnd() {
	try {
		// PITFALLS A7/C12: every registration of this library is removed again while it is still
		// mapped, so `plg reload` never calls into the unloaded .so.
		if (m_weaponFireHooked) {
			m_weaponFireHooked = false;
			if (__s2sdk_UnhookEvent) {
				s2sdk::UnhookEvent("weapon_fire", &InfiniteAmmoPlugin::OnWeaponEvent, s2sdk::HookMode::Pre);
			}
		}
		if (m_weaponReloadHooked) {
			m_weaponReloadHooked = false;
			if (__s2sdk_UnhookEvent) {
				s2sdk::UnhookEvent("weapon_reload", &InfiniteAmmoPlugin::OnWeaponEvent, s2sdk::HookMode::Pre);
			}
		}
		// The map listeners point into this library as well; drop the per-map hooks first, then
		// unregister and close the gate.
		ia::SetMapLifecycleCallbacks(nullptr, nullptr);
		ia::ShutdownMapGate();
		// PITFALLS C8: a reload resolves the schema fields against the live map again instead of
		// reusing offsets a previous instance happened to have found.
		ia::ClearSchemaOffsets();
		ClearPendingSpawns();
		m_vdataFailureLogged = false;

		// The vip API has no feature unregister: re-registering the feature without callbacks
		// replaces the core's entry with null delegates, so the core keeps no pointer into this
		// library after it is unloaded (same value/feature type: the menu entry is unchanged).
		GuardCoreCall([this] { g_pVIPCore->VIP_OnClientLoadedUnregister(); });
		GuardCoreCall([this] { g_pVIPCore->VIP_OnClientDisconnectUnregister(); });
		GuardCoreCall([this] { g_pVIPCore->VIP_OnPlayerSpawnUnregister(); });
		GuardCoreCall([] { g_pVIPCore->VIP_RegisterFeature(ia::kFeature, VIP_STRING, TOGGLABLE); });
	} catch (const std::exception& ex) {
		ia::LogError(std::string(ia::kLogTag) + ": OnPluginEnd failed: " + ex.what());
	} catch (...) {
		ia::LogError(std::string(ia::kLogTag) + ": OnPluginEnd failed with an unknown exception");
	}
	return {};
}

void InfiniteAmmoPlugin::LoadConfig() {
	g_packs.clear();
	const std::vector<std::string> candidates = ia::ConfigCandidates();
	for (const std::string& path : candidates) {
		// ReadTextFile separates "not installed here" (try the next candidate) from a malformed
		// file below (log and stay idle, like the pisexcompat loader did).
		const std::optional<std::string> text = ia::ReadTextFile(path);
		if (!text) {
			continue;
		}
		const std::optional<ia::KvNode> root = ia::ParseKv1(*text);
		if (!root) {
			ia::LogError(std::string(ia::kLogTag) + ": failed to parse " + path +
						 " - no packs, the module stays idle");
			return;
		}
		g_packs = ia::PacksFromKv(*root);
		ia::LogInfo(std::string(ia::kLogTag) + ": loaded " + std::to_string(g_packs.size()) + " pack(s) from " +
					path);
		return;
	}
	ia::LogError(std::string(ia::kLogTag) + ": failed to load " + ia::PrimaryConfigPath() + " (tried " +
				 std::to_string(candidates.size()) + " candidate path(s)) - no packs, the module stays idle");
}

const AmmoPack* InfiniteAmmoPlugin::GetPlayerPack(int iSlot) {
	if (!ia::IsSlotInRange(iSlot) || !g_pVIPCore->VIP_IsClientVIP(iSlot)) {
		return nullptr;
	}
	const char* szPackName = g_pVIPCore->VIP_GetClientFeatureString(iSlot, ia::kFeature);
	if (szPackName == nullptr || szPackName[0] == '\0') {
		return nullptr;
	}
	const auto it = g_packs.find(szPackName);
	return it != g_packs.end() ? &it->second : nullptr;
}

void InfiniteAmmoPlugin::ResetPlayerState(int iSlot) {
	if (!ia::IsSlotInRange(iSlot)) {
		return;
	}
	// The core answers an empty feature string when the cookie is off; an enabled slot simply gets
	// the pack of its group (the original's g_bInfiniteAmmo reset).
	m_enabled[static_cast<std::size_t>(iSlot)] = true;
}

void InfiniteAmmoPlugin::ClearPendingSpawns() {
	m_pendingSpawnSlots.fill(false);
}

void InfiniteAmmoPlugin::FlushPendingSpawns() {
	if (!ia::IsMapReady()) {
		return;
	}
	for (int iSlot = 0; iSlot < ia::kMaxSlots; ++iSlot) {
		const auto index = static_cast<std::size_t>(iSlot);
		if (!m_pendingSpawnSlots[index]) {
			continue;
		}
		// Clear before applying: the entry must not run twice even if the apply path fails.
		m_pendingSpawnSlots[index] = false;
		ApplyToSlot(iSlot);
	}
}

void InfiniteAmmoPlugin::LogVdataFailureOnce(const std::string& weapon) {
	if (m_vdataFailureLogged) {
		return;
	}
	m_vdataFailureLogged = true;
	ia::LogError(std::string(ia::kLogTag) + ": weapon VData lookup failed for '" + weapon +
				 "' - the weapon is left alone (logged once per map)");
}

void InfiniteAmmoPlugin::ApplyFromEvent(void* event) {
	if (event == nullptr) {
		return;
	}
	// Game events are delivered through a changelevel as data; from OnMapEnd to the next map start
	// every client/entity query (including the active weapon below) would crash s2sdk.
	if (!ia::IsMapReady()) {
		return;
	}
	if (__s2sdk_GetEventPlayerSlot == nullptr) {
		return;
	}
	int iSlot = -1;
	try {
		iSlot = s2sdk::GetEventPlayerSlot(event, "userid");
	} catch (...) {
		return;
	}
	if (!ia::IsSlotInRange(iSlot)) {
		return;
	}
	ApplyToSlot(iSlot);
}

void InfiniteAmmoPlugin::ApplyToSlot(int iSlot) {
	if (!ia::IsSlotInRange(iSlot)) {
		return;
	}
	// Spawn flushes and events can run after a map change: revalidate everything here, at the
	// moment of use (PITFALLS B).
	if (!ia::IsMapReady()) {
		return;
	}
	if (!m_enabled[static_cast<std::size_t>(iSlot)]) {
		return;
	}
	// Cheap guards first: only a living player can have an active weapon, and only a VIP with the
	// feature can have a pack. Bots pass IsAlive but never pass the VIP check.
	if (!ia::IsAlive(iSlot)) {
		return;
	}
	const AmmoPack* pack = GetPlayerPack(iSlot);
	if (pack == nullptr) {
		return;
	}
	if (__s2sdk_GetClientActiveWeapon == nullptr || __s2sdk_GetEntityClassname == nullptr ||
		__s2sdk_GetWeaponVData == nullptr || __s2sdk_GetEntData2 == nullptr) {
		return;
	}

	try {
		// The active weapon at this moment (m_pWeaponServices -> m_hActiveWeapon), as a handle.
		const int32_t handle = s2sdk::GetClientActiveWeapon(iSlot);
		// The handle is validated at the moment of use (PITFALLS C1/C2): 0 is the world, -1 is
		// INVALID_EHANDLE_INDEX, and s2sdk::IsValidEntHandle also rejects a stale serial after the
		// weapon was dropped and its slot reused. A valid handle may be negative once the engine's
		// serial counter passes 65535, so the numeric filter is not "handle > 0".
		if (!ia::IsUsableEntityHandle(handle)) {
			return;
		}
		const plg::string weaponName = s2sdk::GetEntityClassname(handle);
		const std::string classname(weaponName.data(), weaponName.size());
		if (classname.empty()) {
			return;
		}

		// The VData is not an entity: read it through the pointer variant, in this same tick as
		// the weapon handle validation above (PITFALLS C3/C4).
		void* vdata = s2sdk::GetWeaponVData(handle);
		if (vdata == nullptr) {
			LogVdataFailureOnce(classname);
			return;
		}
		const int32_t maxClipOffset = ia::SchemaOffset(ia::kWeaponVDataClass, "m_iMaxClip1");
		if (maxClipOffset < 0) {
			LogVdataFailureOnce(std::string(ia::kWeaponVDataClass) + "::m_iMaxClip1");
			return;
		}
		const int maxClip = static_cast<int>(s2sdk::GetEntData2(vdata, maxClipOffset, ia::kInt32Size));

		const AmmoPlan plan = ia::PlanAmmo(*pack, classname, maxClip);
		if (plan.mode == AmmoMode::Excluded) {
			return; // not a firearm, or the pack excludes this weapon (N <= 0)
		}

		// m_pReserveAmmo is an inline int32[2]: element [0] is at the field offset (PITFALLS C5).
		const char* fieldName =
			plan.mode == AmmoMode::InfiniteClip ? "m_iClip1" : "m_pReserveAmmo";
		const int32_t offset = ia::SchemaOffset(ia::kWeaponClass, fieldName);
		int current = 0;
		if (!ia::ReadIntField(handle, offset, current)) {
			return;
		}
		if (current != plan.value) {
			// A weapon is the networkable entity itself, so the chain offset is -1 ("the object
			// itself is the entity"); 0 or anything positive would make s2sdk walk the object as if
			// it had a __m_pChainEntity there. Resolved only for the write, which needs it.
			const int32_t chainOffset = ia::SchemaChainOffset(ia::kWeaponClass);
			// changeState=true is the original's SetStateChanged: the clients see the new ammo now.
			ia::WriteIntField(handle, offset, plan.value, chainOffset);
		}
	} catch (const std::exception& ex) {
		ia::LogError(std::string(ia::kLogTag) + ": slot " + std::to_string(iSlot) + " failed: " + ex.what());
	} catch (...) {
		ia::LogError(std::string(ia::kLogTag) + ": slot " + std::to_string(iSlot) +
					 " failed with an unknown exception");
	}
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_infiniteAmmoPlugin)
