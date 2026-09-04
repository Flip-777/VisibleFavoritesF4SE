#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "NpcDisplay.h"
#include "SlotManager.h"

//============= Display Engine =============
namespace Display
{
    struct SlotState
    {
        std::uint32_t formID{ 0 };
        RE::NiPointer<RE::NiAVObject> source;
        RE::TESBoundObject* object{ nullptr };
        bool equippedDisplay{ false };
        std::uint64_t omodHash{ 0 };
    };

    struct LadderTables
    {
        Slots::LadderSet& set;
        Slots::Transform* slotsPA;
        Slots::NpcTableSet* npc;
        Slots::Transform& Default(int slot) const { return npc ? npc->slots[slot].t : Slots::g_slots[slot].t; }
    };

    extern SlotState g_state[Slots::MAX_INDEX];
    extern std::recursive_mutex g_tablesMutex;
    extern std::atomic<bool> g_dirty;
    extern bool g_inPA;
    extern bool g_playerInBed;
    extern std::vector<std::uint32_t> g_packClaimants;
    extern std::uint32_t g_eqWeapUiForm;
    extern std::uint32_t g_paArmorForm;
    extern std::unordered_map<int, bool> g_hkPrev;

    constexpr const char* INPUT_CAPTURING_MENUS[] = {
        "Console", "PauseMenu", "MainMenu", "PipboyMenu", "TerminalMenu",
        "ExamineMenu", "ExamineConfirmMenu", "ContainerMenu", "BarterMenu",
        "MessageBoxMenu", "LockpickingMenu", "FavoritesMenu", "SleepWaitMenu",
        "LevelUpMenu", "SPECIALMenu", "VATSMenu", "BookMenu", "LooksMenu",
        "LoadingMenu", "WorkshopMenu", "CookingMenu", "RobotModMenu",
        "PowerArmorModMenu", "CreditsMenu"
    };
    extern bool g_menuCapState[std::size(INPUT_CAPTURING_MENUS)];

    LadderTables PlayerTables();
    LadderTables NpcTablesFor(Slots::NpcTableSet& set);
    Npc::WearCtx PlayerWearCtx();
    Slots::Transform* WeaponAt(const LadderTables& tab, int slot, std::uint32_t weapForm, std::uint32_t ctx);
    Slots::Transform* ActiveTransform(int slot);
    Slots::Transform* ResolveLadder(int slot, std::uint32_t weapForm, const Npc::WearCtx& wear, bool inPA, std::uint32_t paArmorForm);
    Slots::Transform* ResolveNpcLadder(Slots::NpcTableSet& set, int slot, std::uint32_t weapForm, const Npc::WearCtx& wear, bool inPA, std::uint32_t paArmorForm);
    inline void Show(RE::NiAVObject* obj, bool on) {
        obj->SetAppCulled(!on);
        obj->fadeAmount = on ? 1.0f : 0.0f;
    }

    template <class F>
    void Walk(RE::NiAVObject* obj, const F& visit) {
        if (!obj || !visit(obj)) {
            return;
        }
        if (auto* node = obj->IsNode()) {
            for (const auto& child : node->GetRuntimeData().children) {
                Walk(child.get(), visit);
            }
        }
    }

    RE::NiAVObject* MakeDisplayClone(RE::NiAVObject* source, std::uint32_t formID);
    void RequestNpcHarvest(RE::TESBoundObject* obj, const RE::BSTSmartPointer<RE::ExtraDataList>& extra, std::uint32_t npcKey, int slot);
    const Slots::Transform* GroupAnchor(int slot);
    void ApplyTransforms(RE::NiAVObject* obj, const Slots::Transform* group, const Slots::Transform& m);
    std::uint64_t OmodHash(const RE::ExtraDataList* extra);
    void Schedule(std::function<void()> fn, int delayMs);
    std::string SlotNodeName(int slot);
    std::string SafeName(RE::TESForm* form);
    RE::NiAVObject* Player3D();
    void RetireSlot(RE::NiAVObject* player3D, int slot);
    void ReapDeadNodes(RE::NiAVObject* root, const char* name);
    void Reconcile();
    void RequestReconcile(int delayMs);
    void ReapplySlotTransform(int slot);
    void UpdateVisibilityAll();
    void ReattachCarrying(std::uint32_t formID);
    void ReattachAllDisplays();
    void OnPlayerDrawFlip(bool drawn);
    void OnButtonInput();
    std::uint32_t ReadPadButtons();
    const char* PadButtonName(std::uint32_t mask);
    const std::string& HotkeyName(int hk);
    void Hud(const std::string& msg, const char* sound);
    void OnPreLoadGame();
    void OnPostLoadGame();
}
