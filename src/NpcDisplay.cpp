#include "NpcDisplay.h"
#include "DisplayManager.h"
#include "Logger.h"
#include "SlotManager.h"

#include <atomic>
#include <deque>

namespace Npc
{
    //============= Weapon Families & Placement =============
    enum Family : int
    {
        LongGun = 0,
        Pistol,
        Melee,
        Heavy,
        TwoHand,
        Knife,
        FAMILY_COUNT
    };
    static constexpr const char* FAMILY_NAMES[FAMILY_COUNT] = { "Long Gun", "Pistol", "Melee", "Heavy", "2H Melee", "Knife" };
    static constexpr int SLOT_COUNT = 12;

    static constexpr int CLAIM_ORDER[FAMILY_COUNT] = { TwoHand, Heavy, LongGun, Pistol, Melee, Knife };

    static int FamSlotsFor(const Slots::NpcTableSet& set, int family, int (&out)[SLOT_COUNT]) {
        int n = 0;
        for (int s = 0; s < SLOT_COUNT; ++s) {
            if (set.slots[s].families & (1 << family)) {
                out[n++] = s;
            }
        }
        std::sort(out, out + n, [&set](int a, int b) {
            const int pa = set.slots[a].prio;
            const int pb = set.slots[b].prio;
            return pa != pb ? pa < pb : a < b;
        });
        return n;
    }

    //============= Config =============
    bool g_hideSleeping = true;

    static std::vector<std::string> g_raceSpecs = {
        "Fallout4.esm|13746",
        "Fallout4.esm|EAFB6",
        "Fallout4.esm|E8D09",
        "Fallout4.esm|10BD65",
        "Fallout4.esm|2261A4",
    };
    static std::vector<std::uint32_t> g_races;
    static std::vector<std::string> g_skeletons;
    static std::unordered_map<std::uint32_t, bool> g_raceVerdict;
    static bool g_racesResolved = false;

    //============= Actor Filters =============
    static std::uint32_t ConfigKeyOf(const RE::Actor* actor) {
        const auto* npc = actor->GetNPC();
        return npc && npc->IsUnique() ? npc->GetFormID() : actor->GetFormID();
    }

    static bool IsDead(const RE::Actor* actor) {
        return (0xA6u >> actor->lifeState) & 1;
    }

    static const char* SkeletonOf(std::uint32_t raceID) {
        const auto* race = RE::TESForm::GetFormByID<RE::TESRace>(raceID);
        const char* path = race ? race->skeletonModel[0].GetModel() : nullptr;
        return path && path[0] ? path : nullptr;
    }

    static bool RaceAllowed(std::uint32_t raceID) {
        if (!g_racesResolved) {
            g_races.clear();
            g_skeletons.clear();
            g_raceVerdict.clear();
            for (const auto& spec : g_raceSpecs) {
                if (const auto id = Slots::SpecToForm(spec); id) {
                    g_races.push_back(id);
                    if (const char* sk = SkeletonOf(id); sk && !Slots::NameInList(g_skeletons, sk)) {
                        g_skeletons.emplace_back(sk);
                    }
                }
            }
            g_racesResolved = true;
            if (g_races.empty()) {
                logger::warn("no NPC races resolved - NPC displays disabled");
            }
        }
        if (std::find(g_races.begin(), g_races.end(), raceID) != g_races.end()) {
            return true;
        }
        if (const auto it = g_raceVerdict.find(raceID); it != g_raceVerdict.end()) {
            return it->second;
        }
        const char* sk = SkeletonOf(raceID);
        const bool shared = sk && Slots::NameInList(g_skeletons, sk);
        g_raceVerdict[raceID] = shared;
        if (shared) {
            logger::info("race {:08X} accepted for NPC displays - shares skeleton {}", raceID, sk);
        }
        return shared;
    }

    //============= Weapon Classification =============
    struct Keywords
    {
        RE::BGSKeyword* pistol{ nullptr };
        RE::BGSKeyword* rifle{ nullptr };
        RE::BGSKeyword* shotgun{ nullptr };
        RE::BGSKeyword* heavy{ nullptr };
        RE::BGSKeyword* melee1H{ nullptr };
        RE::BGSKeyword* melee2H{ nullptr };
        bool resolved{ false };
    };
    static Keywords g_kw;

    static void ResolveKeywords() {
        if (g_kw.resolved) {
            return;
        }
        g_kw.pistol = RE::TESForm::GetFormByID<RE::BGSKeyword>(0x0004A0A0);
        g_kw.rifle = RE::TESForm::GetFormByID<RE::BGSKeyword>(0x0004A0A1);
        g_kw.shotgun = RE::TESForm::GetFormByID<RE::BGSKeyword>(0x00226454);
        g_kw.heavy = RE::TESForm::GetFormByID<RE::BGSKeyword>(0x0004A0A3);
        g_kw.melee1H = RE::TESForm::GetFormByID<RE::BGSKeyword>(0x0004A0A4);
        g_kw.melee2H = RE::TESForm::GetFormByID<RE::BGSKeyword>(0x0004A0A5);
        g_kw.resolved = true;
        if (!g_kw.pistol || !g_kw.rifle || !g_kw.heavy || !g_kw.melee1H) {
            logger::warn("weapon-type keywords unresolved - NPC weapon classification degraded");
        }
    }

