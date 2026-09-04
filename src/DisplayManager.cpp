#include "DisplayManager.h"

#include "Config.h"
#include "EngineCalls.h"
#include "EventSinks.h"
#include "InputHandler.h"
#include "Logger.h"
#include "NpcDisplay.h"
#include "Panel.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace Display
{
    using Slots::MAX_SLOTS;

    static bool AttachSlotClone(int slot);
    static void SetCloneVisible(int slot, bool visible);

    //============= State & Context =============

    SlotState g_state[Slots::MAX_INDEX];

    std::recursive_mutex g_tablesMutex;
    std::atomic<bool> g_dirty = true;
    bool g_hideAll = false;

    bool g_menuCapState[std::size(INPUT_CAPTURING_MENUS)] = {};

    bool HotkeysBlocked() {
        for (const bool open : g_menuCapState) {
            if (open) {
                return true;
            }
        }
        const auto* ui = RE::UI::GetSingleton();
        return ui && ui->menuMode > 0;
    }

    bool g_noBodyArmor = false;
    bool g_inPA = false;
    bool g_playerInBed = false;

    std::uint32_t g_packForm = 0;
    std::vector<std::uint32_t> g_packClaimants;
    std::uint32_t g_eqWeapUiForm = 0;
    std::uint32_t g_armorForm = 0;
    std::uint32_t g_paArmorForm = 0;
    std::uint32_t g_overTorsoForm = 0;
    std::uint32_t g_overLLegForm = 0;
    std::uint32_t g_overRLegForm = 0;

    Npc::WearCtx PlayerWearCtx() {
        return { g_armorForm, g_packForm, g_overTorsoForm, g_overLLegForm, g_overRLegForm };
    }

    static std::uint32_t OverFor(const Npc::WearCtx& wear, int slot) {
        switch (slot) {
        case 2:
        case 11:
            return wear.overLLeg;
        case 3:
        case 10:
            return wear.overRLeg;
        default:
            return wear.overTorso;
        }
    }

    //============= Position Ladder =============
    LadderTables PlayerTables() {
        return { Slots::g_player, Slots::g_slotsPA, nullptr };
    }

    LadderTables NpcTablesFor(Slots::NpcTableSet& set) {
        return { set, set.slotsPA, &set };
    }

    Slots::Transform* WeaponAt(const LadderTables& tab, int slot, std::uint32_t weapForm, std::uint32_t ctx) {
        if (weapForm && ctx) {
            if (auto wit = tab.set.weaponOverrides.find(weapForm); wit != tab.set.weaponOverrides.end()) {
                if (auto it = wit->second.find({ slot, ctx }); it != wit->second.end()) {
                    return &it->second;
                }
            }
        }
        return nullptr;
    }

    static Slots::Transform* ResolveLadderIn(const LadderTables& tab, int slot,
        std::uint32_t weapForm, const Npc::WearCtx& wear, bool inPA, std::uint32_t paArmorForm) {
        if (inPA) {
            if (auto* t = WeaponAt(tab, slot, weapForm, paArmorForm)) {
                return t;
            }
            if (auto* t = WeaponAt(tab, slot, weapForm, Slots::ANY_PA)) {
                return t;
            }
            if (paArmorForm) {
                if (auto ait = tab.set.armorOverrides.find(paArmorForm); ait != tab.set.armorOverrides.end()) {
                    if (auto it = ait->second.find(slot); it != ait->second.end()) {
                        return &it->second;
                    }
                }
            }
            return &tab.slotsPA[slot];
        }
        if (wear.pack && Slots::IsBackSlot(slot)) {
            if (auto* t = WeaponAt(tab, slot, weapForm, wear.pack)) {
                return t;
            }
            if (auto* t = WeaponAt(tab, slot, weapForm, Slots::ANY_PACK)) {
                return t;
            }
            if (auto pit = tab.set.packOverrides.find(wear.pack); pit != tab.set.packOverrides.end()) {
                if (auto it = pit->second.find(slot); it != pit->second.end()) {
                    return &it->second;
                }
            }
            if (auto it = tab.set.packGeneric.find(slot); it != tab.set.packGeneric.end()) {
                return &it->second;
            }
        }
        if (const auto over = OverFor(wear, slot)) {
            if (auto* t = WeaponAt(tab, slot, weapForm, over)) {
                return t;
            }
            if (auto* t = WeaponAt(tab, slot, weapForm, Slots::ANY_OVER_ARMOR)) {
                return t;
            }
            if (auto ait = tab.set.armorOverrides.find(over); ait != tab.set.armorOverrides.end()) {
                if (auto it = ait->second.find(slot); it != ait->second.end()) {
                    return &it->second;
                }
            }
        }
        if (auto* t = WeaponAt(tab, slot, weapForm, wear.armor)) {
            return t;
        }
        if (wear.armor) {
            if (auto* t = WeaponAt(tab, slot, weapForm, Slots::ANY_ARMOR)) {
                return t;
            }
            if (auto ait = tab.set.armorOverrides.find(wear.armor); ait != tab.set.armorOverrides.end()) {
                if (auto it = ait->second.find(slot); it != ait->second.end()) {
                    return &it->second;
                }
            }
        }
        return &tab.Default(slot);
    }

    Slots::Transform* ResolveLadder(int slot, std::uint32_t weapForm,
        const Npc::WearCtx& wear, bool inPA, std::uint32_t paArmorForm) {
        return ResolveLadderIn(PlayerTables(), slot, weapForm, wear, inPA, paArmorForm);
    }

    Slots::Transform* ResolveNpcLadder(Slots::NpcTableSet& set, int slot, std::uint32_t weapForm,
        const Npc::WearCtx& wear, bool inPA, std::uint32_t paArmorForm) {
        return ResolveLadderIn(NpcTablesFor(set), slot, weapForm, wear, inPA, paArmorForm);
    }

    Slots::Transform* ActiveTransform(int slot) {
        return ResolveLadder(slot, g_state[slot].formID, PlayerWearCtx(),
            g_inPA, g_paArmorForm);
    }

    const Slots::Transform* GroupAnchor(int slot) {
        if (slot >= Slots::MAX_SLOTS || !Slots::IsCustom(slot)) {
            return nullptr;
        }
        const int v = Slots::GroupVSlot(Slots::CustomOf(slot).group);
        return v >= 0 ? ActiveTransform(v) : nullptr;
    }

    bool GroupHidden(int slot) {
        const auto* g = GroupAnchor(slot);
        return g && g->hidden;
    }

    void ApplyTransforms(RE::NiAVObject* obj, const Slots::Transform* group, const Slots::Transform& m) {
        RE::NiMatrix3 mrot;
        mrot.FromEulerAnglesXYZ(m.rx, m.ry, m.rz);
        if (!group) {
            obj->local.translate = RE::NiPoint3{ m.px, m.py, m.pz };
            obj->local.rotate = mrot;
            obj->local.scale = m.scale;
            return;
        }
        RE::NiMatrix3 grot;
        grot.FromEulerAnglesXYZ(group->rx, group->ry, group->rz);
        const RE::NiPoint3 rel{ m.px * group->scale, m.py * group->scale, m.pz * group->scale };
        obj->local.translate = RE::NiPoint3{ group->px, group->py, group->pz } + grot * rel;
        obj->local.rotate = grot * mrot;
        obj->local.scale = group->scale * m.scale;
    }

    //============= Omod Fingerprint =============

    std::uint64_t HashInstanceData(const RE::BGSObjectInstanceExtra* inst) noexcept;

    std::uint64_t OmodHash(const RE::ExtraDataList* extra) {
        if (!extra) {
            return 0;
        }
        const auto* inst = extra->GetByType<RE::BGSObjectInstanceExtra>();
        if (!inst) {
            return 0;
        }
        return HashInstanceData(inst);
    }

    std::uint64_t HashInstanceData(const RE::BGSObjectInstanceExtra* inst) noexcept {
        __try {
            const auto data = inst->GetIndexData();
            const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
            const std::size_t n = data.size_bytes();
            if (!p || n == 0 || n > 4096) {
                return 1;
            }
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (std::size_t i = 0; i < n; ++i) {
                h = (h ^ p[i]) * 0x100000001b3ull;
            }
            return h;
        } __except (1) {
            return 0xBAD0BAD0ull;
        }
    }

    //============= Loader & Tick State =============

    static_assert(sizeof(RE::Inventory3DManager) == 0x140);
    alignas(16) std::byte g_invMgrBuf[sizeof(RE::Inventory3DManager)]{};
    RE::Inventory3DManager* g_invMgr = nullptr;

    struct Pending
    {
        int slot{ -1 };
        RE::TESBoundObject* object{ nullptr };
        RE::BSTSmartPointer<RE::ExtraDataList> extra;
        bool equipped{ false };
        std::uint32_t npcKey{ 0 };
        int npcSlot{ -1 };
    };
    std::vector<Pending> g_queue;
    bool g_busy = false;
    Pending g_active;
    int g_polls = 0;
    bool g_harvestLoaded = false;

    bool g_prevCombo = false;
    bool g_playerWeaponDrawn = false;
    bool g_loopStarted = false;
    bool g_loading = false;
    std::uint64_t g_loadGeneration = 0;

    //============= Helpers & Visibility =============

    void Hud(const std::string& msg, const char* sound) {
        Schedule([m = msg, sound]() {
            RE::SendHUDMessage::ShowHUDMessage(m.c_str(), sound, true, false);
        }, 0);
    }

    bool IsWeaponDrawn() {
        auto* player = RE::PlayerCharacter::GetSingleton();
        return player && player->GetWeaponMagicDrawn();
    }

    bool IsFirstPerson() {
        auto* cam = RE::PlayerCamera::GetSingleton();
        return cam && cam->currentState &&
               cam->currentState.get() == cam->GetState(RE::CameraStates::kFirstPerson).get();
    }

    void WashAllDisplays();

    bool PlayerSleeping() {
        return Npc::HideSleepingRef() && g_playerInBed;
    }

    void ResyncPlayerInBed() {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }
        const auto s = static_cast<std::uint32_t>(player->DoGetSitSleepState());
        if (s >= 5 && s <= 7) {
            g_playerInBed = true;
        } else if (s == 0) {
            g_playerInBed = false;
        }
    }

    bool ComputeVisible(int slot) {
        if (Overlay::g_open.load()) {
            return !GroupHidden(slot) && !ActiveTransform(slot)->hidden;
        }
        if (GroupHidden(slot)) {
            return false;
        }
        if (IsFirstPerson()) {
            return false;
        }
        if (ActiveTransform(slot)->hidden) {
            return false;
        }
        if (g_hideAll || (Slots::g_hideWhenNoBodyArmor && g_noBodyArmor)) {
            return false;
        }
        if (PlayerSleeping()) {
            return false;
        }
        if (g_state[slot].equippedDisplay && g_playerWeaponDrawn) {
            return false;
        }
        return true;
    }

    void UpdateVisibilityAll() {
        std::lock_guard lock(g_tablesMutex);
        for (int i = 0; i < MAX_SLOTS; ++i) {
            if (g_state[i].formID) {
                SetCloneVisible(i, ComputeVisible(i));
            }
        }
    }

    //============= Scheduler =============
    struct Delayed
    {
        std::chrono::steady_clock::time_point due;
        std::function<void()> fn;
    };
    std::mutex g_schedMutex;
    std::condition_variable g_schedCv;
    std::vector<Delayed> g_schedQueue;

    void Schedule(std::function<void()> fn, int delayMs) {
        static bool workerStarted = [] {
            std::thread([]() {
                std::unique_lock lock(g_schedMutex);
                for (;;) {
                    if (g_schedQueue.empty()) {
                        g_schedCv.wait(lock);
                        continue;
                    }
                    auto next = std::min_element(g_schedQueue.begin(), g_schedQueue.end(),
                        [](const Delayed& a, const Delayed& b) { return a.due < b.due; });
                    if (std::chrono::steady_clock::now() < next->due) {
                        g_schedCv.wait_until(lock, next->due);
                        continue;
                    }
                    auto fn = std::move(next->fn);
                    g_schedQueue.erase(next);
                    lock.unlock();
                    if (const auto* task = F4SE::GetTaskInterface(); task) {
                        task->AddTask(std::move(fn));
                    }
                    lock.lock();
                }
            }).detach();
            return true;
        }();
        (void)workerStarted;
        {
            std::lock_guard lock(g_schedMutex);
            g_schedQueue.push_back({ std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs),
                std::move(fn) });
        }
        g_schedCv.notify_one();
    }

    //============= Clone Factory =============

    constexpr const char* IGNORE_MARKERS[]{ "[CHWIgnored]", "[VFIgnored]" };

    std::string SlotNodeName(int slot) { return fmt::format("VisFavSlot_{}", slot); }

    std::string SafeName(RE::TESForm* form) {
        if (form) {
            if (const auto n = RE::TESFullName::GetFullName(*form); !n.empty()) {
                return Slots::IniSafe(std::string(n));
            }
        }
        return "<item>";
    }

    RE::NiAVObject* Player3D() {
        auto* player = RE::PlayerCharacter::GetSingleton();
        return player ? player->Get3D(false) : nullptr;
    }

    void StripCollision(RE::NiAVObject* obj) {
        Walk(obj, [](RE::NiAVObject* o) {
            o->collisionObject = nullptr;
            return true;
        });
    }

    void StripControllers(RE::NiAVObject* obj) {
        Walk(obj, [](RE::NiAVObject* o) {
            o->controllers.reset();
            return true;
        });
    }

    void ResetFade(RE::NiAVObject* obj) {
        Walk(obj, [](RE::NiAVObject* o) {
            o->fadeAmount = 1.0f;
            if (Engine::RttiClassName(o) == ".?AVBSFadeNode@@") {
                static_cast<RE::BSFadeNode*>(o)->GetFadeNodeRuntimeData().currentFade = 1.0f;
            }
            return true;
        });
    }

    int CulledBelow(RE::NiAVObject* root) {
        int n = 0;
        Walk(root, [&](RE::NiAVObject* o) {
            n += o != root && o->GetAppCulled() ? 1 : 0;
            return true;
        });
        return n;
    }

    bool HasIgnoreMarker(RE::NiAVObject* obj) {
        auto* node = obj ? obj->IsNode() : nullptr;
        if (!node) {
            return false;
        }
        for (const auto& child : node->GetRuntimeData().children) {
            const char* nm = child ? child->name.c_str() : nullptr;
            if (!nm || !nm[0]) {
                continue;
            }
            const auto canon = Slots::CanonicalNodeName(nm);
            for (const char* marker : IGNORE_MARKERS) {
                if (Slots::SameName(canon, marker)) {
                    return true;
                }
            }
        }
        return false;
    }

    void RenameForAnimIsolation(RE::NiAVObject* obj) {
        Walk(obj, [](RE::NiAVObject* o) {
            if (const char* nm = o->name.c_str(); nm && nm[0]) {
                o->name = fmt::format("VF_{}", nm).c_str();
            }
            return true;
        });
    }

    void CullBloodDecals(RE::NiAVObject* obj) {
        Walk(obj, [](RE::NiAVObject* o) {
            if (const char* nm = o->name.c_str(); nm && nm[0]) {
                for (const char* p = nm; *p; ++p) {
                    if (_strnicmp(p, "blood", 5) == 0) {
                        Show(o, false);
                        return false;
                    }
                }
            }
            return true;
        });
    }

    bool IsLightNodeType(const std::string& rtti) {
        return rtti == ".?AVNiPointLight@@" ||
               rtti == ".?AVNiSpotLight@@" ||
               rtti == ".?AVNiAmbientLight@@" ||
               rtti == ".?AVNiDirectionalLight@@";
    }

    void StripLightNodes(RE::NiAVObject* root) {
        std::vector<RE::NiAVObject*> doomed;
        Walk(root, [&](RE::NiAVObject* o) {
            const auto rtti = Engine::RttiClassName(o);
            if (o == root || !(IsLightNodeType(rtti) || rtti == ".?AVBSValueNode@@")) {
                return true;
            }
            doomed.push_back(o);
            return false;
        });
        for (auto* o : doomed) {
            RE::NiPointer<RE::NiAVObject> released;
            o->parent->DetachChild(o, released);
        }
        if (!doomed.empty()) {
            VF_VLOG("stripped {} light/addon node(s) from display clone", doomed.size());
        }
    }

    bool HasVisController(RE::NiAVObject* obj) {
        for (auto* c = obj->controllers.get(); c; c = c->next.get()) {
            if (Engine::RttiClassName(c) == ".?AVNiVisController@@") {
                return true;
            }
        }
        return false;
    }

    void UnhideGated(RE::NiAVObject* obj) {
        Walk(obj, [](RE::NiAVObject* o) {
            if (o->GetAppCulled() && HasVisController(o)) {
                Show(o, true);
            }
            return true;
        });
    }

    void CullEffects(RE::NiAVObject* obj, std::uint32_t formID) {
        const auto it = Slots::g_hiddenParts.find(formID);
        const auto* parts = it != Slots::g_hiddenParts.end() ? &it->second : nullptr;
        int culled = 0;
        Walk(obj, [&](RE::NiAVObject* o) {
            if (o->GetAppCulled() && o->IsNode()) {
                return false;
            }
            bool cull = Engine::RttiClassName(o).find("ParticleSystem") != std::string::npos || HasIgnoreMarker(o);
            if (!cull && parts) {
                if (const char* nm = o->name.c_str(); nm && nm[0]) {
                    cull = Slots::NameInList(*parts, Slots::CanonicalNodeName(nm));
                }
            }
            if (!cull) {
                return true;
            }
            Show(o, false);
            ++culled;
            return false;
        });
        if (culled) {
            VF_VLOG("culled {} node(s) from display of {:08X}", culled, formID);
        }
    }

    void RetireSlot(RE::NiAVObject* player3D, int slot) {
        const auto nm = SlotNodeName(slot);
        while (auto* old = player3D->GetObjectByName(nm.c_str())) {
            Show(old, false);
            old->name = "VisFavSlot_Dead";
        }
    }

    void ReapDeadNodes(RE::NiAVObject* root, const char* name) {
        if (!root) {
            return;
        }
        while (auto* dead = root->GetObjectByName(name)) {
            auto* parent = dead->parent;
            if (!parent) {
                dead->name = "VisFavSlot_Orphan";
                continue;
            }
            RE::NiPointer<RE::NiAVObject> released;
            parent->DetachChild(dead, released);
        }
    }

    RE::NiNode* FindSlotBone(RE::NiAVObject* root, const std::string& name) {
        if (!g_inPA) {
            auto* obj = root->GetObjectByName(name.c_str());
            return obj ? obj->IsNode() : nullptr;
        }
        RE::NiNode* last = nullptr;
        int count = 0;
        Walk(root, [&](RE::NiAVObject* obj) {
            const char* nm = obj->name.c_str();
            if (auto* node = obj->IsNode(); node && nm && name == nm) {
                last = node;
                ++count;
            }
            return true;
        });
        if (count > 1) {
            VF_VLOG("bone '{}': {} matches in PA rig - using the last (grafted PA skeleton)", name, count);
        }
        return last;
    }

    RE::NiAVObject* MakeDisplayClone(RE::NiAVObject* source, std::uint32_t formID) {
        auto* clone = Engine::CloneNi(source);
        const auto rtti = clone ? Engine::RttiClassName(clone) : std::string{};
        if (!clone || !Engine::LooksLikeAVObject(rtti)) {
            logger::error("display clone failed (rtti='{}')", rtti);
            return nullptr;
        }
        StripCollision(clone);
        if (Slots::g_showWeaponFX) {
            UnhideGated(clone);
        }
        StripControllers(clone);
        StripLightNodes(clone);
        VF_VLOG("clone of {:08X}: root {}, {} node(s) below it hidden as loaded", formID,
            clone->GetAppCulled() ? "culled" : "visible", CulledBelow(clone));
        clone->SetAppCulled(false);
        ResetFade(clone);
        CullEffects(clone, formID);
        RenameForAnimIsolation(clone);
        CullBloodDecals(clone);
        Engine::SetWeaponBloodAmount(clone, 0.0f);
        return clone;
    }

    static bool AttachSlotClone(int slot) {
        std::lock_guard lock(g_tablesMutex);
        auto& st = g_state[slot];
        if (!st.source) {
            return false;
        }
        auto* player3D = Player3D();
        if (!player3D) {
            return false;
        }

        RetireSlot(player3D, slot);

        const auto& def = Slots::g_slots[slot];
        auto* bone = FindSlotBone(player3D, def.bone);
        if (!bone) {
            logger::error("slot {} bone '{}' not found", slot, def.bone);
            return false;
        }

        auto* clone = MakeDisplayClone(st.source.get(), st.formID);
        if (!clone) {
            logger::error("slot {} clone failed", slot);
            return false;
        }

        clone->name = SlotNodeName(slot).c_str();
        ApplyTransforms(clone, GroupAnchor(slot), *ActiveTransform(slot));
        bone->AttachChild(clone, true);

        if (!ComputeVisible(slot)) {
            Show(clone, false);
        }
        return true;
    }

    static void SetCloneVisible(int slot, bool visible) {
        auto* player3D = Player3D();
        auto* obj = player3D ? player3D->GetObjectByName(SlotNodeName(slot).c_str()) : nullptr;
        if (obj) {
            Show(obj, visible);
        }
    }

    //============= Harvest Queue =============

    void PollHarvest();

    void StartNextHarvest() {
        std::lock_guard lock(g_tablesMutex);
        if (g_busy || g_queue.empty()) {
            return;
        }
        g_active = g_queue.front();
        g_queue.erase(g_queue.begin());
        g_busy = true;
        g_polls = 0;

        if (!g_invMgr) {
            VF_VLOG("constructing shared Inventory3DManager");
            g_invMgr = Engine::ConstructInventory3DManager(g_invMgrBuf);
            Engine::Begin3D(g_invMgr);
            VF_VLOG("tempRef = {}", fmt::ptr(g_invMgr->tempRef));
        }
        if (!g_invMgr->tempRef) {
            Engine::Begin3D(g_invMgr);
        }
        if (!g_invMgr->tempRef) {
            logger::warn("no tempRef after Begin3D retry - dropping harvest for slot {}", g_active.slot);
            PollHarvest();
            return;
        }
        if (g_harvestLoaded) {
            Engine::UnloadInventoryItem(g_invMgr);
        }
        g_harvestLoaded = true;

        VF_VLOG("harvest slot {}: {:08X} '{}'{}", g_active.slot,
            g_active.object->GetFormID(), SafeName(g_active.object),
            g_active.equipped ? " [equipped display]" : "");
        Engine::LoadInventoryItem(g_invMgr, g_active.object, g_active.extra.get(), 0);
        PollHarvest();
    }

    void PollHarvest() {
        std::lock_guard lock(g_tablesMutex);
        if (!g_busy || !g_invMgr || !g_invMgr->tempRef) {
            if (g_busy && g_active.npcSlot >= 0 && g_active.object) {
                Npc::OnHarvestModel(g_active.npcKey, g_active.npcSlot, nullptr,
                    g_active.object->GetFormID(), OmodHash(g_active.extra.get()));
            }
            g_busy = false;
            return;
        }
        RE::NiAVObject* root = g_invMgr->tempRef->Load3D(false);
        const auto rtti = root ? Engine::RttiClassName(root) : std::string{};
        if (root && Engine::LooksLikeAVObject(rtti)) {
            if (g_active.npcSlot >= 0) {
                Npc::OnHarvestModel(g_active.npcKey, g_active.npcSlot, root,
                    g_active.object->GetFormID(), OmodHash(g_active.extra.get()));
                g_busy = false;
                Schedule(StartNextHarvest, 50);
                return;
            }
            auto& st = g_state[g_active.slot];
            st.source = RE::NiPointer<RE::NiAVObject>(root);
            st.formID = g_active.object->GetFormID();
            st.object = g_active.object;
            st.equippedDisplay = g_active.equipped;
            st.omodHash = OmodHash(g_active.extra.get());
            const bool ok = AttachSlotClone(g_active.slot);
            VF_VLOG("slot {} display {} ({:08X} '{}')", g_active.slot,
                ok ? "ATTACHED" : "attach failed", st.formID, SafeName(g_active.object));
            g_busy = false;
            Schedule(StartNextHarvest, 50);
            return;
        }
        if (++g_polls < 20) {
            const auto generation = g_loadGeneration;
            Schedule([generation]() {
                if (generation == g_loadGeneration) {
                    PollHarvest();
                }
            }, 150);
        } else {
            const auto formID = g_active.object ? g_active.object->GetFormID() : 0;
            logger::warn("slot {} harvest gave no model - skipping {:08X}", g_active.slot, formID);
            if (g_active.npcSlot >= 0) {
                Npc::OnHarvestModel(g_active.npcKey, g_active.npcSlot, nullptr, formID,
                    OmodHash(g_active.extra.get()));
            }
            g_busy = false;
            Schedule(StartNextHarvest, 50);
        }
    }

}

