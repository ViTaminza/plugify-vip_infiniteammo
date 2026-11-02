# vip_infiniteammo (Plugify)

Infinite ammo for VIPs: the 'InfiniteReserveAmmo' feature picks a pack from vip_infiniteammo.ini. The pack value is the number of reserve MAGAZINES per weapon ('default' plus per-weapon overrides). On weapon_fire and weapon_reload the active firearm's m_pReserveAmmo is kept at N * its max clip; N >= 999 refills the clip instead (the 'god' pack = infinite magazines), and N <= 0 excludes a weapon. Applied once after each spawn too. A port of the CookiesVip module '[VIP] Infinite Ammo' (vip_infiniteammo), rewritten to the partiusfabaa scheme.

Plugify port (fork) of **[VIP] Infinite Ammo (CookiesVip module)** by _ded_cookies (Metamod (C++)).
The code was rewritten for Plugify / s2sdk.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugin version: 1.1.0
- Plugify dependencies: s2sdk (>=2.18.2), vip (>=1.0.0)
- Self-contained s2sdk integration: uses s2sdk directly, without the pisexcompat library.

## Installation
Download `vip_infiniteammo-1.1.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/vip_infiniteammo`.

`plugins/vip` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

