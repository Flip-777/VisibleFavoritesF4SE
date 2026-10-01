#include "EngineCalls.h"

#include "Versions.h"

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
}