    static const RE::TBO_InstanceData* InstDataOf(const RE::ExtraDataList* extra) {
        const auto* x = extra ? extra->GetByType<RE::ExtraInstanceData>() : nullptr;
        return x ? x->data.get() : nullptr;
    }

    static int FamilyOf(RE::TESObjectWEAP* weap, const RE::TBO_InstanceData* inst = nullptr) {
        if (!weap) {
            return -1;
        }
        ResolveKeywords();
        const auto has = [&](RE::BGSKeyword* kw) {
            return kw && weap->HasKeyword(kw, inst);
        };
        if (has(g_kw.heavy)) {
            return Heavy;
        }
        if (has(g_kw.pistol)) {
            return Pistol;
        }
        if (has(g_kw.rifle) || has(g_kw.shotgun)) {
            return LongGun;
        }
        if (has(g_kw.melee2H)) {
            return TwoHand;
        }
        if (has(g_kw.melee1H)) {
            return weap->weaponData.type.get() == RE::WEAPON_TYPE::kOneHandDagger ? Knife : Melee;
        }
        return -1;
    }

    //============= Actor Registry =============
    struct SlotItem
    {
        std::uint32_t formID{ 0 };
        std::uint64_t omodHash{ 0 };
        bool pending{ false };
        bool equippedDisplay{ false };
    };
    struct NpcState
    {
        RE::ActorHandle handle;
        std::uint32_t cfgKey{ 0 };
        SlotItem slot[SLOT_COUNT];
        std::map<int, SlotItem> custom;
        WearCtx wear;
        bool inBed{ false };
        bool weaponDrawn{ false };
        bool situationalHidden{ false };
        std::uint64_t gen{ 0 };
    };
    static std::unordered_map<std::uint32_t, NpcState> g_npcs;
    static std::uint64_t g_gen = 0;
    static std::atomic<bool> g_dirty = false;
    static std::set<std::pair<std::uint32_t, std::uint64_t>> g_inflight;

    static std::map<std::pair<std::uint32_t, std::uint64_t>, RE::NiPointer<RE::NiAVObject>> g_modelCache;
    static std::deque<std::pair<std::uint32_t, std::uint64_t>> g_cacheOrder;
    static constexpr std::size_t CACHE_CAP = 64;
    void MarkDirty() { g_dirty = true; }

    bool ConsumeDirty() { return g_dirty.exchange(false); }

    //============= Display Nodes =============
    static std::string NodeName(std::uint32_t key, int slot) {
        return fmt::format("VisFavNpc_{:X}_s{}", key, slot);
    }

    static void RetireSlot(RE::NiAVObject* actor3D, std::uint32_t key, int slot) {
        const auto nm = NodeName(key, slot);
        while (auto* old = actor3D->GetObjectByName(nm.c_str())) {
            Display::Show(old, false);
            old->name = "VisFavNpc_Dead";
        }
    }

    static SlotItem& ItemAt(NpcState& st, int slot) {
        return slot < SLOT_COUNT ? st.slot[slot] : st.custom[slot];
    }

    static const SlotItem& ItemAt(const NpcState& st, int slot) {
        static const SlotItem empty;
        if (slot < SLOT_COUNT) {
            return st.slot[slot];
        }
        const auto it = st.custom.find(slot);
        return it != st.custom.end() ? it->second : empty;
    }

    static void ForEachLiveSlot(const NpcState& st, auto fn) {
        for (int s = 0; s < SLOT_COUNT; ++s) {
            if (st.slot[s].formID) {
                fn(s);
            }
        }
        for (const auto& [slot, si] : st.custom) {
            if (si.formID) {
                fn(slot);
            }
        }
    }

    struct CustomDef
    {
        int slot{ -1 };
        std::uint32_t formID{ 0 };
        bool hideWhenEquipped{ true };
        bool hideNotInInventory{ true };
        int groupVSlot{ -1 };
    };

    static CustomDef CustomSlotAt(int index) {
        CustomDef d;
        if (index < 0 || index >= Slots::CustomCount()) {
            return d;
        }
        const auto& c = Slots::g_custom[index];
        if (!c.showOnNpc) {
            return d;
        }
        const auto id = Slots::SpecToForm(c.itemSpec);
        if (!id) {
            return d;
        }
        d.slot = Slots::FAV_SLOTS + index;
        d.formID = id;
        d.hideWhenEquipped = c.hideWhenEquipped;
        d.hideNotInInventory = c.hideNotInInventory;
        d.groupVSlot = Slots::GroupVSlot(c.group);
        return d;
    }

