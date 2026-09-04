#include "EngineCalls.h"

#include "Versions.h"

#include <cstring>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

//============= Engine Call Wrappers =============
namespace Engine
{
    RE::Inventory3DManager* ConstructInventory3DManager(void* mem) {
        using func_t = RE::Inventory3DManager* (*)(RE::Inventory3DManager*);
        static REL::Relocation<func_t> func{ REL::ID(Versions::Table().i3dmCtor) };
        return func(static_cast<RE::Inventory3DManager*>(mem));
    }

    void Begin3D(RE::Inventory3DManager* mgr) {
        using func_t = void (*)(RE::Inventory3DManager*);
        static REL::Relocation<func_t> func{ REL::ID(Versions::Table().begin3D) };
        return func(mgr);
    }

    void UnloadInventoryItem(RE::Inventory3DManager* mgr) {
        using func_t = void (*)(RE::Inventory3DManager*);
        static REL::Relocation<func_t> func{ REL::ID(Versions::Table().unloadItem) };
        return func(mgr);
    }

    void SetWeaponBloodAmount(RE::NiAVObject* root, float amount) {
        using func_t = void (*)(RE::NiAVObject*, float);
        static REL::Relocation<func_t> func{ REL::ID(Versions::Table().weaponBlood) };
        return func(root, amount);
    }

    void LoadInventoryItem(RE::Inventory3DManager* mgr, RE::TESForm* form, const RE::ExtraDataList* extra, std::uint32_t index) {
        using func_t = void (*)(RE::Inventory3DManager*, RE::TESForm*, const RE::ExtraDataList*, std::uint32_t);
        static REL::Relocation<func_t> func{ REL::ID(Versions::Table().loadItem) };
        return func(mgr, form, extra, index);
    }

    RE::NiAVObject* CloneNi(RE::NiAVObject* src) {
        using func_t = RE::NiAVObject* (*)(RE::NiAVObject*);
        static REL::Relocation<func_t> func{ REL::ID(Versions::Table().niClone) };
        return func(src);
    }

    static bool SafeReadQword(const void* addr, std::uint64_t& out) noexcept {
        __try {
            out = *static_cast<const volatile std::uint64_t*>(addr);
            return true;
        } __except (1) {
            return false;
        }
    }

    static bool SafeReadBytes(const void* addr, void* dst, std::size_t len) noexcept {
        __try {
            std::memcpy(dst, addr, len);
            return true;
        } __except (1) {
            return false;
        }
    }

    std::string RttiClassName(const void* obj) {
        const auto imgBase = REL::Module::get().base();
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imgBase);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(imgBase + dos->e_lfanew);
        const std::uint64_t imgEnd = imgBase + nt->OptionalHeader.SizeOfImage;
        const auto inImage = [&](std::uint64_t p) { return p >= imgBase && p < imgEnd; };

        std::uint64_t vtbl = 0;
        if (!obj || !SafeReadQword(obj, vtbl) || !inImage(vtbl)) {
            return {};
        }
        std::uint64_t col = 0;
        if (!SafeReadQword(reinterpret_cast<const void*>(vtbl - 8), col) || !inImage(col)) {
            return {};
        }
        std::uint32_t typeDescRva = 0;
        if (!SafeReadBytes(reinterpret_cast<const void*>(col + 0xC), &typeDescRva, 4)) {
            return {};
        }
        const std::uint64_t typeDesc = imgBase + typeDescRva;
        if (!inImage(typeDesc)) {
            return {};
        }
        char name[96]{};
        if (!SafeReadBytes(reinterpret_cast<const void*>(typeDesc + 0x10), name, sizeof(name) - 1)) {
            return {};
        }
        name[sizeof(name) - 1] = 0;
        return name;
    }

    bool LooksLikeAVObject(const std::string& rtti) {
        return rtti.find("Node") != std::string::npos ||
               rtti.find("NiAVObject") != std::string::npos ||
               rtti.find("BSGeometry") != std::string::npos ||
               rtti.find("TriShape") != std::string::npos;
    }
}
