set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

# Pin dependency builds to the SAME toolset the project links with - a
# newer vcpkg-picked STL emits helpers older import libs don't export
# -> LNK2001 at project link time. (VS18 BuildTools since 2026-08.)
set(VCPKG_VISUAL_STUDIO_PATH "C:\\Program Files (x86)\\Microsoft Visual Studio\\18\\BuildTools")
set(VCPKG_PLATFORM_TOOLSET v145)
set(VCPKG_PLATFORM_TOOLSET_VERSION 14.51.36231)