    static const char* SlotBone(int slot, std::uint32_t cfgKey) {
        if (slot < 0 || slot >= Slots::MAX_SLOTS) {
            return nullptr;
        }
        const auto& bone = slot < Slots::FAV_SLOTS ? Slots::NpcSetFor(cfgKey).slots[slot].bone :
                                                     Slots::g_slots[slot].bone;
        return bone.empty() ? nullptr : bone.c_str();
    }

    static const Slots::Transform* ResolveCell(const NpcState& st, int slot, std::uint32_t weapForm) {
        return slot < Slots::FAV_SLOTS ?
                   Display::ResolveNpcLadder(Slots::NpcSetFor(st.cfgKey), slot, weapForm, st.wear, false, 0) :
                   Display::ResolveLadder(slot, weapForm, st.wear, false, 0);
    }

    static int GroupVSlotFor(int slot) {
        if (slot < SLOT_COUNT) {
            return -1;
        }
        for (int i = 0, cc = Slots::CustomCount(); i < cc; ++i) {
            if (const auto d = CustomSlotAt(i); d.slot == slot) {
                return d.groupVSlot;
            }
        }
        return -1;
    }

    static void ResolveAndApply(RE::NiAVObject* node, const NpcState& st, int slot,
        int groupVSlot, const Slots::Transform* editCell = nullptr, const Slots::Transform* editWork = nullptr) {
        const auto* cell = ResolveCell(st, slot, ItemAt(st, slot).formID);
        const Slots::Transform t = editCell && editWork && cell == editCell ? *editWork : *cell;
        Slots::Transform group{};
        const bool hasGroup = groupVSlot >= 0;
        if (hasGroup) {
            const auto* gcell = ResolveCell(st, groupVSlot, 0);
            group = editCell && editWork && gcell == editCell ? *editWork : *gcell;
        }
        Display::ApplyTransforms(node, hasGroup ? &group : nullptr, t);
        const bool visible = !t.hidden && !(hasGroup && group.hidden) && !st.situationalHidden &&
                             !(ItemAt(st, slot).equippedDisplay && st.weaponDrawn);
        Display::Show(node, visible);
    }

    static bool AttachSlot(RE::Actor* actor, NpcState& st, int slot,
        RE::NiAVObject* source, int groupVSlot) {
        auto* actor3D = actor->Get3D(false);
        if (!actor3D) {
            return false;
        }
        const char* boneName = SlotBone(slot, st.cfgKey);
        if (!boneName) {
            return false;
        }
        RetireSlot(actor3D, actor->GetFormID(), slot);
        auto* boneObj = actor3D->GetObjectByName(boneName);
        auto* bone = boneObj ? boneObj->IsNode() : nullptr;
        if (!bone) {
            VF_VLOG("npc {:08X}: bone '{}' missing - slot {} stays empty",
                actor->GetFormID(), boneName, slot);
            return false;
        }
        auto& si = ItemAt(st, slot);
        auto* clone = source ? Display::MakeDisplayClone(source, si.formID) : nullptr;
        if (!clone) {
            return false;
        }
        clone->name = NodeName(actor->GetFormID(), slot).c_str();
        ResolveAndApply(clone, st, slot, groupVSlot);
        bone->AttachChild(clone, true);
        VF_VLOG("npc {:08X}: slot {} display attached ({:08X})",
            actor->GetFormID(), slot, si.formID);
        return true;
    }

    void OnHarvestModel(std::uint32_t npcKey, int slot, RE::NiAVObject* model,
        std::uint32_t formID, std::uint64_t omodHash) {
        std::lock_guard lock(Display::g_tablesMutex);
        g_inflight.erase({ formID, omodHash });
        const auto it = g_npcs.find(npcKey);
        if (!model) {
            if (it != g_npcs.end() && slot >= 0) {
                ItemAt(it->second, slot) = SlotItem{};
            }
            return;
        }
        if (g_modelCache.emplace(std::pair{ formID, omodHash }, RE::NiPointer<RE::NiAVObject>(model)).second) {
            g_cacheOrder.push_back({ formID, omodHash });
            while (g_cacheOrder.size() > CACHE_CAP) {
                g_modelCache.erase(g_cacheOrder.front());
                g_cacheOrder.pop_front();
            }
        } else {
            g_modelCache[{ formID, omodHash }] = RE::NiPointer<RE::NiAVObject>(model);
        }
        MarkDirty();
        if (it == g_npcs.end() || slot < 0) {
            return;
        }
        auto& st = it->second;
        auto& si = ItemAt(st, slot);
        si.pending = false;
        if (si.formID != formID || si.omodHash != omodHash) {
            return;
        }
        if (RE::NiPointer<RE::Actor> actor = st.handle.get(); actor) {
            AttachSlot(actor.get(), st, slot, model, GroupVSlotFor(slot));
        }
    }

    //============= Inventory Scan =============
    struct Cand
    {
        std::uint32_t formID{ 0 };
        RE::BSTSmartPointer<RE::ExtraDataList> extra;
        std::uint32_t value{ 0 };
    };
    struct CustomHit
    {
        bool carried{ false };
        bool equipped{ false };
        RE::BSTSmartPointer<RE::ExtraDataList> extra;
    };
    struct Picks
    {
        WearCtx wear;
        Cand equipped;
        int equippedFam{ -1 };
        std::vector<Cand> fam[FAMILY_COUNT];
        std::map<std::uint32_t, CustomHit> customHits;
    };

