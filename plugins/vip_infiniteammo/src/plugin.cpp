// vip_infiniteammo — Plugify port of the CookiesVip module "[VIP] Infinite Ammo"
// (the original's vip_infiniteammo.cpp; the vdf name is vip_infiniteammo and the config file
// is vip_infiniteammo.ini).
//
// Behaviour kept identical:
//   * config <configs>/vip_infiniteammo/vip_infiniteammo.ini: section "InfiniteReserveAmmo" with
//     one subkey per pack ("god", "premium", "standart", ...). Each pack holds "default" and
//     per-weapon reserve counts (number of *reserve magazines* in the original's file).
//   * the feature is registered as "InfiniteReserveAmmo" (VIP_STRING, TOGGLABLE): its value is the
//     pack name; the toggle callback mirrors the feature state into a per-slot flag and the
//     display callback renders "<name> [<pack>]".
//   * a 0.1 s timer walks the slots. For an enabled VIP with a known pack and a living pawn, every
//     firearm whose classname has a reserve target keeps its reserve ammo at that target:
//       - above the target           -> clamped down,
//       - first time seen            -> set to the target,
//       - fired one shot             -> expected value follows (no infinite *clip*, the reserve
//                                       only refills what was actually spent above the target),
//       - dropped below target - 1   -> restored to target - 1.
//     The weapon identity is tracked per slot (at most 10 weapons, like the original).
//   * a spawn resets the per-slot state and, for a VIP, processes the weapons twice: once on the
//     next frame and once on the frame after (the original's nested NextFrame - the game gives the
//     loadout a moment to settle).
//
// Port notes:
//   * pWeaponServices->m_hMyWeapons() -> s2sdk::GetClientWeapons(slot) (handles); the weapon
//     pointer is resolved through pisex::EntIndexToEntPointer for the raw schema reads, exactly
//     like plugins/RestrictedWeapons resolves weapon data.
//   * IsFirearm(): the weapon's VData max-clip field is > 0, via s2sdk::GetWeaponVData + the
//     schema field.
//   * m_pReserveAmmo[0] read/write plus a state change on the CBasePlayerWeapon field, the same
//     schema the original used.
//   * the timer is created on the first update with a running map (client queries before a map
//     crash s2sdk); the original created it after StartupServer, i.e. at the same moment.
//   * the original logged the loaded pack count; the port does the same.

#include <pisex/pisex.hpp>
#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include "ammo_logic.hpp"
#include "vip_api_compat.hpp"

#include <array>
#include <chrono>
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
constexpr int kMaxTrackedWeapons = 10;
constexpr float kTimerInterval = 0.1f;
constexpr const char* kFeature = "InfiniteReserveAmmo";
constexpr const char* kConfigFile = "vip_infiniteammo.ini";
constexpr const char* kConfigSection = "InfiniteReserveAmmo";

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

struct TrackedWeapon {
	int32_t handle = 0;
	int expectedReserve = -1;
};

struct PlayerAmmoState {
	std::array<TrackedWeapon, kMaxTrackedWeapons> weapons{};
	int count = 0;
	bool enabled = true;
};

std::map<std::string, AmmoPack> g_packs;

// The two schema offsets of the weapon loop are fixed for the life of the process and are
// resolved once: pisex::SchemaOffset builds a std::string key and does an unordered_map lookup per
// call, and the tick asked for both on every weapon of every player (10 times a second each).
// -1 (schema not up yet / field missing) is retried, exactly like pisexcompat's own cache.
int ReserveAmmoFieldOffset() {
	static int offset = -1;
	if (offset < 0) {
		offset = pisex::SchemaOffset("CBasePlayerWeapon", "m_pReserveAmmo");
	}
	return offset;
}

int MaxClipOffset() {
	static int offset = -1;
	if (offset < 0) {
		offset = pisex::SchemaOffset("CCSWeaponBaseVData", "m_iMaxClip1");
	}
	return offset;
}

