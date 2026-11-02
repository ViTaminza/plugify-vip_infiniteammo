#pragma once
// vip_infiniteammo — constants, config path resolution and logging shared by the plugin, its local
// headers and the unit test.
//
// The plugin is pure s2sdk (docs/PLUGIN_STANDARD.md §7): there is no pisexcompat and no guard
// library, so the pisex::KeyValues file lookup lives here and keeps the old candidate order. The
// production layout (configs/vip_infiniteammo/vip_infiniteammo.ini, installed by package.sh) is
// therefore found unchanged:
//
//   vip_infiniteammo.ini                                       the server working directory (as given)
//   <GetConfigsDir()>/vip_infiniteammo/vip_infiniteammo.ini    the packaged location
//   <GetConfigsDir()>/vip_infiniteammo.ini                     a config dropped straight into configs/
//
// Header-only, so the test drives the path resolution with a fake plg::GetConfigsDir.

#include <plg/plugin.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vip_infiniteammo {

// The log prefix the port has always used ("vip_infiniteammo: ...").
inline constexpr char kLogTag[] = "vip_infiniteammo";

// The feature of the original module, as groups.ini spells it: its value is the pack name read
// from vip_infiniteammo.ini.
inline constexpr char kFeature[] = "InfiniteReserveAmmo";

// CS2 has 64 player slots; the per-slot arrays of the plugin (feature state and the pending spawn
// flush) are sized with this.
inline constexpr int kMaxSlots = 64;

// Every ammo field of the schema is an int32: m_iClip1, m_iMaxClip1 and both elements of the
// inline int32[2] m_pReserveAmmo.
inline constexpr int32_t kInt32Size = static_cast<int32_t>(sizeof(int32_t));

// CBasePlayerWeapon carries m_iClip1 and the inline m_pReserveAmmo; the weapon VData
// (CCSWeaponBaseVData, with m_iMaxClip1 inherited from CBasePlayerWeaponVData) is the non-entity
// object s2sdk::GetWeaponVData answers for the weapon handle.
inline constexpr char kWeaponClass[] = "CBasePlayerWeapon";
inline constexpr char kWeaponVDataClass[] = "CCSWeaponBaseVData";

// The config file of the original plugin (addons/configs/vip/vip_infiniteammo.ini), installed by
// package.sh into <configs>/vip_infiniteammo/ (PLUGIN_STANDARD §3).
inline constexpr char kConfigFile[] = "vip_infiniteammo.ini";
inline constexpr char kConfigSubdir[] = "vip_infiniteammo";

// 0 <= slot < 64 (PITFALLS C2). Nothing below a failed range check may reach an s2sdk export.
inline bool IsSlotInRange(int32_t iSlot) {
	return iSlot >= 0 && iSlot < kMaxSlots;
}

inline std::string JoinPath(std::string_view dir, std::string_view file) {
	if (dir.empty()) {
		return std::string(file);
	}
	std::string path(dir);
	if (path.back() != '/') {
		path.push_back('/');
	}
	if (!file.empty() && file.front() == '/') {
		file.remove_prefix(1);
	}
	path.append(file);
	return path;
}

// <plg::GetConfigsDir()>; "" when the host provides no configs dir (the test binary drives it).
inline std::string ConfigsDir() {
	if (plg::GetConfigsDir == nullptr) {
		return {};
	}
	std::string dir(plg::GetConfigsDir());
	return dir;
}

// The candidate paths in the order the pisexcompat build tried them: the path as given first (the
// server working directory), then the packaged plugin directory and the configs root.
inline std::vector<std::string> ConfigCandidates() {
	std::vector<std::string> candidates;
	candidates.emplace_back(kConfigFile);
	const std::string root = ConfigsDir();
	if (!root.empty()) {
		candidates.push_back(JoinPath(root, std::string(kConfigSubdir) + '/' + kConfigFile));
		candidates.push_back(JoinPath(root, kConfigFile));
	}
	return candidates;
}

// The most likely location, for log lines: the packaged one when the host provides a configs dir,
// the bare file name otherwise.
inline std::string PrimaryConfigPath() {
	const std::vector<std::string> candidates = ConfigCandidates();
	return candidates.size() > 1 ? candidates[1] : candidates.front();
}

inline void LogInfo(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Info, plg::source_location::current());
	}
}

inline void LogError(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Error, plg::source_location::current());
	}
}

} // namespace vip_infiniteammo