    static void ScanActor(RE::Actor* actor, const NpcState& prev,
        const std::set<std::uint32_t>& customForms, bool equippedOnly, Picks& out) {
        std::set<std::uint32_t> equippedNow;
        if (auto* proc = actor->currentProcess; proc && proc->middleHigh) {
            for (const auto& ei : proc->middleHigh->equippedItems) {
                if (ei.item.object) {
                    equippedNow.insert(ei.item.object->GetFormID());
                }
            }
        }
        const auto packMask = Slots::BackpackSlotMask();
        actor->inventoryList->ForEachStack(
            [](RE::BGSInventoryItem&) { return true; },
            [&](RE::BGSInventoryItem& item, RE::BGSInventoryItem::Stack& stack) {
                if (!item.object) {
                    return true;
                }
                const auto itemID = item.object->GetFormID();
                const bool equipped = stack.IsEquipped() || equippedNow.contains(itemID);
                const bool blacklisted = Slots::IsDisplayBlacklisted(itemID);
                if (customForms.contains(itemID) && !blacklisted) {
                    auto& h = out.customHits[itemID];
                    h.carried = true;
                    if (equipped) {
                        h.equipped = true;
                        h.extra = stack.extra;
                    } else if (!h.extra) {
                        h.extra = stack.extra;
                    }
                }
                if (auto* weap = item.object->As<RE::TESObjectWEAP>(); weap) {
                    if (blacklisted) {
                        return true;
                    }
                    if (equipped) {
                        if (out.equippedFam < 0) {
                            const int fam = FamilyOf(weap, InstDataOf(stack.extra.get()));
                            if (fam >= 0) {
                                out.equippedFam = fam;
                                out.equipped.formID = itemID;
                                out.equipped.extra = stack.extra;
                            }
                        }
                        return true;
                    }
                    if (equippedOnly) {
                        return true;
                    }
                    const auto* inst = InstDataOf(stack.extra.get());
                    const int fam = FamilyOf(weap, inst);
                    if (fam < 0) {
                        return true;
                    }
                    auto& v = out.fam[fam];
                    for (const auto& c : v) {
                        if (c.formID == itemID) {
                            return true;
                        }
                    }
                    Cand c;
                    c.formID = itemID;
                    c.extra = stack.extra;
                    c.value = inst ? static_cast<const RE::TESObjectWEAP::InstanceData*>(inst)->value :
                                     weap->weaponData.value;
                    v.push_back(std::move(c));
                    return true;
                }
                if (!equipped) {
                    return true;
                }
                if (const auto* armo = item.object->As<RE::TESObjectARMO>(); armo) {
                    const auto slots = armo->bipedModelData.bipedObjectSlots;
                    const auto sticky = [&](std::uint32_t& slotRef, std::uint32_t prevVal) {
                        if (!slotRef || itemID == prevVal) {
                            slotRef = itemID;
                        }
                    };
                    if (slots & (1u << 3)) {
                        sticky(out.wear.armor, prev.wear.armor);
                    } else {
                        if ((slots & packMask) && !Slots::IsPackBlacklisted(itemID)) {
                            sticky(out.wear.pack, prev.wear.pack);
                        }
                        if (slots & (1u << 11)) {
                            sticky(out.wear.overTorso, prev.wear.overTorso);
                        }
                        if (slots & (1u << 14)) {
                            sticky(out.wear.overLLeg, prev.wear.overLLeg);
                        }
                        if (slots & (1u << 15)) {
                            sticky(out.wear.overRLeg, prev.wear.overRLeg);
                        }
                    }
                }
                return true;
            });
        for (int f = 0; f < FAMILY_COUNT; ++f) {
            auto& v = out.fam[f];
            std::sort(v.begin(), v.end(), [](const Cand& a, const Cand& b) {
                return a.value != b.value ? a.value > b.value : a.formID < b.formID;
            });
            if (!v.empty() && Slots::g_verboseLog) {
                std::string line;
                for (const auto& c : v) {
                    line += fmt::format("{}{:08X}(v{})", line.empty() ? "" : ", ", c.formID, c.value);
                }
                logger::info("npc {:08X}: {} candidates by value: {}", actor->GetFormID(), FAMILY_NAMES[f], line);
            }
        }
    }

