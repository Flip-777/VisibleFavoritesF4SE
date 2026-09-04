#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

//============= Slots & Config =============
namespace Slots
{
    constexpr int FAV_SLOTS = 12;
    constexpr int MAX_CUSTOM = 24;
    constexpr int MAX_SLOTS = FAV_SLOTS + MAX_CUSTOM;
    constexpr int MAX_GROUPS = 12;
    constexpr int MAX_INDEX = MAX_SLOTS + MAX_GROUPS;
    constexpr int MAX_NPC_CONFIGS = 64;

    struct Transform
    {
        float px{ 0 }, py{ 0 }, pz{ 0 };
        float rx{ 0 }, ry{ 0 }, rz{ 0 };
        float scale{ 1.0f };
        bool hidden{ false };
    };

    struct SlotDef
    {
        std::string label;
        std::string bone;
        Transform t;
    };

    constexpr Transform PA_DEFAULTS[FAV_SLOTS] = {
        { 0.2500f, -18.0000f, 7.1000f, 1.48353f, 0.08290f, -0.24871f, 1.000f },
        { 26.4900f, -15.1150f, 14.8200f, -1.76947f, 0.02714f, -1.56299f, 1.000f },
        { 11.5000f, 12.3000f, 7.7000f, -0.16825f, -0.19319f, 1.59914f, 1.000f },
        { 11.3420f, 13.7010f, -1.9730f, -3.02947f, 0.03552f, 1.50712f, 1.000f },
        { 18.1330f, -16.5830f, -16.2660f, -1.66243f, -0.00873f, -1.60101f, 1.000f },
        { 4.8000f, -15.5000f, -17.9500f, -1.62316f, -0.00873f, -0.37525f, 1.000f },
        { 13.8500f, 11.4000f, 13.8000f, -1.50988f, -0.32215f, -1.50133f, 1.000f },
        { 14.9030f, 10.4180f, -16.3610f, -1.60343f, -0.46314f, -1.56312f, 1.000f },
        { 26.2350f, -28.3600f, -5.1230f, 0.05765f, -0.28978f, -1.69224f, 1.000f },
        { 8.1700f, 15.5120f, 10.0820f, 4.37472f, -3.20519f, -1.62534f, 1.000f },
        { 19.6500f, 9.5500f, -2.1500f, 1.71948f, -0.04554f, 1.60165f, 1.000f },
        { 23.3500f, 9.4500f, 1.4000f, -4.64224f, 3.07368f, -1.52748f, 1.000f },
    };

    struct CustomSlot
    {
        int id{ 0 };
        std::string label;
        std::string itemSpec;
        std::uint64_t fingerprint{ 0 };
        bool hideNotInInventory{ true };
        bool hideWhenEquipped{ true };
        bool showOnNpc{ false };
        int group{ 0 };
        int hotkey{ 0 };
    };

    struct CustomGroup
    {
        int id{ 0 };
        std::string label;
        int color{ 0 };
        int hotkey{ 0 };
    };

    struct NpcSlotDef
    {
        std::string bone;
        Transform t;
        int families{ 0 };
        int prio{ 0 };
    };

    inline constexpr std::uint32_t ANY_ARMOR = 0xFFFFFFFFu;
    inline constexpr std::uint32_t ANY_PACK = 0xFFFFFFFEu;
    inline constexpr std::uint32_t ANY_OVER_ARMOR = 0xFFFFFFFDu;
    inline constexpr std::uint32_t ANY_PA = 0xFFFFFFFCu;

    extern SlotDef g_slots[MAX_INDEX];
    extern Transform g_slotsPA[MAX_INDEX];
    extern int g_slotFav[FAV_SLOTS];
    extern bool g_slotEnabled[FAV_SLOTS];
    extern bool g_paLoaded;
    using IniLines = std::vector<std::pair<std::string, std::string>>;

    struct LadderSet
    {
        std::map<int, Transform> packGeneric;
        std::unordered_map<std::uint32_t, std::map<int, Transform>> packOverrides;
        std::unordered_map<std::uint32_t, std::map<int, Transform>> armorOverrides;
        std::unordered_map<std::uint32_t, std::map<std::pair<int, std::uint32_t>, Transform>> weaponOverrides;
        IniLines packUnresolved;
        IniLines armorUnresolved;
        IniLines weaponUnresolved;
    };

