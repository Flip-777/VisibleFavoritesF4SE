# Visible Favorites - F4SE

Fully dynamic visible favorites system with an in game editor and support for
custom slots. Inspired by mods like B42 Holstered, Immersive Equipment
Displays, and Classic Holstered Weapons - shows all favorited, equipped, and
custom weapons/objects on the player and NPCs in third person. Supports
modded weapons, backpacks, power armor, and armor layers through a dynamic
in game editor. No more patching meshes for every weapon.

## Runtime support

OG 1.10.163, NG 1.10.980/984 and AE 1.11.x. Requires F4SE and the Address
Library for your game version. VR is not supported.

## Building

CMake + MSVC, vcpkg in manifest mode. Check out
[CommonLibF4 (Flip-777 fork, branch `rel3gen`)](https://github.com/Flip-777/CommonLibF4/tree/rel3gen)
next to this repo - stock alandtse will NOT work on the 1.11.x line. Point
`CommonLibF4Path` at the fork's `CommonLibF4` directory and build Release.

## Credits

- Shavkacagarikia for Classic Holstered Weapons
- The F4SE team
- The CommonLibF4 lineage - Ryan-rsm-McKenzie's original and alandtse's fork
- Address Library for F4SE Plugins
- Powerof3's CommonLibF4
- Dear-Modding-FO4 and libxse, whose published ID tables the 1.11 work was
  cross checked against
- Dear ImGui, MinHook and stb_image, bundled - see THIRD-PARTY-LICENSES.txt

## License

GPL-3.0-or-later with the modding exceptions in [EXCEPTIONS](EXCEPTIONS).