    //============= Slot Assignment =============
    static void AssignSlots(const Slots::NpcTableSet& set, Picks& picks, Cand* (&want)[SLOT_COUNT],
        int& equippedSlot) {
        for (int s = 0; s < SLOT_COUNT; ++s) {
            want[s] = nullptr;
        }
        equippedSlot = -1;
        if (picks.equippedFam >= 0) {
            int famSlots[SLOT_COUNT];
            if (FamSlotsFor(set, picks.equippedFam, famSlots) > 0) {
                want[famSlots[0]] = &picks.equipped;
                equippedSlot = famSlots[0];
            }
        }
        std::vector<std::pair<int, Cand*>> merged;
        for (int f = 0; f < FAMILY_COUNT; ++f) {
            for (auto& c : picks.fam[f]) {
                merged.emplace_back(f, &c);
            }
        }
        std::sort(merged.begin(), merged.end(), [](const auto& a, const auto& b) {
            return a.second->value != b.second->value ? a.second->value > b.second->value :
                                                        a.second->formID < b.second->formID;
        });
        for (auto& [f, c] : merged) {
            int famSlots[SLOT_COUNT];
            const int famCount = FamSlotsFor(set, f, famSlots);
            for (int fi = 0; fi < famCount; ++fi) {
                const int s = famSlots[fi];
                if (!want[s]) {
                    want[s] = c;
                    break;
                }
            }
        }
    }

    //============= Reconcile =============
    static bool SituationalHidden(const NpcState& st) {
        if (g_hideSleeping && st.inBed) {
            return true;
        }
        if (Slots::g_hideWhenNoBodyArmor && !st.wear.armor) {
            return true;
        }
        return false;
    }

    static void ResyncInBed(RE::Actor* actor, NpcState& st) {
        const auto s = static_cast<std::uint32_t>(actor->DoGetSitSleepState());
        if (s >= 5 && s <= 7) {
            st.inBed = true;
        } else if (s == 0) {
            st.inBed = false;
        }
    }

    static void RetireActor(NpcState& st, std::uint32_t key) {
        RE::NiPointer<RE::Actor> actor = st.handle.get();
        auto* actor3D = actor ? actor->Get3D(false) : nullptr;
        if (!actor3D) {
            return;
        }
        ForEachLiveSlot(st, [&](int slot) { RetireSlot(actor3D, key, slot); });
    }

    void Reset() {
        std::lock_guard lock(Display::g_tablesMutex);
        g_npcs.clear();
        g_inflight.clear();
        g_modelCache.clear();
        g_cacheOrder.clear();
        g_dirty = false;
    }

    static void ReconcileOne(RE::Actor* actor, NpcState& st, int slot,
        std::uint32_t wantForm, std::uint64_t wantHash,
        const RE::BSTSmartPointer<RE::ExtraDataList>& extra,
        bool ctxChanged, int groupVSlot, bool equipped = false) {
        auto* actor3D = actor->Get3D(false);
        const auto key = actor->GetFormID();
        auto& si = ItemAt(st, slot);
        const bool changed = wantForm != si.formID || (wantForm && wantHash != si.omodHash);
        if (!changed) {
            if (si.formID && !si.pending) {
                if (si.equippedDisplay != equipped) {
                    si.equippedDisplay = equipped;
                    ctxChanged = true;
                }
                if (!actor3D->GetObjectByName(NodeName(key, slot).c_str())) {
                    if (const auto it = g_modelCache.find({ si.formID, si.omodHash });
                        it != g_modelCache.end()) {
                        AttachSlot(actor, st, slot, it->second.get(), groupVSlot);
                    } else {
                        si = SlotItem{};
                    }
                } else if (ctxChanged) {
                    if (auto* node = actor3D->GetObjectByName(NodeName(key, slot).c_str())) {
                        ResolveAndApply(node, st, slot, groupVSlot);
                    }
                }
            }
            return;
        }
        if (si.pending) {
            return;
        }
        if (si.formID) {
            VF_VLOG("npc {:08X}: slot {} retired ({:08X} -> {:08X})",
                key, slot, si.formID, wantForm);
        }
        RetireSlot(actor3D, key, slot);
        si = SlotItem{};
        if (!wantForm) {
            return;
        }
        si.formID = wantForm;
        si.omodHash = wantHash;
        si.equippedDisplay = equipped;
        if (const auto it = g_modelCache.find({ si.formID, si.omodHash }); it != g_modelCache.end()) {
            AttachSlot(actor, st, slot, it->second.get(), groupVSlot);
        } else if (g_inflight.contains({ si.formID, si.omodHash })) {
            si = SlotItem{};
        } else {
            si.pending = true;
            auto* form = RE::TESForm::GetFormByID<RE::TESBoundObject>(wantForm);
            if (form) {
                g_inflight.insert({ si.formID, si.omodHash });
                VF_VLOG("npc {:08X}: harvesting {:08X} for slot {}", key, wantForm, slot);
                Display::RequestNpcHarvest(form, extra, key, slot);
            } else {
                si = SlotItem{};
            }
        }
    }