namespace Display
{
    void RequestNpcHarvest(RE::TESBoundObject* obj, const RE::BSTSmartPointer<RE::ExtraDataList>& extra,
        std::uint32_t npcKey, int slot) {
        std::lock_guard lock(g_tablesMutex);
        Pending p;
        p.slot = -1;
        p.object = obj;
        p.extra = extra;
        p.npcKey = npcKey;
        p.npcSlot = slot;
        g_queue.push_back(std::move(p));
        Schedule(StartNextHarvest, 0);
    }

    //============= Reconcile =============

    void RequestReconcile(int delayMs) {
        const auto generation = g_loadGeneration;
        Schedule([generation]() {
            if (!g_loading && generation == g_loadGeneration) {
                g_dirty = true;
            }
        }, delayMs);
    }

    void Reconcile() {
        std::lock_guard lock(g_tablesMutex);
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* player3D = Player3D();
        if (!player || !player->inventoryList || !player3D) {
            return;
        }
        ReapDeadNodes(player3D, "VisFavSlot_Dead");

        struct Want
        {
            RE::TESBoundObject* object{ nullptr };
            RE::BSTSmartPointer<RE::ExtraDataList> extra;
            bool equipped{ false };
        };
        Want want[MAX_SLOTS]{};

        std::vector<std::pair<std::uint32_t, std::uint64_t>> equippedItems;
        RE::TESBoundObject* eqWeapObj = nullptr;
        RE::BSTSmartPointer<RE::ExtraDataList> eqWeapExtra;
        int eqWeapFam = -1;
        std::uint32_t eqUiForm = 0;
        std::uint32_t eqUnmappedForm = 0;
        bool bodyArmorWorn = false;
        std::vector<std::uint32_t> packClaimants;
        std::vector<std::uint32_t> packCandidates;
        std::vector<std::uint32_t> armorCandidates;
        std::vector<std::uint32_t> paCandidates;
        std::vector<std::uint32_t> overTorsoCand;
        std::vector<std::uint32_t> overLLegCand;
        std::vector<std::uint32_t> overRLegCand;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> wornArmo;
        const std::uint32_t packMask = Slots::BackpackSlotMask();
        const std::uint32_t paMask = Slots::PATorsoSlotMask();
        const int pmode = Slots::g_playerDisplayMode;
        player->inventoryList->ForEachStack(
            [pmode](RE::BGSInventoryItem&) { return pmode != 2; },
            [&](RE::BGSInventoryItem& item, RE::BGSInventoryItem::Stack& stack) {
                if (item.object && stack.IsEquipped()) {
                    equippedItems.emplace_back(item.object->GetFormID(), OmodHash(stack.extra.get()));
                    if (!eqWeapObj) {
                        if (auto* w = item.object->As<RE::TESObjectWEAP>(); w) {
                            const auto wtype = w->weaponData.type.get();
                            if (wtype != RE::WEAPON_TYPE::kGrenade && wtype != RE::WEAPON_TYPE::kMine) {
                                if (!eqUiForm) {
                                    eqUiForm = item.object->GetFormID();
                                }
                                if (const int fam = Npc::FamilyOfWeapon(w, stack.extra.get()); fam >= 0) {
                                    if (!Slots::IsDisplayBlacklisted(item.object->GetFormID())) {
                                        eqWeapObj = item.object;
                                        eqWeapExtra = stack.extra;
                                        eqWeapFam = fam;
                                    }
                                } else {
                                    eqUnmappedForm = item.object->GetFormID();
                                }
                            }
                        }
                    }
                    if (const auto* armo = item.object->As<RE::TESObjectARMO>(); armo) {
                        const auto slots = armo->bipedModelData.bipedObjectSlots;
                        wornArmo.emplace_back(item.object->GetFormID(), slots);
                        if (slots & (1u << 3)) {
                            bodyArmorWorn = true;
                            armorCandidates.push_back(item.object->GetFormID());
                        }
                        if ((slots & packMask) && !(slots & (1u << 3))) {
                            packClaimants.push_back(item.object->GetFormID());
                            if (!Slots::IsPackBlacklisted(item.object->GetFormID())) {
                                packCandidates.push_back(item.object->GetFormID());
                            }
                        }
                        if (slots & paMask) {
                            paCandidates.push_back(item.object->GetFormID());
                        }
                        if (!(slots & (1u << 3))) {
                            if (slots & (1u << 11)) {
                                overTorsoCand.push_back(item.object->GetFormID());
                            }
                            if (slots & (1u << 14)) {
                                overLLegCand.push_back(item.object->GetFormID());
                            }
                            if (slots & (1u << 15)) {
                                overRLegCand.push_back(item.object->GetFormID());
                            }
                        }
                    }
                }
                return true;
            });
        const auto stickyPick = [](const std::vector<std::uint32_t>& cands, std::uint32_t current) {
            if (cands.empty()) {
                return 0u;
            }
            for (const auto c : cands) {
                if (c == current) {
                    return current;
                }
            }
            return cands.front();
        };
        g_packClaimants = std::move(packClaimants);
        g_eqWeapUiForm = eqUiForm;
        static std::uint32_t unmappedLogged = 0;
        if (!eqWeapObj && eqUnmappedForm && eqUnmappedForm != unmappedLogged) {
            unmappedLogged = eqUnmappedForm;
            logger::info("equipped weapon {:08X} '{}' has no weapon-type keyword (base or instance) - no slot claim",
                eqUnmappedForm, Slots::FriendlyName(eqUnmappedForm));
        }
        const std::uint32_t packForm = stickyPick(packCandidates, g_packForm);
        const std::uint32_t armorForm = stickyPick(armorCandidates, g_armorForm);
        const std::uint32_t paArmorForm = stickyPick(paCandidates, g_paArmorForm);
        const std::uint32_t overTorso = stickyPick(overTorsoCand, g_overTorsoForm);
        const std::uint32_t overLLeg = stickyPick(overLLegCand, g_overLLegForm);
        const std::uint32_t overRLeg = stickyPick(overRLegCand, g_overRLegForm);
        const bool wasNoBodyArmor = g_noBodyArmor;
        g_noBodyArmor = !bodyArmorWorn;
        if (wasNoBodyArmor != g_noBodyArmor) {
            VF_VLOG("body armor {} - refreshing visibility", bodyArmorWorn ? "worn" : "REMOVED");
            UpdateVisibilityAll();
        }
        const bool wasInBed = g_playerInBed;
        ResyncPlayerInBed();
        if (wasInBed != g_playerInBed) {
            UpdateVisibilityAll();
        }
        if (const bool drawnNow = IsWeaponDrawn(); drawnNow != g_playerWeaponDrawn) {
            g_playerWeaponDrawn = drawnNow;
            UpdateVisibilityAll();
            WashAllDisplays();
        }

        const bool inPA = RE::PowerArmor::PlayerInPowerArmor();
        if (inPA != g_inPA) {
            g_inPA = inPA;
            g_packForm = packForm;
            g_armorForm = armorForm;
            g_paArmorForm = paArmorForm;
            g_overTorsoForm = overTorso;
            g_overLLegForm = overLLeg;
            g_overRLegForm = overRLeg;
            logger::info("context: PowerArmor={} - rebuilding all displays", inPA);
            if (inPA) {
                for (const auto& [id, mask] : wornArmo) {
                    logger::info("  PA wearer ARMO {:08X} '{}' bipedMask={:08X}", id, Slots::FriendlyName(id), mask);
                }
                logger::info("  PA torso context = {:08X} '{}'", paArmorForm, Slots::FriendlyName(paArmorForm));
            }
            for (int i = 0; i < MAX_SLOTS; ++i) {
                if (g_state[i].formID) {
                    AttachSlotClone(i);
                }
            }
        } else {
            if (inPA && paArmorForm != g_paArmorForm) {
                g_paArmorForm = paArmorForm;
                VF_VLOG("context: PA torso={:08X} '{}' - reapplying slot transforms",
                    paArmorForm, Slots::FriendlyName(paArmorForm));
                for (int i = 0; i < MAX_SLOTS; ++i) {
                    if (g_state[i].formID) {
                        ReapplySlotTransform(i);
                    }
                }
            } else if (!inPA) {
                g_paArmorForm = 0;
            }
            if (packForm != g_packForm) {
                g_packForm = packForm;
                VF_VLOG("context: backpack={:08X} - reapplying back-slot transforms", packForm);
                for (int i : { 1, 4, 8 }) {
                    if (g_state[i].formID) {
                        ReapplySlotTransform(i);
                    }
                }
            }
            if (armorForm != g_armorForm) {
                g_armorForm = armorForm;
                VF_VLOG("context: body armor={:08X} '{}' - reapplying slot transforms",
                    armorForm, Slots::FriendlyName(armorForm));
                for (int i = 0; i < MAX_SLOTS; ++i) {
                    if (g_state[i].formID) {
                        ReapplySlotTransform(i);
                    }
                }
            }
            if (overTorso != g_overTorsoForm || overLLeg != g_overLLegForm || overRLeg != g_overRLegForm) {
                g_overTorsoForm = overTorso;
                g_overLLegForm = overLLeg;
                g_overRLegForm = overRLeg;
                VF_VLOG("context: over-armor T={:08X} LL={:08X} RL={:08X} - reapplying slot transforms",
                    overTorso, overLLeg, overRLeg);
                for (int i = 0; i < MAX_SLOTS; ++i) {
                    if (g_state[i].formID) {
                        ReapplySlotTransform(i);
                    }
                }
            }
        }
        const auto isEquippedInstance = [&](std::uint32_t id, std::uint64_t hash) {
            for (const auto& [eid, ehash] : equippedItems) {
                if (eid == id && ehash == hash) {
                    return true;
                }
            }
            return false;
        };

        player->inventoryList->ForEachStack(
            [pmode](RE::BGSInventoryItem&) { return pmode == 0; },
            [&](RE::BGSInventoryItem& item, RE::BGSInventoryItem::Stack& stack) {
                const auto extra = stack.extra;
                if (!extra || !extra->HasType(RE::EXTRA_DATA_TYPE::kFavorite)) {
                    return true;
                }
                if (!item.object) {
                    return true;
                }
                if (!Slots::g_displayAnyItemType && !item.object->As<RE::TESObjectWEAP>()) {
                    return true;
                }
                const auto id = item.object->GetFormID();
                if (Slots::IsDisplayBlacklisted(id)) {
                    return true;
                }
                const bool isWeap = item.object->As<RE::TESObjectWEAP>() != nullptr;
                const bool instEquipped = stack.IsEquipped() ||
                                          isEquippedInstance(id, OmodHash(extra.get()));
                if (!isWeap && instEquipped) {
                    return true;
                }
                const auto* fav = extra->GetByType<RE::ExtraFavorite>();
                const int wheel = fav ? fav->quickkeyIndex : -1;
                if (wheel >= 0 && wheel < Slots::FAV_SLOTS) {
                    const int slot = Slots::SlotOfWheel(wheel);
                    if (!Slots::g_slotEnabled[slot]) {
                        return true;
                    }
                    const bool sticky = g_state[slot].formID && id == g_state[slot].formID;
                    if (!want[slot].object || sticky) {
                        want[slot] = { item.object, extra, isWeap && instEquipped };
                    }
                }
                return true;
            });

        if (pmode != 2 && eqWeapObj) {
            const auto eqHash = OmodHash(eqWeapExtra.get());
            bool favPlaced = false;
            for (int s = 0; s < Slots::FAV_SLOTS; ++s) {
                if (want[s].object == eqWeapObj && OmodHash(want[s].extra.get()) == eqHash) {
                    favPlaced = true;
                    break;
                }
            }
            if (!favPlaced) {
                int primary = Npc::PlayerEquipSlot(eqWeapFam, -1);
                if (primary >= 0 && want[primary].object) {
                    if (auto* occ = want[primary].object->As<RE::TESObjectWEAP>(); occ) {
                        const int occFam = Npc::FamilyOfWeapon(occ, want[primary].extra.get());
                        if (occFam >= 0 && Npc::FamilyRank(occFam) < Npc::FamilyRank(eqWeapFam)) {
                            if (const int next = Npc::PlayerEquipSlot(eqWeapFam, primary); next >= 0) {
                                primary = next;
                            }
                        }
                    }
                }
                if (primary >= 0) {
                    want[primary] = { eqWeapObj, eqWeapExtra, true };
                }
            }
        }

        if (pmode != 2 && Slots::CustomCount() > 0) {
            std::vector<std::uint32_t> customForm(Slots::CustomCount(), 0);
            for (int i = 0; i < Slots::CustomCount(); ++i) {
                const auto id = Slots::SpecToForm(Slots::g_custom[i].itemSpec);
                customForm[i] = Slots::IsDisplayBlacklisted(id) ? 0 : id;
            }
            player->inventoryList->ForEachStack(
                [](RE::BGSInventoryItem&) { return true; },
                [&](RE::BGSInventoryItem& item, RE::BGSInventoryItem::Stack& stack) {
                    if (!item.object) {
                        return true;
                    }
                    const auto id = item.object->GetFormID();
                    for (int i = 0; i < Slots::CustomCount(); ++i) {
                        if (customForm[i] != id) {
                            continue;
                        }
                        const auto hash = OmodHash(stack.extra.get());
                        if (Slots::g_custom[i].fingerprint != 0 && hash != Slots::g_custom[i].fingerprint) {
                            continue;
                        }
                        const auto& def = Slots::g_custom[i];
                        const bool isWeap = item.object->As<RE::TESObjectWEAP>() != nullptr;
                        const bool instEquipped = stack.IsEquipped() || isEquippedInstance(id, hash);
                        if (def.hideWhenEquipped && instEquipped && !isWeap) {
                            continue;
                        }
                        want[Slots::FAV_SLOTS + i] = { item.object, stack.extra,
                            isWeap && instEquipped && def.hideWhenEquipped };
                    }
                    return true;
                });
            for (int i = 0; i < Slots::CustomCount(); ++i) {
                const int idx = Slots::FAV_SLOTS + i;
                if (!want[idx].object && !Slots::g_custom[i].hideNotInInventory && customForm[i]) {
                    if (auto* base = RE::TESForm::GetFormByID<RE::TESBoundObject>(customForm[i])) {
                        want[idx] = { base, nullptr, false };
                    }
                }
            }
            for (int i = 0; i < Slots::CustomCount(); ++i) {
                const auto* claimed = want[Slots::FAV_SLOTS + i].object;
                if (!claimed) {
                    continue;
                }
                for (int s = 0; s < Slots::FAV_SLOTS; ++s) {
                    if (want[s].object == claimed) {
                        want[s] = {};
                    }
                }
            }
        }

        for (int i = 0; i < MAX_SLOTS; ++i) {
            auto& st = g_state[i];
            if (g_busy && g_active.slot == i) {
                continue;
            }
            const auto wantID = want[i].object ? want[i].object->GetFormID() : 0;

            if (st.formID && st.formID != wantID) {
                VF_VLOG("slot {}: {:08X} out ({})", i, st.formID, wantID ? "replaced" : "gone");
                RetireSlot(player3D, i);
                st = SlotState{};
            }
            if (wantID && st.formID == wantID) {
                if (st.equippedDisplay != want[i].equipped) {
                    st.equippedDisplay = want[i].equipped;
                    SetCloneVisible(i, ComputeVisible(i));
                }
                const auto h = OmodHash(want[i].extra.get());
                if (h != st.omodHash) {
                    VF_VLOG("slot {}: attachments changed - rebuilding", i);
                    RetireSlot(player3D, i);
                    st.formID = 0;
                    st.source.reset();
                }
            }
            if (wantID && st.formID != wantID) {
                bool queued = false;
                for (const auto& p : g_queue) {
                    if (p.slot == i) {
                        queued = true;
                        break;
                    }
                }
                if (!queued) {
                    g_queue.push_back({ i, want[i].object, want[i].extra, want[i].equipped });
                }
            }
            if (st.formID && st.source && !player3D->GetObjectByName(SlotNodeName(i).c_str())) {
                VF_VLOG("slot {}: clone missing (3D rebuild?) - reattaching", i);
                AttachSlotClone(i);
            }
        }

        if (!g_queue.empty()) {
            StartNextHarvest();
        }
    }