// Weapon data helpers (null-safe: every failure path leaves the weapon untouched).
int32_t* ReserveAmmoOf(void* weapon) {
	if (weapon == nullptr) {
		return nullptr;
	}
	const int offset = ReserveAmmoFieldOffset();
	if (offset < 0) {
		return nullptr;
	}
	// m_pReserveAmmo is an INLINE int32[2] ("fixed_array" in the schema dump) despite the m_p
	// name: the original's field-pointer accessor returns the ADDRESS of the field, never a stored
	// pointer. Dereferencing the field bytes as a pointer read the two ammo ints as an address and
	// would fault the moment a VIP with infinite ammo fired a weapon.
	return vip_infiniteammo::ReserveAmmoAt(weapon, offset);
}

bool IsFirearm(int32_t handle) {
	// The handle comes from GetClientWeapons; a weapon that was just dropped leaves a stale
	// handle behind. s2sdk::GetWeaponVData takes an entity HANDLE and warns ("invalid entity
	// handle") for a stale one, so the handle is validated through pisexcompat first.
	if (!__s2sdk_GetWeaponVData || !__s2sdk_GetEntData2 || !__s2sdk_EntHandleToEntIndex) {
		return false;
	}
	try {
		if (pisex::EntHandleToEntIndex(handle) <= 0) {
			return false;
		}
		void* vdata = s2sdk::GetWeaponVData(handle);
		if (vdata == nullptr) {
			return false;
		}
		const int offset = MaxClipOffset();
		if (offset < 0) {
			return false;
		}
		return static_cast<int32_t>(s2sdk::GetEntData2(vdata, offset, 4)) > 0;
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
	void EnsureTimer();
	void Tick();
	void ProcessPlayerWeapons(int iSlot);
	const AmmoPack* GetPlayerPack(int iSlot);
	int FindOrCreateSlot(int iSlot, int32_t handle);
	void ResetPlayerState(int iSlot);

	std::array<PlayerAmmoState, kMaxSlots> m_state{};
	pisex::CTimer* m_timer = nullptr;
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
												m_state[static_cast<std::size_t>(iSlot)].enabled =
													(eNewStatus == ENABLED);
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
			// The original processed the weapons on the next frame and one frame later.
			pisex::NextFrame([this, iSlot]() {
				ProcessPlayerWeapons(iSlot);
				pisex::NextFrame([this, iSlot]() { ProcessPlayerWeapons(iSlot); });
			});
		});

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
	if (!pisex::IsGameReady()) {
		return {}; // client queries before the first map crash s2sdk
	}
	EnsureTimer();
	return {};
}

plg::PluginResult InfiniteAmmoPlugin::OnPluginEnd() {
	if (m_timer != nullptr) {
		pisex::RemoveTimer(m_timer);
		m_timer = nullptr;
	}
	g_pVIPCore->VIP_OnClientLoadedUnregister();
	g_pVIPCore->VIP_OnClientDisconnectUnregister();
	g_pVIPCore->VIP_OnPlayerSpawnUnregister();
	// The vip API has no feature unregister: re-registering the feature without callbacks
	// replaces the core's entry with null delegates, so the core keeps no pointer into this
	// library after it is unloaded (same value/feature type: the menu entry is unchanged).
	g_pVIPCore->VIP_RegisterFeature(kFeature, VIP_STRING, TOGGLABLE);
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
				pack.defaultReserve = value;
			} else {
				pack.weaponOverrides[std::string(szName)] = value;
			}
		}
		if (pack.defaultReserve > 0 || !pack.weaponOverrides.empty()) {
			g_packs[pack.name] = pack;
		}
	}
	LogInfo("vip_infiniteammo: loaded " + std::to_string(g_packs.size()) + " pack(s)");
}

void InfiniteAmmoPlugin::EnsureTimer() {
	if (m_timer != nullptr) {
		return;
	}
	m_timer = pisex::CreateTimer(kTimerInterval, [this]() -> float {
		try {
			Tick();
		} catch (const std::exception& ex) {
			LogError(std::string("vip_infiniteammo: tick failed: ") + ex.what());
		} catch (...) {
			LogError("vip_infiniteammo: tick failed with an unknown exception");
		}
		return kTimerInterval;
	});
}

