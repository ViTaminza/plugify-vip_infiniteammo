# vip_infiniteammo (Plugify)

Infinite reserve ammo for VIPs: the 'InfiniteReserveAmmo' feature picks a pack from vip_infiniteammo.ini (per-weapon reserve magazine counts plus a default). A 0.1 s timer keeps the reserve ammo of every firearm at the pack's value for VIPs with the feature enabled: reloads refill, dropping a weapon to another player snaps the reserve back to what the original owner is entitled to, and only the rounds actually spent above the pack value are gone. A port of the CookiesVip module '[VIP] Infinite Ammo' (vip_infiniteammo).

Plugify port (fork) of **[VIP] Infinite Ammo (CookiesVip module)** by _ded_cookies (Metamod (C++)).
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, vip, hudmenu (optional)

## Installation
Download `vip_infiniteammo-1.0.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/vip_infiniteammo`.

`plugins/vip` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