    void ReapplySlotTransform(int slot) {
        std::lock_guard lock(g_tablesMutex);
        auto* player3D = Player3D();
        auto* obj = player3D ? player3D->GetObjectByName(SlotNodeName(slot).c_str()) : nullptr;
        if (!obj) {
            VF_VLOG("  slot {} reapply skipped - no display node", slot);
            return;
        }
        const auto* tr = ActiveTransform(slot);
        VF_VLOG("  slot {} -> ({:.1f},{:.1f},{:.1f})", slot, tr->px, tr->py, tr->pz);
        ApplyTransforms(obj, GroupAnchor(slot), *tr);
        SetCloneVisible(slot, ComputeVisible(slot));
    }

    static void ScanWornForPanel() {
        std::lock_guard lock(g_tablesMutex);
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->inventoryList) {
            return;
        }
        std::uint32_t eqUiForm = 0;
        std::vector<std::uint32_t> claimants;
        const std::uint32_t packMask = Slots::BackpackSlotMask();
        player->inventoryList->ForEachStack(
            [](RE::BGSInventoryItem&) { return true; },
            [&](RE::BGSInventoryItem& item, RE::BGSInventoryItem::Stack& stack) {
                if (!item.object || !stack.IsEquipped()) {
                    return true;
                }
                if (!eqUiForm) {
                    if (auto* w = item.object->As<RE::TESObjectWEAP>(); w) {
                        const auto wtype = w->weaponData.type.get();
                        if (wtype != RE::WEAPON_TYPE::kGrenade && wtype != RE::WEAPON_TYPE::kMine) {
                            eqUiForm = item.object->GetFormID();
                        }
                    }
                }
                if (const auto* armo = item.object->As<RE::TESObjectARMO>(); armo) {
                    const auto slots = armo->bipedModelData.bipedObjectSlots;
                    if ((slots & packMask) && !(slots & (1u << 3))) {
                        claimants.push_back(item.object->GetFormID());
                    }
                }
                return true;
            });
        g_eqWeapUiForm = eqUiForm;
        g_packClaimants = std::move(claimants);
    }

    //============= Tick =============

    void ToggleCustomEquip(int ci);
    void ToggleGroupEquip(int gi);
    void FireHotkey(int hk);
    bool ComboDown(int hk);

    static bool FireCustomHotkeys() {
        std::lock_guard lock(g_tablesMutex);
        bool fired = false;
        const bool kbOwned = Overlay::g_open.load() && Overlay::g_kbOwned.load();
        const auto bindDown = [&](int hk) {
            if (hk & 0x10000) {
                return (EngineInput::g_padMask & (hk & 0xFFFF)) != 0;
            }
            return EngineInput::g_keyDown[hk & 0xFF] &&
                   (Slots::CurrentMods() & ~Slots::ModClassOfVk(hk & 0xFF)) == (hk & 0xE0000);
        };
        for (int gi = 0; gi < Slots::GroupCount(); ++gi) {
            const int hk = Slots::g_groups[gi].hotkey;
            if (hk == 0) {
                continue;
            }
            const bool down = bindDown(hk);
            bool& prev = g_hkPrev[hk];
            if (down && !prev && !kbOwned) {
                ToggleGroupEquip(gi);
                fired = true;
            }
            prev = down;
        }
        for (int i = 0; i < Slots::CustomCount(); ++i) {
            const int hk = Slots::g_custom[i].hotkey;
            if (hk == 0) {
                continue;
            }
            const bool down = bindDown(hk);
            bool& prev = g_hkPrev[hk];
            if (down && !prev && !kbOwned) {
                FireHotkey(hk);
                fired = true;
            }
            prev = down;
        }
        return fired;
    }

    void OnButtonInput() {
        const bool blocked = HotkeysBlocked();
        static bool prevMuted = false;
        const bool muted = blocked || Overlay::g_kbOwned.load();
        if (muted && !prevMuted) {
            EngineInput::ClearAll();
        }
        prevMuted = muted;

        if (Overlay::g_open.load()) {
            if (!blocked && FireCustomHotkeys()) {
                Schedule(Reconcile, 150);
            }
            const bool combo = !blocked && ComboDown(Slots::g_openKey);
            if (combo && !g_prevCombo) {
                if (Overlay::g_unsaved.load()) {
                    Overlay::g_confirmClose = true;
                    logger::info("panel close hotkey - blocked by unsaved edits, confirm dialog raised");
                    Hud("VisibleFavorites: unsaved changes - confirm on the panel", "UIMenuCancel");
                } else {
                    Overlay::g_open = false;
                    g_dirty = true;
                    logger::info("panel closed by hotkey");
                    Hud("VisibleFavorites panel closed", "UIMenuCancel");
                }
            }
            g_prevCombo = combo;
            return;
        }
        if (g_loading) {
            return;
        }

        static bool prevHideAllKey = false;
        const bool hideAllKey = !blocked && ComboDown(Slots::g_hideAllKey);
        if (hideAllKey && !prevHideAllKey) {
            g_hideAll = !g_hideAll;
            UpdateVisibilityAll();
            Hud(g_hideAll ? "VisibleFavorites: displays hidden" : "VisibleFavorites: displays shown",
                g_hideAll ? "UIMenuCancel" : "UIMenuOK");
        }
        prevHideAllKey = hideAllKey;

        if (!blocked) {
            FireCustomHotkeys();
        }

        const bool combo = !blocked && ComboDown(Slots::g_openKey);
        if (combo && !g_prevCombo && Slots::g_enableOverlay) {
            Overlay::g_open = true;
            logger::info("panel opened by hotkey (imgui ready={})", Overlay::g_imguiReady);
            Npc::RefreshTargets();
            Hud("VisibleFavorites panel OPEN", "UIMenuOK");
        }
        g_prevCombo = combo;
    }

    void Tick() {
        static bool prevOverlayOpen = false;
        if (const bool nowOpen = Overlay::g_open.load(); nowOpen != prevOverlayOpen) {
            prevOverlayOpen = nowOpen;
            UpdateVisibilityAll();
            if (nowOpen) {
                ScanWornForPanel();
            } else {
                std::lock_guard lock(g_tablesMutex);
                for (int i = 0; i < MAX_SLOTS; ++i) {
                    if (g_state[i].formID) {
                        ReapplySlotTransform(i);
                    }
                }
                Npc::ReapplyTransforms();
            }
        }

        if (Overlay::g_open.load()) {
            if (Npc::ConsumeDirty()) {
                Npc::Reconcile();
            }
            Schedule(Tick, 50);
            return;
        }

        if (g_loading) {
            Schedule(Tick, 50);
            return;
        }

        if (g_dirty) {
            g_dirty = false;
            Reconcile();
            Npc::Reconcile();
        } else if (Npc::ConsumeDirty()) {
            Npc::Reconcile();
        }

        static bool prevFirstPerson = false;
        if (const bool fp = IsFirstPerson(); fp != prevFirstPerson) {
            prevFirstPerson = fp;
            UpdateVisibilityAll();
        }

        Schedule(Tick, 50);
    }

    void OnPlayerDrawFlip(bool drawn) {
        if (g_playerWeaponDrawn == drawn) {
            return;
        }
        g_playerWeaponDrawn = drawn;
        g_dirty = true;
        UpdateVisibilityAll();
        WashAllDisplays();
        VF_VLOG("player weapon {}", drawn ? "DRAWN" : "sheathed");
    }

    //============= Blood Washes =============

    void WashAllDisplays() {
        std::lock_guard lock(g_tablesMutex);
        auto* player3D = Player3D();
        if (!player3D) {
            return;
        }
        for (int i = 0; i < Slots::TotalSlots(); ++i) {
            if (g_state[i].formID) {
                if (auto* obj = player3D->GetObjectByName(SlotNodeName(i).c_str())) {
                    CullBloodDecals(obj);
                    Engine::SetWeaponBloodAmount(obj, 0.0f);
                }
            }
        }
    }

    void ReattachAllDisplays() {
        std::lock_guard lock(g_tablesMutex);
        for (int i = 0; i < Slots::TotalSlots(); ++i) {
            if (g_state[i].formID && g_state[i].source) {
                AttachSlotClone(i);
            }
        }
    }

    void ReattachCarrying(std::uint32_t formID) {
        std::lock_guard lock(g_tablesMutex);
        for (int i = 0; i < Slots::TotalSlots(); ++i) {
            if (g_state[i].formID == formID && g_state[i].source) {
                AttachSlotClone(i);
            }
        }
    }

    //============= Equip Hotkeys =============

    std::uint32_t ReadPadButtons() {
        using XInputGetState_t = DWORD(WINAPI*)(DWORD, void*);
        static const XInputGetState_t fn = [] {
            HMODULE m = LoadLibraryA("XInput9_1_0.dll");
            return m ? reinterpret_cast<XInputGetState_t>(GetProcAddress(m, "XInputGetState")) : nullptr;
        }();
        if (!fn) {
            return 0;
        }
        struct
        {
            DWORD packet;
            WORD buttons;
            BYTE lt, rt;
            SHORT lx, ly, rx, ry;
        } state{};
        return fn(0, &state) == 0 ? state.buttons : 0;
    }

    bool ComboDown(int hk) {
        if (hk == 0 || (hk & 0x10000)) {
            return false;
        }
        if (!EngineInput::g_keyDown[hk & 0xFF]) {
            return false;
        }
        return (Slots::CurrentMods() & ~Slots::ModClassOfVk(hk & 0xFF)) == (hk & 0xE0000);
    }

    const char* PadButtonName(std::uint32_t mask) {
        switch (mask) {
        case 0x0001: return "Pad Up";
        case 0x0002: return "Pad Down";
        case 0x0004: return "Pad Left";
        case 0x0008: return "Pad Right";
        case 0x0010: return "Pad Start";
        case 0x0020: return "Pad Back";
        case 0x0040: return "LS Click";
        case 0x0080: return "RS Click";
        case 0x0100: return "Pad LB";
        case 0x0200: return "Pad RB";
        case 0x1000: return "Pad A";
        case 0x2000: return "Pad B";
        case 0x4000: return "Pad X";
        case 0x8000: return "Pad Y";
        }
        return "Pad ?";
    }

    static std::string HotkeyNameUncached(int hk) {
        if (hk == 0) {
            return "none";
        }
        if (hk & 0x10000) {
            return PadButtonName(hk & 0xFFFF);
        }
        std::string pre;
        if (hk & 0x20000) {
            pre += "Ctrl+";
        }
        if (hk & 0x80000) {
            pre += "Alt+";
        }
        if (hk & 0x40000) {
            pre += "Shift+";
        }
        const int vk = hk & 0xFF;
        UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
        switch (vk) {
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_RCONTROL:
        case VK_RMENU:
            sc |= 0x100;
            break;
        }
        char buf[64]{};
        if (GetKeyNameTextA(static_cast<LONG>(sc << 16), buf, sizeof(buf)) > 0) {
            return pre + buf;
        }
        return pre + fmt::format("VK {:02X}", vk);
    }

    const std::string& HotkeyName(int hk) {
        static std::unordered_map<int, std::string> names;
        if (const auto it = names.find(hk); it != names.end()) {
            return it->second;
        }
        return names.emplace(hk, HotkeyNameUncached(hk)).first->second;
    }

    struct ItemPresence
    {
        bool carried{ false };
        bool equipped{ false };
        RE::BSTSmartPointer<RE::ExtraDataList> extra;
    };

    ItemPresence QueryCustomItem(int ci) {
        ItemPresence r;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->inventoryList) {
            return r;
        }
        const auto& def = Slots::g_custom[ci];
        const auto formID = Slots::SpecToForm(def.itemSpec);
        if (!formID) {
            return r;
        }
        player->inventoryList->ForEachStack(
            [](RE::BGSInventoryItem&) { return true; },
            [&](RE::BGSInventoryItem& item, RE::BGSInventoryItem::Stack& stack) {
                if (item.object && item.object->GetFormID() == formID) {
                    const auto hash = OmodHash(stack.extra.get());
                    if (def.fingerprint == 0 || hash == def.fingerprint) {
                        r.carried = true;
                        if (!r.extra || (stack.IsEquipped() && !r.equipped)) {
                            r.extra = stack.extra;
                        }
                        r.equipped = r.equipped || stack.IsEquipped();
                    }
                }
                return true;
            });
        return r;
    }

    void ToggleCustomEquip(int ci) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }
        const auto& def = Slots::g_custom[ci];
        const auto formID = Slots::SpecToForm(def.itemSpec);
        auto* form = formID ? RE::TESForm::GetFormByID(formID) : nullptr;
        if (!form || !(form->As<RE::TESObjectWEAP>() || form->As<RE::TESObjectARMO>() ||
                         form->As<RE::AlchemyItem>())) {
            return;
        }
        const auto p = QueryCustomItem(ci);
        if (!p.carried) {
            Hud(fmt::format("VisibleFavorites: {} not in inventory", def.label), "UIMenuCancel");
            return;
        }
        auto* mgr = RE::ActorEquipManager::GetSingleton();
        if (!mgr) {
            return;
        }
        const auto* instX = p.extra ? p.extra->GetByType<RE::ExtraInstanceData>() : nullptr;
        RE::BGSObjectInstance inst(form, instX ? instX->data.get() : nullptr);
        if (p.equipped) {
            mgr->UnequipObject(player, &inst, 1, nullptr, 0, false, false, true, true, nullptr);
        } else {
            mgr->EquipObject(player, inst, 0, 1, nullptr, false, false, true, true, false);
        }
        g_dirty = true;
    }

    void SetCustomEquip(int ci, bool equip) {
        const auto p = QueryCustomItem(ci);
        if (p.carried && p.equipped != equip) {
            ToggleCustomEquip(ci);
        }
    }

    void ToggleGroupEquip(int gi) {
        const auto& g = Slots::g_groups[gi];
        bool anyEquipped = false;
        bool anyMember = false;
        for (int i = 0; i < Slots::CustomCount(); ++i) {
            if (Slots::g_custom[i].group == g.id) {
                anyMember = true;
                anyEquipped = anyEquipped || QueryCustomItem(i).equipped;
            }
        }
        if (!anyMember) {
            return;
        }
        for (int i = 0; i < Slots::CustomCount(); ++i) {
            if (Slots::g_custom[i].group == g.id) {
                SetCustomEquip(i, !anyEquipped);
            }
        }
        Hud(fmt::format("VisibleFavorites: {} {}", g.label, anyEquipped ? "off" : "on"),
            anyEquipped ? "UIMenuCancel" : "UIMenuOK");
        g_dirty = true;
    }

    std::unordered_map<int, bool> g_hkPrev;

    void FireHotkey(int hk) {
        for (int i = 0; i < Slots::CustomCount(); ++i) {
            if (Slots::g_custom[i].hotkey == hk) {
                ToggleCustomEquip(i);
                return;
            }
        }
    }

    //============= Save-Load Lifecycle =============

    void ResetRuntimeForLoad() {
        std::lock_guard lock(g_tablesMutex);
        ++g_loadGeneration;
        if (auto* player3D = Player3D()) {
            for (int i = 0; i < MAX_SLOTS; ++i) {
                RetireSlot(player3D, i);
            }
        }
        for (auto& st : g_state) {
            st = SlotState{};
        }
        g_queue.clear();
        g_active = Pending{};
        g_busy = false;
        g_polls = 0;
        g_dirty = false;
        g_prevCombo = false;
        g_playerWeaponDrawn = false;
        g_noBodyArmor = false;
        g_inPA = false;
        g_playerInBed = false;
        Npc::Reset();
        for (auto& open : g_menuCapState) {
            open = false;
        }
        g_packForm = 0;
        g_armorForm = 0;
        g_paArmorForm = 0;
        g_overTorsoForm = 0;
        g_overLLegForm = 0;
        g_overRLegForm = 0;
        Overlay::g_open = false;
        Overlay::g_needCapture.store(true);
    }

    void OnPreLoadGame() {
        g_loading = true;
        ResetRuntimeForLoad();
        logger::info("save transition - old displays retired and pending work cancelled");
    }

    void OnPostLoadGame() {
        RegisterEquipSinkOnce();
        ResetRuntimeForLoad();
        g_loading = false;
        g_dirty = true;
        logger::info("save loaded - clean reconcile requested");
        if (!g_loopStarted) {
            g_loopStarted = true;
            Schedule(Tick, 2000);
        }
    }
}