void InfiniteAmmoPlugin::Tick() {
	for (int i = 0; i < kMaxSlots; ++i) {
		try {
			ProcessPlayerWeapons(i);
		} catch (const std::exception& ex) {
			LogError("vip_infiniteammo: slot " + std::to_string(i) + " failed: " + ex.what());
		} catch (...) {
			LogError("vip_infiniteammo: slot " + std::to_string(i) + " failed with an unknown exception");
		}
	}
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

int InfiniteAmmoPlugin::FindOrCreateSlot(int iSlot, int32_t handle) {
	PlayerAmmoState& state = m_state[static_cast<std::size_t>(iSlot)];
	for (int i = 0; i < state.count; ++i) {
		if (state.weapons[static_cast<std::size_t>(i)].handle == handle) {
			return i;
		}
	}
	if (state.count < kMaxTrackedWeapons) {
		const int index = state.count;
		state.weapons[static_cast<std::size_t>(index)].handle = handle;
		state.weapons[static_cast<std::size_t>(index)].expectedReserve = -1;
		++state.count;
		return index;
	}
	return -1;
}

void InfiniteAmmoPlugin::ResetPlayerState(int iSlot) {
	if (iSlot < 0 || iSlot >= kMaxSlots) {
		return;
	}
	PlayerAmmoState& state = m_state[static_cast<std::size_t>(iSlot)];
	state.count = 0;
	state.enabled = true;
	for (TrackedWeapon& weapon : state.weapons) {
		weapon.handle = 0;
		weapon.expectedReserve = -1;
	}
}

void InfiniteAmmoPlugin::ProcessPlayerWeapons(int iSlot) {
	PlayerAmmoState& state = m_state[static_cast<std::size_t>(iSlot)];
	if (!state.enabled) {
		return;
	}
	// Cheap guards first: only a living player can have a weapon to top up, and the pack lookup is
	// a VIP API call (a cross-plugin call into the vip core, which composes the
	// '<group>.InfiniteReserveAmmo' cookie key and reads the permissions cookie store) while
	// GetClientWeapons is an s2sdk call that allocates the handle vector. All of them are pure
	// reads, so their order is not observable.
	if (!pisex::IsAlive(iSlot)) {
		return;
	}
	const AmmoPack* pack = GetPlayerPack(iSlot);
	if (pack == nullptr) {
		return;
	}
	if (!__s2sdk_GetClientWeapons) {
		return;
	}

	plg::vector<int32_t> handles;
	try {
		handles = s2sdk::GetClientWeapons(iSlot);
	} catch (...) {
		return;
	}

	for (const int32_t handle : handles) {
		if (handle == 0) {
			continue;
		}
		if (!IsFirearm(handle)) {
			continue;
		}
		std::string classname;
		try {
			const int index = pisex::EntHandleToEntIndex(handle);
			classname = index > 0 ? pisex::GetEntityClassname(index) : std::string();
		} catch (...) {
			continue;
		}
		if (classname.empty()) {
			continue;
		}
		const int target = vip_infiniteammo::TargetReserve(*pack, classname);
		if (target <= 0) {
			continue;
		}
		const int slotIndex = FindOrCreateSlot(iSlot, handle);
		if (slotIndex < 0) {
			continue;
		}
		TrackedWeapon& tracked = state.weapons[static_cast<std::size_t>(slotIndex)];

		int32_t* reserve = nullptr;
		try {
			const int index = pisex::EntHandleToEntIndex(handle);
			reserve = ReserveAmmoOf(pisex::EntIndexToEntPointer(index));
		} catch (...) {
			reserve = nullptr;
		}
		if (reserve == nullptr) {
			continue;
		}
		const int current = reserve[0];
		const int entityIndex = pisex::EntHandleToEntIndex(handle);
		// The original's reserve bookkeeping (ammo_logic.hpp, shared with the test). "set" writes
		// the value and marks the schema field as changed, like the original's SetStateChanged.
		const vip_infiniteammo::ReserveDecision decision =
			vip_infiniteammo::DecideReserve(current, tracked.expectedReserve, target);
		if (decision.set) {
			reserve[0] = decision.value;
			pisex::SetStateChanged(entityIndex, "CBasePlayerWeapon", "m_pReserveAmmo");
		}
		tracked.expectedReserve = decision.expected;
	}
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_infiniteAmmoPlugin)