    struct NpcTableSet : LadderSet
    {
        NpcSlotDef slots[FAV_SLOTS];
        Transform slotsPA[FAV_SLOTS];
    };

    extern LadderSet g_player;

    struct NpcConfigGroup
    {
        std::string label;
        int mode{ -1 };
        std::vector<std::string> memberSpecs;
        NpcTableSet tables;
    };

    extern NpcTableSet g_npcSet;
    extern std::vector<NpcConfigGroup> g_npcConfigs;
    extern int g_npcDisplayMode;

    NpcTableSet& NpcSetFor(std::uint32_t cfgKey);
    int NpcModeFor(std::uint32_t cfgKey);
    bool AnyNpcDisplays();
    int NpcConfigIndexFor(std::uint32_t cfgKey);
    void InvalidateNpcConfigMembership();
    void AddNpcToConfig(int configIndex, std::uint32_t cfgKey);
    void RemoveNpcFromConfigs(std::uint32_t cfgKey);
    int NewNpcConfig(std::uint32_t firstMember);
    void EnsureNpcConfigs(int count);
    void DeleteNpcConfig(int configIndex);
    extern std::vector<std::uint32_t> g_packBlacklist;
    extern std::vector<std::uint32_t> g_displayBlacklist;
    extern std::vector<CustomSlot> g_custom;
    extern int g_nextCustomId;
    extern std::vector<CustomGroup> g_groups;
    extern std::vector<int> g_backpackBipedSlots;
    extern std::vector<int> g_paTorsoBipedSlots;
    extern std::unordered_map<std::uint32_t, std::vector<std::string>> g_hiddenParts;
    extern std::unordered_map<std::uint32_t, std::string> g_specOf;
    extern bool g_displayAnyItemType;
    extern bool g_showWeaponFX;
    extern bool g_hideWhenNoBodyArmor;
    extern int g_playerDisplayMode;
    extern bool g_enableOverlay;
    extern bool g_verboseLog;
    extern bool g_enableOverlayPending;
    extern float g_panelX;
    extern float g_panelY;
    extern float g_panelW;
    extern float g_panelH;
    extern float g_overlayScale;
    extern int g_hideAllKey;
    extern int g_openKey;
    extern std::vector<std::string> g_blacklistSpecs;
    extern std::vector<std::string> g_displayBlacklistSpecs;

    void ResetNpcSlotData(NpcTableSet& set, int slot);
    void ResetAllNpcSlots(NpcTableSet& set);
    void MoveNpcSlot(NpcTableSet& set, int slot, int target);
    void RestoreBaselineTransforms();
    int CustomCount();
    int TotalSlots();
    bool IsCustom(int slot);
    CustomSlot& CustomOf(int slot);
    CustomGroup* FindGroup(int id);
    int GroupCount();
    int GroupVSlot(int id);
    int NewGroupId();
    void DeleteGroupData(int id);
    int StealHotkey(int code, bool keepIsGroup, int keepId);
    void SanitizeHotkeys();
    int SlotOfWheel(int wheel);
    void SanitizeFavMap();
    void LeaveGroup(CustomSlot& c);
    std::string_view CanonicalNodeName(std::string_view name);
    bool SameName(std::string_view a, std::string_view b);
    bool NameInList(const std::vector<std::string>& list, std::string_view name);
    std::vector<std::string> SplitCsv(const std::string& val);
    bool IsPackBlacklisted(std::uint32_t id);
    std::uint32_t BackpackSlotMask();
    std::uint32_t PATorsoSlotMask();
    bool IsBackSlot(int slot);
    int ModClassOfVk(int vk);
    int CurrentMods();
    int CurrentModsRaw();
    std::string FormToSpec(std::uint32_t id);
    std::uint32_t SpecToForm(const std::string& spec);
    const std::string& SpecKeyFor(std::uint32_t id);
    void SetPackBlacklisted(std::uint32_t id, bool blacklisted);
    bool IsDisplayBlacklisted(std::uint32_t id);
    void SetDisplayBlacklisted(std::uint32_t id, bool blacklisted);
    std::string FriendlyName(std::uint32_t id);
}