    static void ReconcileCorpse(RE::Actor* actor, NpcState& st,
        const std::vector<CustomDef>& customDefs) {
        const auto retireable = [&](int slot) {
            if (slot < SLOT_COUNT) {
                return true;
            }
            for (const auto& d : customDefs) {
                if (d.slot == slot) {
                    return d.hideNotInInventory;
                }
            }
            return true;
        };
        std::set<std::uint32_t> shown;
        for (int s = 0; s < SLOT_COUNT; ++s) {
            if (st.slot[s].formID) {
                shown.insert(st.slot[s].formID);
            }
        }
        for (const auto& [slot, si] : st.custom) {
            if (si.formID && retireable(slot)) {
                shown.insert(si.formID);
            }
        }
        if (shown.empty()) {
            return;
        }
        std::set<std::uint32_t> present;
        actor->inventoryList->ForEachStack(
            [&](RE::BGSInventoryItem& item) {
                return item.object && shown.contains(item.object->GetFormID());
            },
            [&](RE::BGSInventoryItem& item, RE::BGSInventoryItem::Stack&) {
                present.insert(item.object->GetFormID());
                return true;
            });
        if (present.size() == shown.size()) {
            return;
        }
        auto* actor3D = actor->Get3D(false);
        const auto key = actor->GetFormID();
        const auto sweep = [&](int slot, SlotItem& si) {
            if (si.formID && retireable(slot) && !present.contains(si.formID)) {
                VF_VLOG("npc {:08X}: slot {} retired - {:08X} looted off the corpse",
                    key, slot, si.formID);
                RetireSlot(actor3D, key, slot);
                si = SlotItem{};
            }
        };
        for (int s = 0; s < SLOT_COUNT; ++s) {
            sweep(s, st.slot[s]);
        }
        for (auto& [slot, si] : st.custom) {
            sweep(slot, si);
        }
    }

    void Reconcile() {
        std::lock_guard lock(Display::g_tablesMutex);
        if (!Slots::AnyNpcDisplays()) {
            if (!g_npcs.empty()) {
                for (auto& [key, st] : g_npcs) {
                    RetireActor(st, key);
                }
                g_npcs.clear();
            }
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }
        auto* pl = RE::ProcessLists::GetSingleton();
        if (!pl) {
            return;
        }
        const auto& handles = pl->highActorHandles;
        std::vector<CustomDef> customDefs;
        std::set<std::uint32_t> customForms;
        for (int i = 0, cc = Slots::CustomCount(); i < cc; ++i) {
            if (const auto d = CustomSlotAt(i); d.slot >= 0) {
                customDefs.push_back(d);
                customForms.insert(d.formID);
            }
        }
        ++g_gen;
        for (const auto& h : handles) {
            RE::NiPointer<RE::Actor> actorPtr = h.get();
            auto* actor = actorPtr.get();
            if (!actor || actor == player || !actor->Get3D(false) || !actor->inventoryList) {
                if (actor) {
                    if (const auto it = g_npcs.find(actor->GetFormID()); it != g_npcs.end()) {
                        it->second.gen = g_gen;
                    }
                }
                continue;
            }
            const auto raceID = actor->race ? actor->race->GetFormID() : 0u;
            if (!RaceAllowed(raceID)) {
                continue;
            }
            Display::ReapDeadNodes(actor->Get3D(false), "VisFavNpc_Dead");
            const auto cfgKey = ConfigKeyOf(actor);
            const int actorMode = Slots::NpcModeFor(cfgKey);
            if (actorMode == 2) {
                continue;
            }
            if (IsDead(actor)) {
                if (const auto dit = g_npcs.find(actor->GetFormID()); dit != g_npcs.end()) {
                    dit->second.gen = g_gen;
                    ReconcileCorpse(actor, dit->second, customDefs);
                }
                continue;
            }
            const auto key = actor->GetFormID();
            auto& st = g_npcs[key];
            st.handle = h;
            st.gen = g_gen;
            st.cfgKey = cfgKey;
            const auto& aset = Slots::NpcSetFor(cfgKey);

            Picks picks;
            ScanActor(actor, st, customForms, actorMode == 1, picks);
            bool ctxChanged = picks.wear != st.wear;
            st.wear = picks.wear;
            ResyncInBed(actor, st);
            const bool drawnNow = actor->GetWeaponMagicDrawn();
            ctxChanged = ctxChanged || drawnNow != st.weaponDrawn;
            st.weaponDrawn = drawnNow;
            const bool hid = SituationalHidden(st);
            ctxChanged = ctxChanged || hid != st.situationalHidden;
            st.situationalHidden = hid;

            Cand* want[SLOT_COUNT];
            int eqSlot = -1;
            AssignSlots(aset, picks, want, eqSlot);
            static const RE::BSTSmartPointer<RE::ExtraDataList> noExtra;
            for (int s = 0; s < SLOT_COUNT; ++s) {
                if (aset.slots[s].families == 0) {
                    if (st.slot[s].formID) {
                        ReconcileOne(actor, st, s, 0, 0, noExtra, false, -1);
                    }
                    continue;
                }
                const Cand* w = want[s];
                ReconcileOne(actor, st, s,
                    w ? w->formID : 0u,
                    w ? Display::OmodHash(w->extra.get()) : 0,
                    w ? w->extra : noExtra, ctxChanged, -1, s == eqSlot);
            }
            for (const auto& d : customDefs) {
                const auto hit = picks.customHits.find(d.formID);
                const bool carried = hit != picks.customHits.end() && hit->second.carried;
                const bool eqHidden = carried && d.hideWhenEquipped && hit->second.equipped;
                const bool show = carried && !eqHidden;
                ReconcileOne(actor, st, d.slot,
                    show ? d.formID : 0u,
                    show ? Display::OmodHash(hit->second.extra.get()) : 0,
                    show ? hit->second.extra : noExtra, ctxChanged, d.groupVSlot);
            }
            for (auto& [slot, si] : st.custom) {
                if (!si.formID) {
                    continue;
                }
                bool stillWanted = false;
                for (const auto& d : customDefs) {
                    stillWanted = stillWanted || d.slot == slot;
                }
                if (!stillWanted) {
                    ReconcileOne(actor, st, slot, 0, 0, noExtra, false, -1);
                }
            }
        }
        for (auto it = g_npcs.begin(); it != g_npcs.end();) {
            if (it->second.gen != g_gen) {
                RetireActor(it->second, it->first);
                it = g_npcs.erase(it);
            } else {
                ++it;
            }
        }
    }

