#include "SlotManager.h"

#include "Config.h"
#include "InputHandler.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <unordered_set>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace Slots
{
    //============= Slot Tables & Defaults =============

    struct SlotBaseline
    {
        const char* bone;
        Transform t;
    };

    constexpr SlotBaseline BASE_DEFAULTS[FAV_SLOTS] = {
        { "Pelvis", { 11.2000f, -8.4490f, 5.0920f, 1.51407f, 0.08290f, -0.25744f, 1.000f } },
        { "Chest", { 20.3230f, -12.0550f, 2.2280f, -0.05470f, -0.23524f, -1.53249f, 1.000f } },
        { "LLeg_Thigh", { 10.4960f, 1.2320f, -7.4520f, 1.28543f, 0.01189f, 1.67332f, 1.000f } },
        { "RLeg_Thigh", { 10.4850f, -0.6270f, 7.8020f, 1.65637f, -0.06170f, 1.48857f, 0.935f } },
        { "Chest", { 15.7730f, -10.7050f, -13.1720f, -0.05470f, -0.25269f, -1.53249f, 1.000f } },
        { "Pelvis", { 14.6500f, -9.9990f, -12.6580f, 4.74730f, -0.13526f, -0.45815f, 0.820f } },
        { "SPINE2", { 14.0000f, 7.0000f, 12.8500f, -1.71060f, -0.50105f, -1.51879f, 1.000f } },
        { "SPINE2", { 14.3170f, 3.5090f, -11.4610f, -1.66888f, -0.50677f, -1.61984f, 1.000f } },
        { "Chest", { 19.9230f, -13.4050f, -5.8220f, -0.05470f, -0.17415f, -1.53249f, 1.000f } },
        { "Pelvis", { 9.6800f, 7.1910f, 5.3850f, 4.30491f, -3.17901f, -1.55989f, 0.730f } },
        { "RLeg_Calf", { 9.2500f, 0.9500f, 6.5500f, -6.23486f, -0.08917f, 1.56238f, 1.000f } },
        { "LLeg_Calf", { 9.9500f, 1.4000f, -6.0500f, -3.52960f, 3.06495f, -1.60602f, 1.000f } },
    };

    static std::map<int, Transform> MakeBackpackDefaults() {
        return {
            { 1, { 12.7020f, -16.2110f, 12.6720f, 1.59148f, -0.09903f, -1.59431f, 1.000f } },
            { 4, { 12.7020f, -16.2110f, -13.5780f, 1.59148f, -0.09903f, -1.59431f, 1.000f } },
            { 8, { 12.6140f, -25.4930f, -3.8010f, -3.23052f, 3.37580f, 1.67449f, 0.825f } },
        };
    }

    SlotDef g_slots[MAX_INDEX] = {
        { "L LBack", BASE_DEFAULTS[0].bone, BASE_DEFAULTS[0].t },
        { "L Back", BASE_DEFAULTS[1].bone, BASE_DEFAULTS[1].t },
        { "L Hip", BASE_DEFAULTS[2].bone, BASE_DEFAULTS[2].t },
        { "R Hip", BASE_DEFAULTS[3].bone, BASE_DEFAULTS[3].t },
        { "R Back", BASE_DEFAULTS[4].bone, BASE_DEFAULTS[4].t },
        { "R LBack", BASE_DEFAULTS[5].bone, BASE_DEFAULTS[5].t },
        { "R Chest", BASE_DEFAULTS[6].bone, BASE_DEFAULTS[6].t },
        { "L Chest", BASE_DEFAULTS[7].bone, BASE_DEFAULTS[7].t },
        { "Belt Back", BASE_DEFAULTS[8].bone, BASE_DEFAULTS[8].t },
        { "Belt Front", BASE_DEFAULTS[9].bone, BASE_DEFAULTS[9].t },
        { "R Ankle", BASE_DEFAULTS[10].bone, BASE_DEFAULTS[10].t },
        { "L Ankle", BASE_DEFAULTS[11].bone, BASE_DEFAULTS[11].t },
    };

    Transform g_slotsPA[MAX_INDEX] = {
        PA_DEFAULTS[0], PA_DEFAULTS[1], PA_DEFAULTS[2], PA_DEFAULTS[3],
        PA_DEFAULTS[4], PA_DEFAULTS[5], PA_DEFAULTS[6], PA_DEFAULTS[7],
        PA_DEFAULTS[8], PA_DEFAULTS[9], PA_DEFAULTS[10], PA_DEFAULTS[11]
    };

    int g_slotFav[FAV_SLOTS] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    bool g_slotEnabled[FAV_SLOTS] = { true, true, true, true, true, true, true, true, true, true, true, true };

    bool g_paLoaded = false;

    LadderSet g_player{ MakeBackpackDefaults() };

    static constexpr int NPC_FAMILY_DEFAULTS[FAV_SLOTS] = {
        0, 1 << 0, 0, 1 << 1, (1 << 0) | (1 << 4), 1 << 2,
        0, 0, (1 << 0) | (1 << 3) | (1 << 4), 0, 1 << 5, 0
    };
    static constexpr int NPC_PRIO_DEFAULTS[FAV_SLOTS] = { 8, 2, 4, 3, 1, 7, 5, 6, 0, 11, 9, 10 };

    static NpcSlotDef MakeNpcSlot(int i) {
        return { BASE_DEFAULTS[i].bone, BASE_DEFAULTS[i].t, NPC_FAMILY_DEFAULTS[i], NPC_PRIO_DEFAULTS[i] };
    }

    static NpcTableSet MakeDefaultNpcSet() {
        NpcTableSet set;
        for (int i = 0; i < FAV_SLOTS; ++i) {
            set.slots[i] = MakeNpcSlot(i);
            set.slotsPA[i] = PA_DEFAULTS[i];
        }
        set.packGeneric = MakeBackpackDefaults();
        return set;
    }

    NpcTableSet g_npcSet = MakeDefaultNpcSet();

    std::vector<NpcConfigGroup> g_npcConfigs;
    int g_npcDisplayMode = 0;

    static std::unordered_map<std::uint32_t, int> g_npcConfigOf;
    static bool g_npcConfigsResolved = false;

    void InvalidateNpcConfigMembership() { g_npcConfigsResolved = false; }

    static void ResolveNpcConfigMembership() {
        if (g_npcConfigsResolved) {
            return;
        }
        g_npcConfigOf.clear();
        for (int gi = 0; gi < static_cast<int>(g_npcConfigs.size()); ++gi) {
            for (const auto& spec : g_npcConfigs[gi].memberSpecs) {
                if (const auto id = SpecToForm(spec)) {
                    g_npcConfigOf.emplace(id, gi);
                }
            }
        }
        g_npcConfigsResolved = true;
    }

    int NpcConfigIndexFor(std::uint32_t cfgKey) {
        if (!cfgKey) {
            return -1;
        }
        ResolveNpcConfigMembership();
        const auto it = g_npcConfigOf.find(cfgKey);
        return it != g_npcConfigOf.end() ? it->second : -1;
    }

    NpcTableSet& NpcSetFor(std::uint32_t cfgKey) {
        const int gi = NpcConfigIndexFor(cfgKey);
        return gi >= 0 ? g_npcConfigs[gi].tables : g_npcSet;
    }

    int NpcModeFor(std::uint32_t cfgKey) {
        const int gi = NpcConfigIndexFor(cfgKey);
        if (gi >= 0 && g_npcConfigs[gi].mode >= 0) {
            return g_npcConfigs[gi].mode;
        }
        return g_npcDisplayMode;
    }

    bool AnyNpcDisplays() {
        if (g_npcDisplayMode != 2) {
            return true;
        }
        for (const auto& cfg : g_npcConfigs) {
            if (cfg.mode == 0 || cfg.mode == 1) {
                return true;
            }
        }
        return false;
    }

    void RemoveNpcFromConfigs(std::uint32_t cfgKey) {
        for (auto& cfg : g_npcConfigs) {
            std::erase_if(cfg.memberSpecs, [&](const std::string& s) { return SpecToForm(s) == cfgKey; });
        }
        InvalidateNpcConfigMembership();
    }

    void AddNpcToConfig(int configIndex, std::uint32_t cfgKey) {
        if (!cfgKey || configIndex < 0 || configIndex >= static_cast<int>(g_npcConfigs.size())) {
            return;
        }
        RemoveNpcFromConfigs(cfgKey);
        g_npcConfigs[configIndex].memberSpecs.push_back(FormToSpec(cfgKey));
    }

    static void PushNpcConfig(const NpcTableSet& tables) {
        NpcConfigGroup cfg;
        cfg.label = "Group " + std::to_string(g_npcConfigs.size() + 1);
        cfg.tables = tables;
        g_npcConfigs.push_back(std::move(cfg));
    }

    int NewNpcConfig(std::uint32_t firstMember) {
        if (static_cast<int>(g_npcConfigs.size()) >= MAX_NPC_CONFIGS) {
            return -1;
        }
        PushNpcConfig(g_npcSet);
        const int gi = static_cast<int>(g_npcConfigs.size()) - 1;
        if (firstMember) {
            AddNpcToConfig(gi, firstMember);
        }
        return gi;
    }

    void EnsureNpcConfigs(int count) {
        while (static_cast<int>(g_npcConfigs.size()) < count && static_cast<int>(g_npcConfigs.size()) < MAX_NPC_CONFIGS) {
            PushNpcConfig(MakeDefaultNpcSet());
        }
    }

    void DeleteNpcConfig(int configIndex) {
        if (configIndex >= 0 && configIndex < static_cast<int>(g_npcConfigs.size())) {
            g_npcConfigs.erase(g_npcConfigs.begin() + configIndex);
            InvalidateNpcConfigMembership();
        }
    }

    std::vector<std::uint32_t> g_packBlacklist;
    std::vector<std::uint32_t> g_displayBlacklist;

    void RestoreBaselineTransforms() {
        for (int i = 0; i < FAV_SLOTS; ++i) {
            g_slots[i].bone = BASE_DEFAULTS[i].bone;
            g_slots[i].t = BASE_DEFAULTS[i].t;
            g_slotsPA[i] = PA_DEFAULTS[i];
        }
        g_player.packGeneric = MakeBackpackDefaults();
        g_paLoaded = true;
    }

    void ResetAllNpcSlots(NpcTableSet& set) {
        set = MakeDefaultNpcSet();
    }

    void ResetNpcSlotData(NpcTableSet& set, int slot) {
        if (slot < 0 || slot >= FAV_SLOTS) {
            return;
        }
        set.slots[slot].t = BASE_DEFAULTS[slot].t;
        set.slotsPA[slot] = PA_DEFAULTS[slot];
        const auto packDefaults = MakeBackpackDefaults();
        if (const auto it = packDefaults.find(slot); it != packDefaults.end()) {
            set.packGeneric[slot] = it->second;
        } else {
            set.packGeneric.erase(slot);
        }
        for (auto it = set.packOverrides.begin(); it != set.packOverrides.end();) {
            it->second.erase(slot);
            it = it->second.empty() ? set.packOverrides.erase(it) : std::next(it);
        }
        for (auto it = set.armorOverrides.begin(); it != set.armorOverrides.end();) {
            it->second.erase(slot);
            it = it->second.empty() ? set.armorOverrides.erase(it) : std::next(it);
        }
        for (auto wit = set.weaponOverrides.begin(); wit != set.weaponOverrides.end();) {
            auto& entries = wit->second;
            for (auto it = entries.begin(); it != entries.end();) {
                it = it->first.first == slot ? entries.erase(it) : std::next(it);
            }
            wit = entries.empty() ? set.weaponOverrides.erase(wit) : std::next(wit);
        }
    }

    void MoveNpcSlot(NpcTableSet& set, int slot, int target) {
        if (slot < 0 || slot >= FAV_SLOTS || target < 0 || target >= FAV_SLOTS || slot == target) {
            return;
        }
        int order[FAV_SLOTS];
        for (int i = 0; i < FAV_SLOTS; ++i) {
            order[i] = i;
        }
        std::sort(std::begin(order), std::end(order), [&set](int a, int b) {
            return set.slots[a].prio != set.slots[b].prio ? set.slots[a].prio < set.slots[b].prio : a < b;
        });
        int srcIdx = -1;
        int dstIdx = -1;
        for (int i = 0; i < FAV_SLOTS; ++i) {
            if (order[i] == slot) {
                srcIdx = i;
            }
            if (order[i] == target) {
                dstIdx = i;
            }
        }
        const bool movingDown = srcIdx < dstIdx;
        int seq[FAV_SLOTS];
        int n = 0;
        for (int i = 0; i < FAV_SLOTS; ++i) {
            const int s = order[i];
            if (s == slot) {
                continue;
            }
            if (s == target && !movingDown) {
                seq[n++] = slot;
            }
            seq[n++] = s;
            if (s == target && movingDown) {
                seq[n++] = slot;
            }
        }
        for (int i = 0; i < n; ++i) {
            set.slots[seq[i]].prio = i;
        }
    }

    //============= Custom Slots & Groups =============

    std::vector<CustomSlot> g_custom;
    int g_nextCustomId = 1;

    int CustomCount() { return static_cast<int>(g_custom.size()); }
    int TotalSlots() { return FAV_SLOTS + CustomCount(); }
    bool IsCustom(int slot) { return slot >= FAV_SLOTS && slot < TotalSlots(); }
    CustomSlot& CustomOf(int slot) { return g_custom[slot - FAV_SLOTS]; }

    std::vector<CustomGroup> g_groups;

    int GroupCount() { return static_cast<int>(g_groups.size()); }

    CustomGroup* FindGroup(int id) {
        if (id != 0) {
            for (auto& g : g_groups) {
                if (g.id == id) {
                    return &g;
                }
            }
        }
        return nullptr;
    }

    int GroupVSlot(int id) {
        for (int i = 0; id && i < GroupCount(); ++i) {
            if (g_groups[i].id == id) {
                return MAX_SLOTS + i;
            }
        }
        return -1;
    }

    int NewGroupId() {
        int id = 1;
        while (FindGroup(id)) {
            ++id;
        }
        return id;
    }

    void DeleteGroupData(int id);

    void LeaveGroup(CustomSlot& c) {
        if (c.group == 0) {
            return;
        }
        const int gid = c.group;
        c.group = 0;
        int remaining = 0;
        for (const auto& other : g_custom) {
            remaining += other.group == gid ? 1 : 0;
        }
        if (remaining == 0) {
            DeleteGroupData(gid);
        }
    }

    //============= Override Tables & Blacklists =============

    std::vector<int> g_backpackBipedSlots = { 36, 54, 55, 61 };
    std::vector<int> g_paTorsoBipedSlots = { 41 };

    std::unordered_map<std::uint32_t, std::vector<std::string>> g_hiddenParts;

    bool SameName(std::string_view a, std::string_view b) {
        return a.size() == b.size() && _strnicmp(a.data(), b.data(), a.size()) == 0;
    }

    bool NameInList(const std::vector<std::string>& list, std::string_view name) {
        for (const auto& e : list) {
            if (SameName(e, name)) {
                return true;
            }
        }
        return false;
    }

    std::vector<std::string> SplitCsv(const std::string& val) {
        std::vector<std::string> out;
        std::stringstream ss(val);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            while (!tok.empty() && tok.back() == ' ') {
                tok.pop_back();
            }
            while (!tok.empty() && tok.front() == ' ') {
                tok.erase(tok.begin());
            }
            if (!tok.empty()) {
                out.push_back(tok);
            }
        }
        return out;
    }

    std::string_view CanonicalNodeName(std::string_view name) {
        for (;;) {
            const auto colon = name.rfind(':');
            if (colon == std::string_view::npos || colon + 1 >= name.size()) {
                break;
            }
            bool digits = true;
            for (auto i = colon + 1; i < name.size(); ++i) {
                digits = digits && name[i] >= '0' && name[i] <= '9';
            }
            if (!digits) {
                break;
            }
            name = name.substr(0, colon);
        }
        return name;
    }

    bool IsPackBlacklisted(std::uint32_t id) {
        return std::find(g_packBlacklist.begin(), g_packBlacklist.end(), id) != g_packBlacklist.end();
    }

    static std::uint32_t SlotListMask(const std::vector<int>& slots) {
        std::uint32_t m = 0;
        for (int s : slots) {
            if (s >= 30 && s < 62) {
                m |= 1u << (s - 30);
            }
        }
        return m;
    }

    std::uint32_t BackpackSlotMask() { return SlotListMask(g_backpackBipedSlots); }
    std::uint32_t PATorsoSlotMask() { return SlotListMask(g_paTorsoBipedSlots); }

    bool IsBackSlot(int slot) { return slot == 1 || slot == 4 || slot == 8; }

    //============= Settings & Hotkeys =============

    bool g_displayAnyItemType = false;
    bool g_showWeaponFX = false;
    bool g_hideWhenNoBodyArmor = true;
    int g_playerDisplayMode = 0;
    bool g_enableOverlay = true;
    bool g_verboseLog = false;
    bool g_enableOverlayPending = true;
    float g_panelX = -1.0f;
    float g_panelY = -1.0f;
    float g_panelW = 0.0f;
    float g_panelH = 0.0f;
    float g_overlayScale = 1.4f;
    int ModClassOfVk(int vk) {
        switch (vk) {
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return 0x20000;
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return 0x40000;
        case VK_MENU: case VK_LMENU: case VK_RMENU: return 0x80000;
        }
        return 0;
    }

    int CurrentMods() {
        return (EngineInput::g_keyDown[VK_CONTROL] ? 0x20000 : 0) |
               (EngineInput::g_keyDown[VK_SHIFT] ? 0x40000 : 0) |
               (EngineInput::g_keyDown[VK_MENU] ? 0x80000 : 0);
    }

    int CurrentModsRaw() {
        return ((GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 0x20000 : 0) |
               ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 0x40000 : 0) |
               ((GetAsyncKeyState(VK_MENU) & 0x8000) ? 0x80000 : 0);
    }
    int g_hideAllKey = 0x40048;
    int g_openKey = 0x400A3;

    int StealHotkey(int code, bool keepIsGroup, int keepId) {
        if (!code) {
            return 0;
        }
        int stolen = 0;
        for (auto& g : g_groups) {
            if (g.hotkey == code && !(keepIsGroup && g.id == keepId)) {
                g.hotkey = 0;
                ++stolen;
                logger::info("hotkey steal: group '{}' unbound", g.label);
            }
        }
        for (auto& c : g_custom) {
            if (c.hotkey == code && !(!keepIsGroup && c.id == keepId)) {
                c.hotkey = 0;
                ++stolen;
                logger::info("hotkey steal: slot '{}' unbound", c.label);
            }
        }
        return stolen;
    }

    void SanitizeHotkeys() {
        std::unordered_set<int> seen;
        int cleared = 0;
        for (auto& g : g_groups) {
            if (g.hotkey && !seen.insert(g.hotkey).second) {
                g.hotkey = 0;
                ++cleared;
            }
        }
        for (auto& c : g_custom) {
            if (c.hotkey && !seen.insert(c.hotkey).second) {
                c.hotkey = 0;
                ++cleared;
            }
        }
        if (cleared) {
            logger::info("hotkey sanitize: {} duplicate bind(s) cleared (one key per slot; groups win)", cleared);
        }
    }

    int SlotOfWheel(int wheel) {
        for (int s = 0; s < FAV_SLOTS; ++s) {
            if (g_slotFav[s] == wheel) {
                return s;
            }
        }
        return wheel;
    }

    void SanitizeFavMap() {
        bool used[FAV_SLOTS]{};
        int cleared = 0;
        for (int i = 0; i < FAV_SLOTS; ++i) {
            int& w = g_slotFav[i];
            if (w < 0 || w >= FAV_SLOTS || used[w]) {
                w = -1;
                ++cleared;
            } else {
                used[w] = true;
            }
        }
        int next = 0;
        for (int i = 0; i < FAV_SLOTS; ++i) {
            if (g_slotFav[i] < 0) {
                while (used[next]) {
                    ++next;
                }
                g_slotFav[i] = next;
                used[next] = true;
            }
        }
        if (cleared) {
            logger::info("fav map sanitize: {} invalid or duplicate entries reassigned", cleared);
        }
    }

    //============= Form-Spec Persistence =============

    std::unordered_map<std::uint32_t, std::string> g_specOf;
    static std::unordered_map<std::string, std::uint32_t> g_formOfSpec;
    std::vector<std::string> g_blacklistSpecs;
    std::vector<std::string> g_displayBlacklistSpecs;

    std::string FormToSpec(std::uint32_t id) {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (dh) {
            if ((id >> 24) == 0xFE) {
                if (const auto* f = dh->LookupLoadedLightModByIndex(static_cast<std::uint16_t>((id >> 12) & 0xFFF)); f) {
                    return fmt::format("{}|{:X}", f->GetFilename(), id & 0xFFF);
                }
            } else if (const auto* f = dh->LookupLoadedModByIndex(static_cast<std::uint8_t>(id >> 24)); f) {
                return fmt::format("{}|{:X}", f->GetFilename(), id & 0xFFFFFF);
            }
        }
        return fmt::format("{:08X}", id);
    }

    std::uint32_t SpecToForm(const std::string& spec) {
        const auto bar = spec.rfind('|');
        if (bar == std::string::npos) {
            return static_cast<std::uint32_t>(std::strtoul(spec.c_str(), nullptr, 16));
        }
        if (const auto it = g_formOfSpec.find(spec); it != g_formOfSpec.end()) {
            return it->second;
        }
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            return 0;
        }
        const auto local = static_cast<std::uint32_t>(std::strtoul(spec.c_str() + bar + 1, nullptr, 16));
        const auto id = dh->LookupFormID(local, std::string_view(spec.data(), bar));
        g_formOfSpec.emplace(spec, id);
        return id;
    }

    const std::string& SpecKeyFor(std::uint32_t id) {
        auto it = g_specOf.find(id);
        if (it == g_specOf.end()) {
            it = g_specOf.emplace(id, FormToSpec(id)).first;
        }
        return it->second;
    }

    static void SetListed(std::vector<std::uint32_t>& ids, std::vector<std::string>& specs, std::uint32_t id, bool listed) {
        std::erase(ids, id);
        std::erase_if(specs, [&](const std::string& s) { return SpecToForm(s) == id; });
        if (listed) {
            ids.push_back(id);
            specs.push_back(SpecKeyFor(id));
        }
    }

    void SetPackBlacklisted(std::uint32_t id, bool blacklisted) {
        SetListed(g_packBlacklist, g_blacklistSpecs, id, blacklisted);
    }

    bool IsDisplayBlacklisted(std::uint32_t id) {
        return std::find(g_displayBlacklist.begin(), g_displayBlacklist.end(), id) != g_displayBlacklist.end();
    }

    void SetDisplayBlacklisted(std::uint32_t id, bool blacklisted) {
        SetListed(g_displayBlacklist, g_displayBlacklistSpecs, id, blacklisted);
    }

    std::string FriendlyName(std::uint32_t id) {
        switch (id) {
        case ANY_ARMOR:
            return "any armor";
        case ANY_PACK:
            return "any backpack";
        case ANY_OVER_ARMOR:
            return "any over-armor";
        case ANY_PA:
            return "any power armor";
        }
        auto* form = RE::TESForm::GetFormByID(id);
        if (!form) {
            return {};
        }
        if (auto* ref = form->As<RE::TESObjectREFR>()) {
            const char* name = ref->GetDisplayFullName();
            return name && name[0] ? IniSafe(name) : std::string{};
        }
        if (const auto name = RE::TESFullName::GetFullName(*form); !name.empty()) {
            return IniSafe(std::string(name));
        }
        return {};
    }

    void DeleteGroupData(int id) {
        int idx = -1;
        for (int i = 0; i < GroupCount(); ++i) {
            if (g_groups[i].id == id) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            return;
        }
        const int vslot = MAX_SLOTS + idx;
        for (auto& c : g_custom) {
            if (c.group == id) {
                c.group = 0;
            }
        }
        const auto remap = [&](int s) { return s <= vslot ? s : s - 1; };
        for (auto& [k, m] : g_player.armorOverrides) {
            std::map<int, Transform> next;
            for (const auto& [s, t] : m) {
                if (s != vslot) {
                    next[remap(s)] = t;
                }
            }
            m = std::move(next);
        }
        for (auto& [k, m] : g_player.packOverrides) {
            std::map<int, Transform> next;
            for (const auto& [s, t] : m) {
                if (s != vslot) {
                    next[remap(s)] = t;
                }
            }
            m = std::move(next);
        }
        for (auto& [k, m] : g_player.weaponOverrides) {
            std::map<std::pair<int, std::uint32_t>, Transform> next;
            for (const auto& [s, t] : m) {
                if (s.first != vslot) {
                    next[{ remap(s.first), s.second }] = t;
                }
            }
            m = std::move(next);
        }
        for (int s = vslot; s + 1 < MAX_SLOTS + GroupCount(); ++s) {
            g_slots[s] = g_slots[s + 1];
            g_slotsPA[s] = g_slotsPA[s + 1];
        }
        g_slots[MAX_SLOTS + GroupCount() - 1] = SlotDef{};
        g_slotsPA[MAX_SLOTS + GroupCount() - 1] = Transform{};
        g_groups.erase(g_groups.begin() + idx);
    }
}
