#pragma once

#include <cstdint>

//============= Engine Calls =============
namespace Engine
{
    RE::Inventory3DManager* ConstructInventory3DManager(void* mem);
    void Begin3D(RE::Inventory3DManager* mgr);
    void UnloadInventoryItem(RE::Inventory3DManager* mgr);
    void SetWeaponBloodAmount(RE::NiAVObject* root, float amount);
    void LoadInventoryItem(RE::Inventory3DManager* mgr, RE::TESForm* form, const RE::ExtraDataList* extra, std::uint32_t index);
    RE::NiAVObject* CloneNi(RE::NiAVObject* src);
}
