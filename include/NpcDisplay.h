#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

#include "SlotManager.h"

namespace Npc
{
    struct WearCtx
    {
        std::uint32_t armor{ 0 };
        std::uint32_t pack{ 0 };
        std::uint32_t overTorso{ 0 }, overLLeg{ 0 }, overRLeg{ 0 };
        bool operator==(const WearCtx&) const = default;
    };

    //============= Hooks (called from DisplayManager.cpp) =============
    void Reconcile();
    void OnFurnitureFlip(std::uint32_t actorID, bool enter);
    void OnDrawFlip(std::uint32_t actorID, bool drawn);
    void MarkDirty();
    bool ConsumeDirty();
    void Reset();
    void OnHarvestModel(std::uint32_t npcKey, int slot, RE::NiAVObject* model,
        std::uint32_t formID, std::uint64_t omodHash);

    //============= INI (called from Slots::Save / Slots::Load) =============
    void WriteIni(std::ostream& out);
    bool TryLoadKey(const std::string& key, const std::string& val);

    //============= Panel Access =============
    bool& HideSleepingRef();
    struct TargetInfo
    {
        std::uint32_t id{ 0 };
        std::uint32_t cfgKey{ 0 };
        std::string name;
        WearCtx wear;
    };
    void RefreshTargets();
    const std::vector<TargetInfo>& CachedTargets();
    std::uint32_t SuggestedTarget();
    std::uint32_t TargetGen();
    std::uint32_t SlotItemOf(std::uint32_t npcID, int slot);
    RE::NiAVObject* DisplayNode(std::uint32_t npcID, int slot);
    const char* FamilyLabel(int family);
    int FamilyCount();
    int FamilyOfWeapon(RE::TESObjectWEAP* weap, const RE::ExtraDataList* extra);
    int PlayerEquipSlot(int family, int after);
    int FamilyRank(int family);
    void RefreshAll();
    void RefreshCarrying(std::uint32_t formID);
    void ReapplyTransforms();
    void PreviewTransforms(const Slots::Transform* cell, const Slots::Transform& work);
}