    static void RefreshActorDisplays(RE::NiAVObject* actor3D, std::uint32_t key, const NpcState& st) {
        ForEachLiveSlot(st, [&](int slot) {
            if (auto* node = actor3D->GetObjectByName(NodeName(key, slot).c_str())) {
                ResolveAndApply(node, st, slot, GroupVSlotFor(slot));
            }
        });
    }

    static NpcState* FlipTarget(std::uint32_t actorID, RE::NiAVObject*& actor3D) {
        const auto it = g_npcs.find(actorID);
        if (it == g_npcs.end()) {
            return nullptr;
        }
        RE::NiPointer<RE::Actor> actor = it->second.handle.get();
        actor3D = actor ? actor->Get3D(false) : nullptr;
        if (!actor3D || IsDead(actor.get())) {
            return nullptr;
        }
        return &it->second;
    }

    void OnFurnitureFlip(std::uint32_t actorID, bool enter) {
        std::lock_guard lock(Display::g_tablesMutex);
        RE::NiAVObject* actor3D = nullptr;
        auto* st = FlipTarget(actorID, actor3D);
        if (!st) {
            return;
        }
        st->inBed = enter;
        const bool hid = SituationalHidden(*st);
        if (hid == st->situationalHidden) {
            return;
        }
        st->situationalHidden = hid;
        RefreshActorDisplays(actor3D, actorID, *st);
    }

    void OnDrawFlip(std::uint32_t actorID, bool drawn) {
        std::lock_guard lock(Display::g_tablesMutex);
        RE::NiAVObject* actor3D = nullptr;
        auto* st = FlipTarget(actorID, actor3D);
        if (!st || st->weaponDrawn == drawn) {
            return;
        }
        st->weaponDrawn = drawn;
        VF_VLOG("npc {:08X}: weapon {}", actorID, drawn ? "DRAWN" : "sheathed");
        RefreshActorDisplays(actor3D, actorID, *st);
    }

    //============= Panel Access =============
    bool& HideSleepingRef() { return g_hideSleeping; }

    int FamilyOfWeapon(RE::TESObjectWEAP* weap, const RE::ExtraDataList* extra) {
        return FamilyOf(weap, InstDataOf(extra));
    }

    static constexpr int PLAYER_EQUIP_SLOTS[FAMILY_COUNT][3] = {
        { 8, 4, 1 }, { 3, -1, -1 }, { 5, -1, -1 }, { 8, -1, -1 }, { 8, 4, -1 }, { 10, -1, -1 }
    };

    int PlayerEquipSlot(int family, int after) {
        if (family < 0 || family >= FAMILY_COUNT) {
            return -1;
        }
        const auto& chain = PLAYER_EQUIP_SLOTS[family];
        for (int i = 0; i < 3 && chain[i] >= 0; ++i) {
            if (chain[i] == after) {
                return i + 1 < 3 ? chain[i + 1] : -1;
            }
        }
        return chain[0];
    }

    const char* FamilyLabel(int family) {
        return family >= 0 && family < FAMILY_COUNT ? FAMILY_NAMES[family] : "Disabled";
    }

    int FamilyCount() { return FAMILY_COUNT; }

    static std::vector<TargetInfo> g_targetCache;
    static std::uint32_t g_targetSuggest = 0;
    static std::uint32_t g_targetGen = 0;

    void RefreshTargets() {
        std::lock_guard lock(Display::g_tablesMutex);
        g_targetCache.clear();
        g_targetSuggest = 0;
        ++g_targetGen;
        auto* player = RE::PlayerCharacter::GetSingleton();
        float bestDist = 0.0f;
        for (auto& [key, st] : g_npcs) {
            RE::NiPointer<RE::Actor> a = st.handle.get();
            if (!a) {
                continue;
            }
            TargetInfo ti;
            ti.id = key;
            ti.wear = st.wear;
            ti.cfgKey = ConfigKeyOf(a.get());
            if (const char* nm = a->GetDisplayFullName(); nm && nm[0]) {
                ti.name = nm;
            }
            if (ti.name.empty()) {
                ti.name = fmt::format("NPC {:08X}", key);
            }
            g_targetCache.push_back(std::move(ti));
            if (player) {
                const float d2 = (a->GetPosition() - player->GetPosition()).SqrLength();
                if (!g_targetSuggest || d2 < bestDist) {
                    bestDist = d2;
                    g_targetSuggest = key;
                }
            }
        }
        std::sort(g_targetCache.begin(), g_targetCache.end(),
            [](const TargetInfo& a, const TargetInfo& b) { return a.name < b.name; });
    }

    const std::vector<TargetInfo>& CachedTargets() { return g_targetCache; }

    std::uint32_t SuggestedTarget() { return g_targetSuggest; }

    std::uint32_t TargetGen() { return g_targetGen; }

    std::uint32_t SlotItemOf(std::uint32_t npcID, int slot) {
        std::lock_guard lock(Display::g_tablesMutex);
        if (slot < 0 || slot >= SLOT_COUNT) {
            return 0;
        }
        const auto it = g_npcs.find(npcID);
        return it != g_npcs.end() ? it->second.slot[slot].formID : 0;
    }

    RE::NiAVObject* DisplayNode(std::uint32_t npcID, int slot) {
        std::lock_guard lock(Display::g_tablesMutex);
        const auto it = g_npcs.find(npcID);
        RE::NiPointer<RE::Actor> actor = it != g_npcs.end() ? it->second.handle.get() : nullptr;
        auto* actor3D = actor ? actor->Get3D(false) : nullptr;
        return actor3D ? actor3D->GetObjectByName(NodeName(npcID, slot).c_str()) : nullptr;
    }

    int FamilyRank(int family) {
        for (int i = 0; i < FAMILY_COUNT; ++i) {
            if (CLAIM_ORDER[i] == family) {
                return i;
            }
        }
        return FAMILY_COUNT;
    }

    void RefreshAll() {
        std::lock_guard lock(Display::g_tablesMutex);
        for (auto& [key, st] : g_npcs) {
            RetireActor(st, key);
        }
        g_npcs.clear();
        MarkDirty();
    }

    void RefreshCarrying(std::uint32_t formID) {
        std::lock_guard lock(Display::g_tablesMutex);
        for (auto it = g_npcs.begin(); it != g_npcs.end();) {
            bool carries = false;
            ForEachLiveSlot(it->second, [&](int slot) { carries = carries || ItemAt(it->second, slot).formID == formID; });
            if (!carries) {
                ++it;
                continue;
            }
            RetireActor(it->second, it->first);
            it = g_npcs.erase(it);
            MarkDirty();
        }
    }

    //============= Transform Reapply =============
    static void ReapplyAll(const Slots::Transform* editCell, const Slots::Transform* editWork) {
        std::lock_guard lock(Display::g_tablesMutex);
        int applied = 0;
        for (auto& [key, st] : g_npcs) {
            RE::NiPointer<RE::Actor> actor = st.handle.get();
            auto* actor3D = actor ? actor->Get3D(false) : nullptr;
            if (!actor3D) {
                continue;
            }
            ForEachLiveSlot(st, [&](int slot) {
                if (auto* node = actor3D->GetObjectByName(NodeName(key, slot).c_str())) {
                    ResolveAndApply(node, st, slot, GroupVSlotFor(slot), editCell, editWork);
                    ++applied;
                }
            });
        }
        if (!editCell) {
            VF_VLOG("npc reapply: {} displays re-resolved", applied);
        }
    }

    void ReapplyTransforms() {
        ReapplyAll(nullptr, nullptr);
    }

    void PreviewTransforms(const Slots::Transform* cell, const Slots::Transform& work) {
        Display::Schedule([cell, w = work]() { ReapplyAll(cell, &w); }, 0);
    }

    //============= INI =============
    void WriteIni(std::ostream& out) {
        out << "\n[NPC]\n";
        out << "iNPCDisplay=" << Slots::g_npcDisplayMode << "\n";
        out << "bNPCHideSleeping=" << (g_hideSleeping ? 1 : 0) << "\n";
        out << "NPCRaces=";
        for (std::size_t i = 0; i < g_raceSpecs.size(); ++i) {
            out << (i ? "," : "") << g_raceSpecs[i];
        }
        out << "\n";
    }

    bool TryLoadKey(const std::string& key, const std::string& val) {
        if (key == "iNPCDisplay") {
            const int m = std::atoi(val.c_str());
            Slots::g_npcDisplayMode = m >= 0 && m <= 2 ? m : 0;
        } else if (key == "bNPCHideSleeping") {
            g_hideSleeping = std::atoi(val.c_str()) != 0;
        } else if (key == "NPCRaces") {
            g_raceSpecs = Slots::SplitCsv(val);
            g_racesResolved = false;
        } else {
            return false;
        }
        return true;
    }
}
