#include "Panel.h"

#include "Config.h"
#include "DisplayManager.h"
#include "EngineCalls.h"
#include "Hooks.h"
#include "NpcDisplay.h"
#include "SlotManager.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <set>
#include <unordered_set>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <d3d11.h>

#include <imgui.h>

#pragma warning(push, 0)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>
#pragma warning(pop)

namespace Overlay
{
    //============= Override Layer Ladder =============
    enum class Layer : int
    {
        Default,
        Armor,
        ArmorAnyItem,
        ArmorItem,
        OverArmor,
        OverArmorAnyItem,
        OverArmorItem,
        PackAny,
        Pack,
        PackAnyItem,
        PackItem,
        PABase,
        PAArmor,
        PAAnyItem,
        PAItem
    };
    constexpr Layer LAYER_ORDER[] = { Layer::Default, Layer::Armor, Layer::ArmorAnyItem,
        Layer::ArmorItem, Layer::OverArmor, Layer::OverArmorAnyItem, Layer::OverArmorItem,
        Layer::PackAny, Layer::Pack, Layer::PackAnyItem, Layer::PackItem,
        Layer::PABase, Layer::PAArmor, Layer::PAAnyItem, Layer::PAItem };
    constexpr float RAD2DEG = 57.295779513f;
    constexpr const char* DISPLAY_MODES[4] = { "Use Global", "All", "Equipped Only", "Disabled" };

    //============= Panel State =============
    std::atomic<bool> g_open{ false };
    std::atomic<bool> g_kbOwned{ false };
    std::atomic<bool> g_unsaved{ false };
    std::atomic<bool> g_confirmClose{ false };
    int g_slot = 3;
    int g_page = 0;

    struct PickItem
    {
        RE::TESBoundObject* object{ nullptr };
        std::uint64_t hash{ 0 };
        std::string name;
        int type{ 0 };
    };
    constexpr const char* ITEM_CATS[] = { "All", "Weapon", "Armor", "Aid", "Misc", "Book", "Ammo" };
    std::vector<PickItem> g_pickItems;
    std::vector<PickItem> g_pickItemsAll;
    std::uint32_t g_pickGen = 0;
    bool g_pickAll = true;
    bool g_pickAllBuilt = false;
    int g_pickCategory = 0;
    std::vector<std::string> g_pickNodes;
    bool g_showAllBones = false;
    char g_itemFilter[64] = {};
    char g_nodeFilter[64] = {};
    int g_pickedItem = -1;
    int g_bindSlot = -1;
    std::atomic<bool> g_pickBusy{ false };
    bool g_addPopupPending = false;
    bool g_blPopupPending = false;

    int ItemCategoryOf(RE::TESBoundObject* obj) {
        if (!obj || obj->As<RE::TESKey>() || obj->As<RE::BGSNote>()) {
            return 0;
        }
        if (obj->As<RE::TESObjectWEAP>()) {
            return 1;
        }
        if (obj->As<RE::TESObjectARMO>()) {
            return 2;
        }
        if (obj->As<RE::AlchemyItem>()) {
            return 3;
        }
        if (obj->As<RE::TESObjectMISC>()) {
            return 4;
        }
        if (obj->As<RE::TESObjectBOOK>()) {
            return 5;
        }
        if (obj->As<RE::TESAmmo>()) {
            return 6;
        }
        return 0;
    }
    constexpr ImU32 GROUP_COLORS[] = {
        IM_COL32(235, 180, 60, 255),
        IM_COL32(90, 200, 250, 255),
        IM_COL32(235, 110, 110, 255),
        IM_COL32(170, 140, 255, 255),
        IM_COL32(240, 140, 200, 255),
        IM_COL32(200, 210, 120, 255),
        IM_COL32(120, 160, 255, 255),
        IM_COL32(120, 220, 200, 255),
    };
    Slots::Transform g_clip{};
    bool g_hasClip = false;
    bool AnyNpcDirty();
    void SaveAllPlayerDirty();
    void SaveAllNpcDirty();
    void DiscardNpcEdits();
    void KickNpcReapply();
    float g_lastW = 0.0f;
    float g_lastH = 0.0f;
    float g_lastX = 0.0f;
    float g_lastY = 0.0f;

    Slots::Transform g_workAll[Slots::MAX_INDEX]{};
    Slots::Transform g_origAll[Slots::MAX_INDEX]{};
    bool g_slotCaptured[Slots::MAX_INDEX]{};
    bool g_slotDirty[Slots::MAX_INDEX]{};
    Layer g_slotLayer[Slots::MAX_INDEX]{};
    std::atomic<bool> g_needCapture{ true };
    Layer g_layerSel = Layer::Default;

    //============= Layer Resolution & Storage =============
    struct LayerEnv
    {
        Display::LadderTables tab;
        Npc::WearCtx wear;
        std::uint32_t item{ 0 };
        bool inPA{ false };
        std::uint32_t paArmor{ 0 };
        bool npc{ false };
    };

    LayerEnv PlayerEnv(int slot) {
        using namespace Display;
        return { PlayerTables(), PlayerWearCtx(), g_state[slot].formID, g_inPA, g_paArmorForm, false };
    }

    std::uint32_t LayerOver(const LayerEnv& e, int slot) {
        return (slot == 2 || slot == 11) ? e.wear.overLLeg :
               (slot == 3 || slot == 10) ? e.wear.overRLeg :
                                           e.wear.overTorso;
    }

    std::uint32_t LayerCtxForm(const LayerEnv& e, Layer l, int slot) {
        switch (l) {
        case Layer::Armor:
        case Layer::ArmorItem:
            return e.wear.armor;
        case Layer::ArmorAnyItem:
            return Slots::ANY_ARMOR;
        case Layer::OverArmor:
        case Layer::OverArmorItem:
            return LayerOver(e, slot);
        case Layer::OverArmorAnyItem:
            return Slots::ANY_OVER_ARMOR;
        case Layer::Pack:
        case Layer::PackItem:
            return e.wear.pack;
        case Layer::PackAnyItem:
            return Slots::ANY_PACK;
        case Layer::PAArmor:
        case Layer::PAItem:
            return e.paArmor;
        case Layer::PAAnyItem:
            return Slots::ANY_PA;
        default:
            return 0;
        }
    }

    bool LayerApplicable(const LayerEnv& e, Layer l, int slot) {
        const bool item = e.item != 0;
        switch (l) {
        case Layer::Default:
            return true;
        case Layer::Armor:
            return !e.inPA && e.wear.armor != 0;
        case Layer::ArmorAnyItem:
        case Layer::ArmorItem:
            return !e.inPA && e.wear.armor != 0 && item;
        case Layer::OverArmor:
            return !e.inPA && LayerOver(e, slot) != 0;
        case Layer::OverArmorAnyItem:
        case Layer::OverArmorItem:
            return !e.inPA && LayerOver(e, slot) != 0 && item;
        case Layer::PackAny:
        case Layer::Pack:
            return !e.inPA && e.wear.pack != 0 && Slots::IsBackSlot(slot);
        case Layer::PackAnyItem:
        case Layer::PackItem:
            return !e.inPA && e.wear.pack != 0 && Slots::IsBackSlot(slot) && item;
        case Layer::PABase:
            return e.inPA;
        case Layer::PAArmor:
            return e.inPA && e.paArmor != 0;
        case Layer::PAAnyItem:
            return e.inPA && item;
        case Layer::PAItem:
            return e.inPA && e.paArmor != 0 && item;
        }
        return false;
    }

    Slots::Transform* LayerGet(const LayerEnv& e, Layer l, int slot) {
        switch (l) {
        case Layer::Default:
            return &e.tab.Default(slot);
        case Layer::PABase:
            return &e.tab.slotsPA[slot];
        case Layer::PackAny:
            if (auto it = e.tab.set.packGeneric.find(slot); it != e.tab.set.packGeneric.end()) {
                return &it->second;
            }
            return nullptr;
        case Layer::Pack:
            if (auto pit = e.tab.set.packOverrides.find(e.wear.pack); pit != e.tab.set.packOverrides.end()) {
                if (auto it = pit->second.find(slot); it != pit->second.end()) {
                    return &it->second;
                }
            }
            return nullptr;
        case Layer::Armor:
        case Layer::OverArmor:
        case Layer::PAArmor:
            if (auto ait = e.tab.set.armorOverrides.find(LayerCtxForm(e, l, slot)); ait != e.tab.set.armorOverrides.end()) {
                if (auto it = ait->second.find(slot); it != ait->second.end()) {
                    return &it->second;
                }
            }
            return nullptr;
        case Layer::ArmorAnyItem:
        case Layer::ArmorItem:
        case Layer::OverArmorAnyItem:
        case Layer::OverArmorItem:
        case Layer::PackAnyItem:
        case Layer::PackItem:
        case Layer::PAAnyItem:
        case Layer::PAItem:
            return Display::WeaponAt(e.tab, slot, e.item, LayerCtxForm(e, l, slot));
        }
        return nullptr;
    }

    bool LayerExists(const LayerEnv& e, Layer l, int slot) { return LayerGet(e, l, slot) != nullptr; }

    Layer Winner(const LayerEnv& e, int slot) {
        if (e.inPA) {
            for (Layer l : { Layer::PAItem, Layer::PAAnyItem, Layer::PAArmor }) {
                if (LayerApplicable(e, l, slot) && LayerExists(e, l, slot)) {
                    return l;
                }
            }
            return Layer::PABase;
        }
        for (Layer l : { Layer::PackItem, Layer::PackAnyItem, Layer::Pack, Layer::PackAny,
                 Layer::OverArmorItem, Layer::OverArmorAnyItem, Layer::OverArmor,
                 Layer::ArmorItem, Layer::ArmorAnyItem, Layer::Armor }) {
            if (LayerApplicable(e, l, slot) && LayerExists(e, l, slot)) {
                return l;
            }
        }
        return Layer::Default;
    }

    Slots::Transform* LayerActive(const LayerEnv& e, int slot) { return LayerGet(e, Winner(e, slot), slot); }

    std::atomic<bool> g_savePending{ false };

    void SaveDeferred() {
        if (g_savePending.exchange(true)) {
            return;
        }
        logger::info("settings save queued");
        Display::Schedule([]() {
            std::lock_guard lock(Display::g_tablesMutex);
            g_savePending = false;
            Slots::Save();
        }, 0);
    }

    void LayerChanged(const LayerEnv& e, int slot) {
        SaveDeferred();
        if (!e.npc) {
            Display::Schedule([slot]() { Display::ReapplySlotTransform(slot); }, 0);
        }
        Display::Schedule([]() { Npc::ReapplyTransforms(); }, 0);
    }

    const char* LayerNum(Layer l);
    const char* LayerName(Layer l);

    void WriteLayer(const LayerEnv& e, Layer l, int slot, const Slots::Transform& t) {
        switch (l) {
        case Layer::Default:
            e.tab.Default(slot) = t;
            break;
        case Layer::PABase:
            e.tab.slotsPA[slot] = t;
            break;
        case Layer::PackAny:
            e.tab.set.packGeneric[slot] = t;
            break;
        case Layer::Pack:
            e.tab.set.packOverrides[e.wear.pack][slot] = t;
            break;
        case Layer::Armor:
        case Layer::OverArmor:
        case Layer::PAArmor:
            e.tab.set.armorOverrides[LayerCtxForm(e, l, slot)][slot] = t;
            break;
        case Layer::ArmorAnyItem:
        case Layer::ArmorItem:
        case Layer::OverArmorAnyItem:
        case Layer::OverArmorItem:
        case Layer::PackAnyItem:
        case Layer::PackItem:
        case Layer::PAAnyItem:
        case Layer::PAItem:
            if (e.item) {
                e.tab.set.weaponOverrides[e.item][{ slot, LayerCtxForm(e, l, slot) }] = t;
            }
            break;
        }
        const Layer winner = Winner(e, slot);
        logger::info("{}slot {} layer {} ({}) saved: pos=({:.2f},{:.2f},{:.2f}) rot=({:.3f},{:.3f},{:.3f}) scale={:.3f}{} - active layer {} ({})",
            e.npc ? "npc " : "", slot, LayerNum(l), LayerName(l), t.px, t.py, t.pz, t.rx, t.ry, t.rz, t.scale,
            t.hidden ? " hidden" : "", LayerNum(winner), LayerName(winner));
        if (l != winner) {
            logger::warn("slot {} layer {} ({}) is overridden by layer {} ({}) - the saved values are kept but will not show while it applies",
                slot, LayerNum(l), LayerName(l), LayerNum(winner), LayerName(winner));
        }
        LayerChanged(e, slot);
    }

    void EraseLayer(const LayerEnv& e, Layer l, int slot) {
        switch (l) {
        case Layer::Default:
            break;
        case Layer::PABase:
            e.tab.slotsPA[slot] = e.tab.Default(slot);
            break;
        case Layer::PackAny:
            e.tab.set.packGeneric.erase(slot);
            break;
        case Layer::Pack:
            if (auto it = e.tab.set.packOverrides.find(e.wear.pack); it != e.tab.set.packOverrides.end()) {
                it->second.erase(slot);
                if (it->second.empty()) {
                    e.tab.set.packOverrides.erase(it);
                }
            }
            break;
        case Layer::Armor:
        case Layer::OverArmor:
        case Layer::PAArmor:
            if (auto it = e.tab.set.armorOverrides.find(LayerCtxForm(e, l, slot)); it != e.tab.set.armorOverrides.end()) {
                it->second.erase(slot);
                if (it->second.empty()) {
                    e.tab.set.armorOverrides.erase(it);
                }
            }
            break;
        case Layer::ArmorAnyItem:
        case Layer::ArmorItem:
        case Layer::OverArmorAnyItem:
        case Layer::OverArmorItem:
        case Layer::PackAnyItem:
        case Layer::PackItem:
        case Layer::PAAnyItem:
        case Layer::PAItem:
            if (auto it = e.tab.set.weaponOverrides.find(e.item); it != e.tab.set.weaponOverrides.end()) {
                it->second.erase({ slot, LayerCtxForm(e, l, slot) });
                if (it->second.empty()) {
                    e.tab.set.weaponOverrides.erase(it);
                }
            }
            break;
        }
        logger::info("{}slot {} layer {} ({}) removed", e.npc ? "npc " : "", slot, LayerNum(l), LayerName(l));
        LayerChanged(e, slot);
    }

    const char* LayerNum(Layer l) {
        switch (l) {
        case Layer::Default: return "1";
        case Layer::Armor: return "2";
        case Layer::ArmorAnyItem: return "3";
        case Layer::ArmorItem: return "4";
        case Layer::OverArmor: return "5";
        case Layer::OverArmorAnyItem: return "6";
        case Layer::OverArmorItem: return "7";
        case Layer::PackAny: return "8";
        case Layer::Pack: return "9";
        case Layer::PackAnyItem: return "10";
        case Layer::PackItem: return "11";
        case Layer::PABase: return "12";
        case Layer::PAArmor: return "13";
        case Layer::PAAnyItem: return "14";
        case Layer::PAItem: return "15";
        }
        return "?";
    }

    const char* LayerName(Layer l) {
        switch (l) {
        case Layer::Default: return "Default";
        case Layer::Armor: return "Armor Current";
        case Layer::ArmorAnyItem: return "Item + Any Armor";
        case Layer::ArmorItem: return "Armor Current + Item";
        case Layer::OverArmor: return "Over Armor Current";
        case Layer::OverArmorAnyItem: return "Item + Any Over Armor";
        case Layer::OverArmorItem: return "Over Armor Current + Item";
        case Layer::PackAny: return "Backpack Default";
        case Layer::Pack: return "Backpack Current";
        case Layer::PackAnyItem: return "Item + Any Backpack";
        case Layer::PackItem: return "Backpack Current + Item";
        case Layer::PABase: return "Power Armor Default";
        case Layer::PAArmor: return "PA Current Armor";
        case Layer::PAAnyItem: return "Item + Any Power Armor";
        case Layer::PAItem: return "PA Current Armor + Item";
        }
        return "?";
    }

    std::string LayerDesc(const LayerEnv& e, Layer l, int slot) {
        const auto item = [&]() { return e.item ? Slots::FriendlyName(e.item) : std::string("item"); };
        const char* noArmor = e.npc ? "preview NPC has no body armor" : "no body armor worn";
        const char* noPack = e.npc ? "preview NPC has no backpack" : "no backpack worn";
        const char* paHint = e.npc ? "NPC Power Armor pending" : "enter Power Armor";
        const auto over = LayerOver(e, slot);
        switch (l) {
        case Layer::Default:
            return "base position for all items in this slot";
        case Layer::Armor:
            return e.wear.armor ? fmt::format("all items while [{}] is worn", Slots::FriendlyName(e.wear.armor)) : noArmor;
        case Layer::ArmorAnyItem:
            return e.wear.armor ? fmt::format("[{}] while ANY body armor is worn", item()) : noArmor;
        case Layer::ArmorItem:
            return e.wear.armor ? fmt::format("[{}] while [{}] is worn", item(), Slots::FriendlyName(e.wear.armor)) : noArmor;
        case Layer::OverArmor:
            return over ? fmt::format("all items while [{}] covers this slot", Slots::FriendlyName(over)) : "no armor piece over this slot";
        case Layer::OverArmorAnyItem:
            return over ? fmt::format("[{}] while ANY armor piece covers this slot", item()) : "no armor piece over this slot";
        case Layer::OverArmorItem:
            return over ? fmt::format("[{}] while [{}] covers this slot", item(), Slots::FriendlyName(over)) : "no armor piece over this slot";
        case Layer::PackAny: {
            std::string bs;
            for (std::size_t i = 0; i < Slots::g_backpackBipedSlots.size(); ++i) {
                bs += fmt::format("{}{}", i ? "," : "", Slots::g_backpackBipedSlots[i]);
            }
            return fmt::format("all items while a backpack is worn (slot {})", bs);
        }
        case Layer::Pack:
            return e.wear.pack ? fmt::format("all items while [{}] is worn", Slots::FriendlyName(e.wear.pack)) : noPack;
        case Layer::PackAnyItem:
            return e.wear.pack ? fmt::format("[{}] while ANY backpack is worn", item()) : noPack;
        case Layer::PackItem:
            return e.wear.pack ? fmt::format("[{}] while [{}] is worn", item(), Slots::FriendlyName(e.wear.pack)) : noPack;
        case Layer::PABase:
            return "base position while in Power Armor";
        case Layer::PAArmor:
            if (!e.inPA) {
                return fmt::format("per-PA-armor override ({})", paHint);
            }
            return e.paArmor ? fmt::format("all items in [{}]", Slots::FriendlyName(e.paArmor)) : "no PA torso detected";
        case Layer::PAAnyItem:
            if (!e.inPA) {
                return fmt::format("per-item any-PA override ({})", paHint);
            }
            return fmt::format("[{}] in ANY Power Armor", item());
        case Layer::PAItem:
            if (!e.inPA) {
                return fmt::format("per-item PA override ({})", paHint);
            }
            return e.paArmor ? fmt::format("[{}] in [{}]", item(), Slots::FriendlyName(e.paArmor)) : "no PA torso detected";
        }
        return {};
    }

    //============= Body Art Textures =============
    ID3D11ShaderResourceView* g_bodySRV = nullptr;
    int g_bodyW = 0;
    int g_bodyH = 0;
    ID3D11ShaderResourceView* g_bodyPASRV = nullptr;
    int g_bodyPAW = 0;
    int g_bodyPAH = 0;

    void LoadOneTexture(const char* file, ID3D11ShaderResourceView*& srv, int& w, int& h) {
        const auto path = std::filesystem::current_path() / "Data" / "F4SE" / "Plugins" / "VisibleFavorites" / file;
        int iw = 0, ih = 0, n = 0;
        unsigned char* pix = stbi_load(path.string().c_str(), &iw, &ih, &n, 4);
        if (!pix) {
            logger::info("overlay: no {} - fallback in effect", file);
            return;
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(iw);
        td.Height = static_cast<UINT>(ih);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA srd{ pix, static_cast<UINT>(iw * 4), 0 };
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(g_device->CreateTexture2D(&td, &srd, &tex))) {
            if (SUCCEEDED(g_device->CreateShaderResourceView(tex, nullptr, &srv)) && srv) {
                w = iw;
                h = ih;
                logger::info("overlay: {} loaded ({}x{})", file, w, h);
            } else {
                logger::warn("overlay: SRV creation failed for {}", file);
            }
            tex->Release();
        }
        stbi_image_free(pix);
    }

    void LoadBodyTexture() {
        LoadOneTexture("body.png", g_bodySRV, g_bodyW, g_bodyH);
        LoadOneTexture("body_pa.png", g_bodyPASRV, g_bodyPAW, g_bodyPAH);
    }

    //============= Live Preview & Rebuild Helpers =============
    void ApplyWorkToNode(int slot, Layer layer) {
        using namespace Display;
        if (const auto* cell = LayerGet(PlayerEnv(slot), layer, slot)) {
            Npc::PreviewTransforms(cell, g_workAll[slot]);
        }
        const Slots::Transform work = g_workAll[slot];
        Display::Schedule([slot, work]() {
            using namespace Display;
            std::lock_guard lock(g_tablesMutex);
            auto* p3d = Player3D();
            if (!p3d) {
                return;
            }
            if (slot >= Slots::MAX_SLOTS) {
                if (slot - Slots::MAX_SLOTS >= Slots::GroupCount()) {
                    return;
                }
                const int gid = Slots::g_groups[slot - Slots::MAX_SLOTS].id;
                for (int i = 0; i < Slots::CustomCount(); ++i) {
                    if (Slots::g_custom[i].group != gid) {
                        continue;
                    }
                    const int m = Slots::FAV_SLOTS + i;
                    if (auto* obj = p3d->GetObjectByName(SlotNodeName(m).c_str())) {
                        ApplyTransforms(obj, &work, *ActiveTransform(m));
                    }
                }
                return;
            }
            if (auto* obj = p3d->GetObjectByName(SlotNodeName(slot).c_str())) {
                ApplyTransforms(obj, GroupAnchor(slot), work);
            }
        }, 0);
    }

    void KickReconcile() {
        Display::Schedule([]() { Display::Reconcile(); }, 50);
    }

    void ReapplyAllTransforms() {
        Display::Schedule([]() {
            for (int i = 0; i < Slots::MAX_SLOTS; ++i) {
                if (Display::g_state[i].formID) {
                    Display::ReapplySlotTransform(i);
                }
            }
        }, 50);
    }

    void RebuildSlotDisplay(int slot) {
        Display::Schedule([slot]() {
            using namespace Display;
            std::lock_guard lock(g_tablesMutex);
            if (auto* p3d = Player3D()) {
                RetireSlot(p3d, slot);
            }
            g_state[slot] = SlotState{};
            Reconcile();
        }, 50);
    }

    //============= Item & Node Pick Lists =============
    void BuildItemLists() {
        using namespace Display;
        g_pickItems.clear();
        g_pickedItem = -1;
        g_itemFilter[0] = 0;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (player && player->inventoryList) {
            std::set<std::pair<RE::TESBoundObject*, std::uint64_t>> seen;
            player->inventoryList->ForEachStack(
                [](RE::BGSInventoryItem&) { return true; },
                [&](RE::BGSInventoryItem& item, RE::BGSInventoryItem::Stack& stack) {
                    auto* obj = item.object;
                    const int cat = ItemCategoryOf(obj);
                    if (cat == 0) {
                        return true;
                    }
                    const auto hash = OmodHash(stack.extra.get());
                    if (seen.insert({ obj, hash }).second) {
                        g_pickItems.push_back({ obj, hash, SafeName(obj), cat });
                    }
                    return true;
                });
        }
        std::sort(g_pickItems.begin(), g_pickItems.end(),
            [](const PickItem& a, const PickItem& b) { return a.name < b.name; });
        for (std::size_t i = 0; i < g_pickItems.size(); ++i) {
            const bool dup = (i > 0 && g_pickItems[i - 1].name == g_pickItems[i].name) ||
                             (i + 1 < g_pickItems.size() && g_pickItems[i + 1].name == g_pickItems[i].name);
            if (dup || g_pickItems[i].hash) {
                g_pickItems[i].name += fmt::format(" [{:04X}]", g_pickItems[i].hash & 0xFFFF);
            }
        }
        ++g_pickGen;
    }

    bool MainBone(const std::string& node) {
        for (int i = 0; i < Slots::FAV_SLOTS; ++i) {
            if (Slots::SameName(node, Slots::g_slots[i].bone)) {
                return true;
            }
        }
        return false;
    }

    void BuildNodeList() {
        g_pickNodes.clear();
        g_nodeFilter[0] = 0;
        if (auto* p3d = Display::Player3D()) {
            std::unordered_set<std::string_view> seen;
            Display::Walk(p3d, [&](RE::NiAVObject* obj) {
                if (!obj->IsNode()) {
                    return false;
                }
                if (const char* nm = obj->name.c_str(); nm && nm[0] &&
                    std::string_view(nm).find("VisFavSlot") == std::string_view::npos) {
                    if (seen.insert(nm).second) {
                        g_pickNodes.emplace_back(nm);
                    }
                }
                return true;
            });
            std::sort(g_pickNodes.begin(), g_pickNodes.end());
        }
    }

    void BuildAllItemsList() {
        g_pickItemsAll.clear();
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            return;
        }
        const auto add = [&](RE::TESBoundObject* obj) {
            const int cat = ItemCategoryOf(obj);
            if (cat == 0) {
                return;
            }
            auto nm = Display::SafeName(obj);
            if (nm.empty() || nm == "<item>") {
                return;
            }
            g_pickItemsAll.push_back({ obj, 0, std::move(nm), cat });
        };
        for (auto* f : dh->GetFormArray<RE::TESObjectWEAP>()) add(f);
        for (auto* f : dh->GetFormArray<RE::TESObjectARMO>()) add(f);
        for (auto* f : dh->GetFormArray<RE::AlchemyItem>()) add(f);
        for (auto* f : dh->GetFormArray<RE::TESObjectMISC>()) add(f);
        for (auto* f : dh->GetFormArray<RE::TESObjectBOOK>()) add(f);
        for (auto* f : dh->GetFormArray<RE::TESAmmo>()) add(f);
        std::sort(g_pickItemsAll.begin(), g_pickItemsAll.end(),
            [](const PickItem& a, const PickItem& b) {
                return a.name != b.name ? a.name < b.name :
                                          a.object->GetFormID() < b.object->GetFormID();
            });
        g_pickItemsAll.erase(std::unique(g_pickItemsAll.begin(), g_pickItemsAll.end(),
                                 [](const PickItem& a, const PickItem& b) { return a.object == b.object; }),
            g_pickItemsAll.end());
        ++g_pickGen;
        logger::info("all-items picker built: {} named usable forms", g_pickItemsAll.size());
    }

    //============= Custom Slot Create/Delete =============
    void CreateCustomSlot(const PickItem& item, const std::string& node) {
        using namespace Display;
        Slots::CustomSlot c;
        c.id = Slots::g_nextCustomId++;
        c.label = SafeName(item.object).substr(0, 24);
        c.itemSpec = Slots::FormToSpec(item.object->GetFormID());
        c.fingerprint = item.hash;
        bool carried = false;
        for (const auto& e : g_pickItems) {
            if (e.object == item.object) {
                carried = true;
                break;
            }
        }
        if (!carried) {
            c.hideNotInInventory = false;
        }
        Slots::g_custom.push_back(std::move(c));
        const int slot = Slots::TotalSlots() - 1;
        Slots::g_slots[slot].label = Slots::g_custom.back().label;
        Slots::g_slots[slot].bone = node;
        Slots::g_slots[slot].t = Slots::Transform{};
        Slots::g_slotsPA[slot] = Slots::Transform{};
        g_slotCaptured[slot] = false;
        g_slotDirty[slot] = false;
        SaveDeferred();
        g_dirty = true;
        g_slot = slot;
        KickReconcile();
        logger::info("custom slot C{} created: '{}' on '{}'", Slots::g_custom.back().id,
            Slots::g_custom.back().label, node);
    }

    void DeleteCustomSlot(int slot) {
        using namespace Display;
        const int ci = slot - Slots::FAV_SLOTS;
        const int id = Slots::g_custom[ci].id;
        const int total = Slots::TotalSlots();
        for (int i = Slots::FAV_SLOTS; i < total; ++i) {
            g_state[i] = SlotState{};
            g_slotCaptured[i] = false;
            g_slotDirty[i] = false;
        }
        Display::Schedule([total]() {
            using namespace Display;
            std::lock_guard lock(g_tablesMutex);
            if (auto* p3d = Player3D()) {
                for (int i = Slots::FAV_SLOTS; i < total; ++i) {
                    RetireSlot(p3d, i);
                }
            }
        }, 0);
        const auto remap = [&](int s) { return (s >= Slots::MAX_SLOTS || s < slot) ? s : s - 1; };
        for (auto& [k, m] : Slots::g_player.armorOverrides) {
            std::map<int, Slots::Transform> next;
            for (const auto& [s, t] : m) {
                if (s != slot) {
                    next[remap(s)] = t;
                }
            }
            m = std::move(next);
        }
        for (auto& [k, m] : Slots::g_player.packOverrides) {
            std::map<int, Slots::Transform> next;
            for (const auto& [s, t] : m) {
                if (s != slot) {
                    next[remap(s)] = t;
                }
            }
            m = std::move(next);
        }
        for (auto& [k, m] : Slots::g_player.weaponOverrides) {
            std::map<std::pair<int, std::uint32_t>, Slots::Transform> next;
            for (const auto& [s, t] : m) {
                if (s.first != slot) {
                    next[{ remap(s.first), s.second }] = t;
                }
            }
            m = std::move(next);
        }
        for (int i = slot; i + 1 < Slots::TotalSlots(); ++i) {
            Slots::g_slots[i] = Slots::g_slots[i + 1];
            Slots::g_slotsPA[i] = Slots::g_slotsPA[i + 1];
        }
        Slots::g_slots[Slots::TotalSlots() - 1] = Slots::SlotDef{};
        Slots::g_slotsPA[Slots::TotalSlots() - 1] = Slots::Transform{};
        Slots::LeaveGroup(Slots::g_custom[ci]);
        Slots::g_custom.erase(Slots::g_custom.begin() + ci);
        Display::g_hkPrev.clear();
        g_bindSlot = -1;
        g_needCapture.store(true);
        SaveDeferred();
        g_dirty = true;
        if (g_slot >= Slots::TotalSlots()) {
            g_slot = Slots::CustomCount() ? Slots::TotalSlots() - 1 : 3;
        }
        KickReconcile();
        logger::info("custom slot C{} deleted", id);
    }

    //============= Shared Widgets =============
    bool ContainsNoCase(const std::string& hay, const char* needle) {
        const auto n = std::strlen(needle);
        if (n == 0) {
            return true;
        }
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; };
        for (std::size_t i = 0; i + n <= hay.size(); ++i) {
            std::size_t j = 0;
            while (j < n && lower(hay[i + j]) == lower(needle[j])) {
                ++j;
            }
            if (j == n) {
                return true;
            }
        }
        return false;
    }

    int DrawItemPickList(float cfs) {
        bool invOnly = !g_pickAll;
        if (ImGui::Checkbox("Current Inventory##invsrc", &invOnly)) {
            g_pickAll = !invOnly;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("check to show only items from your current inventory\n(unchecked = every item in the game)");
        }
        if (g_pickAll && !g_pickAllBuilt) {
            if (!g_pickBusy.exchange(true)) {
                Display::Schedule([]() {
                    std::lock_guard lock(Display::g_tablesMutex);
                    BuildAllItemsList();
                    g_pickAllBuilt = true;
                    g_pickBusy = false;
                }, 0);
            }
            ImGui::TextDisabled("building the all-items list...");
            return -1;
        }
        ImGui::SetNextItemWidth(cfs * 8.0f);
        if (ImGui::BeginCombo("##icat", ITEM_CATS[g_pickCategory])) {
            for (int c = 0; c < static_cast<int>(std::size(ITEM_CATS)); ++c) {
                if (ImGui::Selectable(ITEM_CATS[c], c == g_pickCategory)) {
                    g_pickCategory = c;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(cfs * 14.0f);
        ImGui::InputTextWithHint("##ifilter", "filter...", g_itemFilter, sizeof(g_itemFilter));
        ImGui::Separator();
        const auto& items = g_pickAll ? g_pickItemsAll : g_pickItems;
        static std::vector<int> vis;
        static std::uint32_t visGen = 0;
        static bool visAll = false;
        static int visCat = -1;
        static char visFilter[sizeof(g_itemFilter)] = {};
        if (visGen != g_pickGen || visAll != g_pickAll || visCat != g_pickCategory ||
            std::strcmp(visFilter, g_itemFilter) != 0) {
            visGen = g_pickGen;
            visAll = g_pickAll;
            visCat = g_pickCategory;
            std::snprintf(visFilter, sizeof(visFilter), "%s", g_itemFilter);
            vis.clear();
            for (int n = 0; n < static_cast<int>(items.size()); ++n) {
                if (g_pickCategory != 0 && items[n].type != g_pickCategory) {
                    continue;
                }
                if (g_itemFilter[0] && !ContainsNoCase(items[n].name, g_itemFilter)) {
                    continue;
                }
                vis.push_back(n);
            }
        }
        int picked = -1;
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(vis.size()));
        while (clip.Step()) {
            for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
                const auto& it = items[vis[row]];
                char il[224];
                std::snprintf(il, sizeof(il), "[%s] %s##pi%d",
                    ITEM_CATS[it.type], it.name.c_str(), vis[row]);
                if (ImGui::Selectable(il)) {
                    picked = vis[row];
                }
            }
        }
        return picked;
    }

    void DrawCustomStrip() {
        ImGui::SeparatorText("Custom Slots");
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        const float cfs = ImGui::GetFontSize();
        const float stripH = std::max(cfs * 8.0f, ImGui::GetWindowSize().y * 0.27f);
        ImGui::BeginChild("customstrip", ImVec2(0.0f, stripH), true);
        {
            auto* srv = (Display::g_inPA && g_bodyPASRV) ? g_bodyPASRV : g_bodySRV;
            const int texW = (Display::g_inPA && g_bodyPASRV) ? g_bodyPAW : g_bodyW;
            const int texH = (Display::g_inPA && g_bodyPASRV) ? g_bodyPAH : g_bodyH;
            if (srv && texH > 0) {
                const auto wp = ImGui::GetWindowPos();
                const auto ws = ImGui::GetWindowSize();
                const float aspect = static_cast<float>(texW) / static_cast<float>(texH);
                const float th = ws.y;
                const float tw = th * aspect;
                const float tx = wp.x + (ws.x - tw) * 0.5f;
                ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(srv),
                    ImVec2(tx, wp.y), ImVec2(tx + tw, wp.y + th),
                    ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 48));
            }
        }
        const float cellW = cfs * 6.4f;
        const float cellH = cfs * 2.9f;
        const float underH = cfs * 1.35f;
        const int perRow = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (cellW + cfs * 0.45f)));
        int col = 0;
        const auto place = [&]() {
            if (col > 0 && col % perRow != 0) {
                ImGui::SameLine();
            }
            ++col;
        };
        auto* font = ImGui::GetFont();
        auto* dlc = ImGui::GetWindowDrawList();
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, cfs * 0.32f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.40f, 0.56f, 0.76f, 0.60f));
        std::vector<int> order;
        {
            std::vector<bool> placed(static_cast<std::size_t>(Slots::CustomCount()), false);
            for (int i = 0; i < Slots::CustomCount(); ++i) {
                if (placed[i]) {
                    continue;
                }
                order.push_back(i);
                placed[i] = true;
                if (const int gid = Slots::g_custom[i].group; gid != 0) {
                    for (int j = i + 1; j < Slots::CustomCount(); ++j) {
                        if (!placed[j] && Slots::g_custom[j].group == gid) {
                            order.push_back(j);
                            placed[j] = true;
                        }
                    }
                }
            }
        }
        for (const int i : order) {
            const int slot = Slots::FAV_SLOTS + i;
            place();
            ImGui::BeginGroup();
            const bool sel = g_slot == slot;
            if (sel) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.42f, 0.72f, 1.0f));
            }
            char bid[24];
            std::snprintf(bid, sizeof(bid), "##cs%d", Slots::g_custom[i].id);
            if (ImGui::Button(bid, ImVec2(cellW, cellH))) {
                g_slot = slot;
            }
            const auto* grp = Slots::FindGroup(Slots::g_custom[i].group);
            if (ImGui::IsItemHovered()) {
                const std::string groupLine = grp ? fmt::format("\ngroup: {}", grp->label) : std::string{};
                ImGui::SetTooltip("%s\nbone: %s\nhotkey: %s%s%s", Slots::g_custom[i].label.c_str(),
                    Slots::g_slots[slot].bone.c_str(),
                    Display::HotkeyName(Slots::g_custom[i].hotkey).c_str(),
                    groupLine.c_str(),
                    g_slotDirty[slot] ? "\n(* = unsaved changes)" : "");
            }
            if (sel) {
                ImGui::PopStyleColor();
            }
            const auto rmin = ImGui::GetItemRectMin();
            if (grp) {
                const ImU32 gc = GROUP_COLORS[static_cast<std::size_t>(grp->color) % std::size(GROUP_COLORS)];
                dlc->AddRectFilled(ImVec2(rmin.x + 2.0f, rmin.y + cellH - cfs * 0.24f),
                    ImVec2(rmin.x + cellW - 2.0f, rmin.y + cellH - cfs * 0.10f), gc, cfs * 0.06f);
            }
            const float pad = cfs * 0.4f;
            const float tsz = cfs * 0.92f;
            const float wrapW = cellW - pad * 2.0f;
            char boxText[80];
            std::snprintf(boxText, sizeof(boxText), "%s%.72s", g_slotDirty[slot] ? "*" : "", Slots::g_custom[i].label.c_str());
            const auto tdim = font->CalcTextSizeA(tsz, std::numeric_limits<float>::max(), wrapW, boxText);
            const ImVec4 clip(rmin.x + pad * 0.4f, rmin.y + pad * 0.25f,
                rmin.x + cellW - pad * 0.4f, rmin.y + cellH - pad * 0.25f);
            dlc->AddText(font, tsz,
                ImVec2(rmin.x + std::max(pad, (cellW - tdim.x) * 0.5f),
                    rmin.y + std::max(pad * 0.4f, (cellH - tdim.y) * 0.5f)),
                IM_COL32(255, 255, 255, 235), boxText, nullptr, wrapW, &clip);
            const float nsz = cfs * 0.92f;
            const int hk = Slots::g_custom[i].hotkey;
            if (hk) {
                std::string under = Display::HotkeyName(hk);
                const float maxUnderW = cellW - cfs * 0.8f;
                float uw = font->CalcTextSizeA(nsz, std::numeric_limits<float>::max(), 0.0f, under.c_str()).x;
                if (uw > maxUnderW) {
                    under += "..";
                    while (under.size() > 4 && uw > maxUnderW) {
                        under.erase(under.size() - 3, 1);
                        uw = font->CalcTextSizeA(nsz, std::numeric_limits<float>::max(), 0.0f, under.c_str()).x;
                    }
                }
                const float bw = uw + cfs * 0.6f;
                const float bh = nsz + cfs * 0.28f;
                const ImVec2 b0(rmin.x + (cellW - bw) * 0.5f, rmin.y + cellH + cfs * 0.12f);
                const ImVec2 b1(b0.x + bw, b0.y + bh);
                dlc->AddRectFilled(b0, b1, IM_COL32(22, 34, 28, 225), bh * 0.28f);
                dlc->AddRect(b0, b1, IM_COL32(95, 195, 105, 190), bh * 0.28f);
                dlc->AddText(font, nsz, ImVec2(b0.x + cfs * 0.3f, b0.y + (bh - nsz) * 0.5f),
                    IM_COL32(150, 235, 150, 255), under.c_str());
            }
            ImGui::Dummy(ImVec2(cellW, underH));
            ImGui::EndGroup();
        }
        if (Slots::CustomCount() < Slots::MAX_CUSTOM) {
            place();
            if (ImGui::Button("+ Add##csadd", ImVec2(cellW, cellH)) && !g_pickBusy.exchange(true)) {
                g_addPopupPending = true;
                Display::Schedule([]() {
                    std::lock_guard lock(Display::g_tablesMutex);
                    BuildItemLists();
                    BuildNodeList();
                    g_pickBusy = false;
                }, 0);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("add a new display slot: pick an item, then a body node");
            }
            if (g_addPopupPending && !g_pickBusy.load()) {
                g_addPopupPending = false;
                ImGui::OpenPopup("Add Custom Slot");
            }
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        if (ImGui::BeginPopupModal("Add Custom Slot", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (g_pickedItem < 0) {
                ImGui::TextUnformatted("1. Pick the item to display");
                ImGui::BeginChild("##addlist", ImVec2(cfs * 28.0f, cfs * 18.0f), true);
                const int p = DrawItemPickList(cfs);
                ImGui::EndChild();
                if (p >= 0) {
                    g_pickedItem = p;
                }
            } else {
                const auto& src = g_pickAll ? g_pickItemsAll : g_pickItems;
                ImGui::Text("Item: %s", src[g_pickedItem].name.c_str());
                ImGui::TextUnformatted("2. Pick the body node it attaches to");
                ImGui::SetNextItemWidth(cfs * 24.0f);
                ImGui::InputTextWithHint("##nfilter", "filter... (Pelvis, Chest, SPINE2...)", g_nodeFilter, sizeof(g_nodeFilter));
                ImGui::Checkbox("Show all bone nodes##addall", &g_showAllBones);
                if (ImGui::BeginListBox("##nlist", ImVec2(cfs * 26.0f, cfs * 15.0f))) {
                    for (std::size_t n = 0; n < g_pickNodes.size(); ++n) {
                        if (g_nodeFilter[0] && !ContainsNoCase(g_pickNodes[n], g_nodeFilter)) {
                            continue;
                        }
                        if (!g_showAllBones && !g_nodeFilter[0] && !MainBone(g_pickNodes[n])) {
                            continue;
                        }
                        char nl[160];
                        std::snprintf(nl, sizeof(nl), "%.120s##pn%zu", g_pickNodes[n].c_str(), n);
                        if (ImGui::Selectable(nl)) {
                            CreateCustomSlot(src[g_pickedItem], g_pickNodes[n]);
                            ImGui::CloseCurrentPopup();
                        }
                    }
                    ImGui::EndListBox();
                }
                if (ImGui::Button("Back##addback")) {
                    g_pickedItem = -1;
                }
                ImGui::SameLine();
            }
            if (ImGui::Button("Cancel##addcancel")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::EndChild();
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
    }

    void BindKeyRow(const char* label, float labelCol, int sentinel, int& key,
        bool allowPad, bool allowClear, bool combo, const char* tooltip) {
        static std::unordered_map<int, std::pair<int, int>> capState;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(std::max(labelCol, ImGui::CalcTextSize(label).x + ImGui::GetFontSize() * 0.8f));
        ImGui::PushID(sentinel);
        if (g_bindSlot == sentinel) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.85f, 0.40f, 1.0f));
            if (ImGui::Button(allowPad ? "press a key or pad button...##bind" : "press a key...##bind")) {
                g_bindSlot = -1;
                capState.erase(sentinel);
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(combo ? "esc cancels; hold modifiers with a key for a combo,\nor press modifiers alone and release" : "esc cancels");
            }
            bool done = false;
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                g_bindSlot = -1;
                capState.erase(sentinel);
                done = true;
            }
            const int curMods = Slots::CurrentModsRaw();
            auto& cap = capState[sentinel];
            if (!done && combo) {
                cap.first |= curMods;
                for (int vk = VK_LSHIFT; vk <= VK_RMENU; ++vk) {
                    if (GetAsyncKeyState(vk) & 0x8000) {
                        cap.second = vk;
                    }
                }
            }
            for (int vk = 0x08; vk <= 0xFE && !done; ++vk) {
                if (vk == VK_ESCAPE || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
                    (vk >= VK_LSHIFT && vk <= VK_RMENU)) {
                    if (!combo && vk >= VK_LSHIFT) {
                        continue;
                    }
                    if (!combo && (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) &&
                        (GetAsyncKeyState(vk) & 0x8000)) {
                        key = vk;
                        g_bindSlot = -1;
                        SaveDeferred();
                        done = true;
                    }
                    continue;
                }
                if (GetAsyncKeyState(vk) & 0x8000) {
                    key = combo ? (vk | cap.first | curMods) : vk;
                    g_bindSlot = -1;
                    capState.erase(sentinel);
                    SaveDeferred();
                    done = true;
                }
            }
            if (!done && combo && curMods == 0 && cap.second != 0) {
                key = cap.second | (cap.first & ~Slots::ModClassOfVk(cap.second));
                g_bindSlot = -1;
                capState.erase(sentinel);
                SaveDeferred();
                done = true;
            }
            if (!done && g_bindSlot == sentinel && allowPad) {
                if (const auto pads = Display::ReadPadButtons(); pads != 0) {
                    key = 0x10000 | static_cast<int>(pads & (~pads + 1));
                    g_bindSlot = -1;
                    capState.erase(sentinel);
                    SaveDeferred();
                }
            }
        } else {
            if (key) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.88f, 0.45f, 1.0f));
            }
            char btn[96];
            std::snprintf(btn, sizeof(btn), "%s##bind",
                key ? Display::HotkeyName(key).c_str() : "Bind");
            if (ImGui::Button(btn)) {
                g_bindSlot = sentinel;
                capState.erase(sentinel);
            }
            if (key) {
                ImGui::PopStyleColor();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", tooltip);
            }
            if (allowClear) {
                ImGui::SameLine();
                const bool unbound = key == 0;
                if (unbound) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Button("Clear##bindclr")) {
                    key = 0;
                    SaveDeferred();
                }
                if (unbound) {
                    ImGui::EndDisabled();
                }
            }
        }
        ImGui::PopID();
    }

    void EndPanel(bool stayOpen) {
        if (!stayOpen && g_unsaved.load()) {
            g_confirmClose = true;
            stayOpen = true;
        }
        if (g_confirmClose.load() && !ImGui::IsPopupOpen("Close without saving?")) {
            ImGui::OpenPopup("Close without saving?");
        }
        if (ImGui::BeginPopupModal("Close without saving?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("There are unsaved layer changes (*).");
            ImGui::TextUnformatted("Closing now discards them.");
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            if (ImGui::Button("Save & Close##ccsave")) {
                logger::info("close dialog: Save & Close");
                SaveAllPlayerDirty();
                SaveAllNpcDirty();
                g_confirmClose = false;
                ImGui::CloseCurrentPopup();
                stayOpen = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Close Anyway##ccdrop")) {
                logger::info("close dialog: Close Anyway - unsaved edits discarded");
                if (AnyNpcDirty()) {
                    DiscardNpcEdits();
                    KickNpcReapply();
                }
                g_confirmClose = false;
                ImGui::CloseCurrentPopup();
                stayOpen = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel##cckeep")) {
                g_confirmClose = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::End();
        if (!stayOpen) {
            g_open = false;
            Display::g_dirty = true;
            logger::info("panel closed from the panel");
        }
    }

    //============= Favorites Slot Map =============
    void DrawSlotMap() {
        static constexpr int CROSS[7][7] = {
            { -1, -1, -1, 6, -1, -1, -1 },
            { -1, -1, -1, 7, -1, -1, -1 },
            { -1, -1, -1, 8, -1, -1, -1 },
            { 0, 1, 2, -1, 3, 4, 5 },
            { -1, -1, -1, 9, -1, -1, -1 },
            { -1, -1, -1, 10, -1, -1, -1 },
            { -1, -1, -1, 11, -1, -1, -1 },
        };
        static constexpr const char* SHORT_NAMES[12] = {
            "L LBACK", "L BACK", "L HIP", "R HIP", "R BACK", "R LBACK",
            "R CHEST", "L CHEST", "BELT BK", "BELT FR", "R ANKLE", "L ANKLE"
        };
        const float fs = ImGui::GetFontSize();
        const float cellW = fs * 5.6f;
        const float cellH = fs * 2.6f;
        const float gap = 4.0f;
        const float gridW = 7.0f * cellW + 6.0f * gap;
        const float gridH = 7.0f * cellH + 6.0f * gap;
        const float availW = ImGui::GetContentRegionAvail().x;
        const float pad = std::max(0.0f, (availW - gridW) * 0.5f);
        const float left = ImGui::GetCursorPosX() + pad;
        const float top = ImGui::GetCursorPosY();
        const ImVec2 scr = ImGui::GetCursorScreenPos();
        const float gcx = scr.x + pad + gridW * 0.5f;
        const float gty = scr.y;
        auto* dl = ImGui::GetWindowDrawList();
        const float scrLeft = scr.x + pad;
        auto* bodySRV = (Display::g_inPA && g_bodyPASRV) ? g_bodyPASRV : g_bodySRV;
        const int bodyTexW = (Display::g_inPA && g_bodyPASRV) ? g_bodyPAW : g_bodyW;
        const int bodyTexH = (Display::g_inPA && g_bodyPASRV) ? g_bodyPAH : g_bodyH;
        if (bodySRV) {
            const float texAspect = static_cast<float>(bodyTexW) / static_cast<float>(bodyTexH);
            float th = gridH;
            float tw = th * texAspect;
            if (tw > gridW) {
                tw = gridW;
                th = tw / texAspect;
            }
            const float tx = scrLeft + (gridW - tw) * 0.5f;
            const float ty = gty + (gridH - th) * 0.5f;
            dl->AddImage((ImTextureID)bodySRV, ImVec2(tx, ty), ImVec2(tx + tw, ty + th),
                ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32(255, 255, 255, 90));
        } else {
            const ImU32 ghost = IM_COL32(110, 210, 150, 46);
            dl->AddCircle(ImVec2(gcx, gty + gridH * 0.055f), gridH * 0.045f, ghost, 24, 5.0f);
            dl->AddLine(ImVec2(gcx, gty + gridH * 0.10f), ImVec2(gcx, gty + gridH * 0.56f), ghost, 9.0f);
            dl->AddLine(ImVec2(gcx - gridW * 0.30f, gty + gridH * 0.22f),
                ImVec2(gcx + gridW * 0.30f, gty + gridH * 0.22f), ghost, 8.0f);
            dl->AddLine(ImVec2(gcx, gty + gridH * 0.56f), ImVec2(gcx - gridW * 0.09f, gty + gridH * 0.98f), ghost, 9.0f);
            dl->AddLine(ImVec2(gcx, gty + gridH * 0.56f), ImVec2(gcx + gridW * 0.09f, gty + gridH * 0.98f), ghost, 9.0f);
        }

        auto* idm = RE::BSInputDeviceManager::GetSingleton();
        const bool gamepadMode = idm && idm->IsGamepadConnected();
        static constexpr const char* KEY_NAMES[12] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=" };
        const auto drawSlotButton = [&](int s, float x, float y) {
            ImGui::SetCursorPos(ImVec2(x, y));
            const bool has = Display::g_state[s].formID != 0;
            const bool sel = s == g_slot;
            const bool dim = !has || !Slots::g_slotEnabled[s];
            if (dim) {
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.40f);
            }
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, fs * 0.32f);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.40f, 0.56f, 0.76f, 0.60f));
            if (sel) {
                ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(66, 130, 220, 255));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(80, 150, 240, 255));
                ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 255, 255, 230));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.5f);
            }
            char bid[24];
            std::snprintf(bid, sizeof(bid), "##slot%d", s);
            ImGui::SetNextItemAllowOverlap();
            if (ImGui::Button(bid, ImVec2(cellW, cellH))) {
                g_slot = s;
            }
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                ImGui::SetDragDropPayload("VF_FAVPOS", &s, sizeof(int));
                const int wsrc = Slots::g_slotFav[s];
                ImGui::Text("Fav %s (%s) - drop on a slot to swap",
                    gamepadMode ? std::to_string(wsrc).c_str() : KEY_NAMES[wsrc], SHORT_NAMES[s]);
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("VF_FAVPOS")) {
                    if (const int from = *static_cast<const int*>(payload->Data); from != s) {
                        const int w = Slots::g_slotFav[from];
                        Slots::g_slotFav[from] = Slots::g_slotFav[s];
                        Slots::g_slotFav[s] = w;
                        SaveDeferred();
                        Display::g_dirty = true;
                        KickReconcile();
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s%s%s", Slots::g_slots[s].label.c_str(),
                    Slots::g_slotEnabled[s] ? "" : " (disabled)",
                    g_slotDirty[s] ? " (* = unsaved changes)" : "");
            }
            const auto rmin = ImGui::GetItemRectMin();
            auto* fnt = ImGui::GetFont();
            const float tsz = fs * 0.92f;
            const int wheel = Slots::g_slotFav[s];
            char l1[24];
            std::snprintf(l1, sizeof(l1), "%s%s", SHORT_NAMES[s], g_slotDirty[s] ? "*" : "");
            const ImU32 tcol = IM_COL32(255, 255, 255, dim ? 100 : 235);
            const float w1 = fnt->CalcTextSizeA(tsz, std::numeric_limits<float>::max(), 0.0f, l1).x;
            dl->AddText(fnt, tsz, ImVec2(rmin.x + (cellW - w1) * 0.5f, rmin.y + cellH - tsz - fs * 0.18f), tcol, l1);
            if (dim) {
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.0f);
            }
            const float cw = fs * 2.2f;
            ImGui::SetCursorScreenPos(ImVec2(rmin.x + (cellW - cw) * 0.5f, rmin.y + fs * 0.12f));
            ImGui::SetNextItemWidth(cw);
            char fid[16];
            std::snprintf(fid, sizeof(fid), "##fav%d", s);
            char wl[8];
            std::snprintf(wl, sizeof(wl), "%s", gamepadMode ? std::to_string(wheel).c_str() : KEY_NAMES[wheel]);
            if (ImGui::BeginCombo(fid, "", ImGuiComboFlags_NoArrowButton)) {
                for (int w = 0; w < Slots::FAV_SLOTS; ++w) {
                    if (w == wheel) {
                        continue;
                    }
                    const int other = Slots::SlotOfWheel(w);
                    char il[32];
                    std::snprintf(il, sizeof(il), "%s##sw%d", SHORT_NAMES[other], w);
                    if (ImGui::Selectable(il)) {
                        Slots::g_slotFav[other] = wheel;
                        Slots::g_slotFav[s] = w;
                        SaveDeferred();
                        Display::g_dirty = true;
                        KickReconcile();
                    }
                }
                ImGui::EndCombo();
            }
            {
                const auto bmin = ImGui::GetItemRectMin();
                const auto bmax = ImGui::GetItemRectMax();
                const float ww = fnt->CalcTextSizeA(fs, std::numeric_limits<float>::max(), 0.0f, wl).x;
                dl->AddText(fnt, fs, ImVec2((bmin.x + bmax.x - ww) * 0.5f, (bmin.y + bmax.y - fs) * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_Text), wl);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("drag and drop, or pick a favorites position to swap slots");
            }
            if (dim) {
                ImGui::PopStyleVar();
            }
            if (sel) {
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(3);
            }
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
            if (dim) {
                ImGui::PopStyleVar();
            }
        };
        for (int r = 0; r < 7; ++r) {
            for (int c = 0; c < 7; ++c) {
                const int w = CROSS[r][c];
                if (w >= 0) {
                    drawSlotButton(Slots::SlotOfWheel(w), left + c * (cellW + gap), top + r * (cellH + gap));
                }
            }
        }
        ImGui::SetCursorPosY(top + gridH + fs * 0.8f);
        if (ImGui::SmallButton("Reset Layout##favreset")) {
            for (int i = 0; i < Slots::FAV_SLOTS; ++i) {
                Slots::g_slotFav[i] = i;
            }
            SaveDeferred();
            Display::g_dirty = true;
            KickReconcile();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("put every fav position back on its default body slot");
        }
    }

    //============= Layer Editor (shared by the player and NPC pages) =============
    struct LayerEditor
    {
        Slots::Transform* work;
        Slots::Transform* orig;
        bool* dirty;
        bool* captured;
        Layer* slotLayer;
        Layer& sel;
        int count;
        bool npc;
    };

    void NPreview(int slot);
    void KickNpcReapply();
    LayerEnv NpcEnvFor(int slot);
    void DrawHiddenParts(std::uint32_t npcID, int slot, std::uint32_t form, std::uint64_t omod);

    static void Preview(const LayerEditor& s, int slot) {
        if (s.npc) {
            NPreview(slot);
        } else {
            ApplyWorkToNode(slot, s.slotLayer[slot]);
        }
    }

    static void Visibility(const LayerEditor& s) {
        if (s.npc) {
            KickNpcReapply();
        } else {
            Display::Schedule([]() { Display::UpdateVisibilityAll(); }, 0);
            Display::Schedule([]() { Npc::ReapplyTransforms(); }, 0);
        }
    }

    void LabeledValue(const char* label, const char* value, bool continueLine) {
        ImGui::Text("%s = [", label);
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextColored(ImVec4(0.43f, 0.88f, 0.43f, 1.0f), "%s", value);
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextUnformatted(continueLine ? "]  |  " : "]");
        if (continueLine) {
            ImGui::SameLine(0.0f, 0.0f);
        } else {
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
        }
    }

    void SaveAllDirty(LayerEditor& s) {
        for (int i = 0; i < s.count; ++i) {
            if (s.dirty[i] && s.captured[i]) {
                const LayerEnv e = s.npc ? NpcEnvFor(i) : PlayerEnv(i);
                if (LayerApplicable(e, s.slotLayer[i], i) && LayerExists(e, s.slotLayer[i], i)) {
                    WriteLayer(e, s.slotLayer[i], i, s.work[i]);
                    s.orig[i] = s.work[i];
                    s.dirty[i] = false;
                } else {
                    logger::warn("slot {} edit on layer {} ({}) not saved - the layer no longer applies",
                        i, LayerNum(s.slotLayer[i]), LayerName(s.slotLayer[i]));
                }
            }
        }
    }

    void DrawLayerEditor(const LayerEnv& e, int slot, LayerEditor& s) {
        const float sfs = ImGui::GetFontSize();
        auto& work = s.work[slot];
        auto& orig = s.orig[slot];
        const auto edited = [&]() {
            s.dirty[slot] = true;
            s.slotLayer[slot] = s.sel;
            Preview(s, slot);
        };
        const Layer winner = Winner(e, slot);
        if (ImGui::BeginTable("stack", 4, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("num", ImGuiTableColumnFlags_WidthFixed, sfs * 3.0f);
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, sfs * 16.5f);
            ImGui::TableSetupColumn("desc", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthFixed, sfs * 6.5f);
            for (Layer l : LAYER_ORDER) {
                const auto* entry = LayerGet(e, l, slot);
                if (!entry) {
                    continue;
                }
                const bool applicable = LayerApplicable(e, l, slot);
                const bool isWinner = applicable && l == winner;
                const bool isSel = l == s.sel;
                ImGui::TableNextRow();
                if (isSel) {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(40, 72, 122, 130));
                }
                ImGui::TableNextColumn();
                if (!applicable) {
                    ImGui::BeginDisabled();
                }
                char rid[24];
                std::snprintf(rid, sizeof(rid), "%s##Lr%d", LayerNum(l), static_cast<int>(l));
                if (ImGui::RadioButton(rid, isSel)) {
                    s.sel = l;
                    work = *entry;
                    edited();
                }
                if (!applicable) {
                    ImGui::EndDisabled();
                }
                ImGui::TableNextColumn();
                if (isWinner) {
                    ImGui::TextColored(ImVec4(0.43f, 0.88f, 0.43f, 1.0f), "%s", LayerName(l));
                } else if (applicable) {
                    ImGui::Text("%s", LayerName(l));
                } else {
                    ImGui::TextDisabled("%s", LayerName(l));
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("stored position:\npos (%.1f, %.1f, %.1f)\nrot (%.1f, %.1f, %.1f)\nscale %.2f%s",
                        entry->px, entry->py, entry->pz,
                        entry->rx * RAD2DEG, entry->ry * RAD2DEG, entry->rz * RAD2DEG, entry->scale,
                        applicable ? "" :
                        e.npc    ? "\n\n(inactive - applies only in its context,\ne.g. the preview NPC wearing that pack/armor)" :
                                   "\n\n(inactive - applies only in its context,\ne.g. wearing that pack/armor or being in Power Armor)");
                }
                ImGui::TableNextColumn();
                const auto desc = LayerDesc(e, l, slot);
                ImGui::TextDisabled("%s", desc.c_str());
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip("%s", desc.c_str());
                }
                ImGui::TableNextColumn();
                if (entry->hidden) {
                    ImGui::TextColored(ImVec4(0.95f, 0.34f, 0.30f, 1.0f), "[Hidden]");
                }
            }
            ImGui::EndTable();
        }
        std::vector<Layer> addable;
        for (Layer l : LAYER_ORDER) {
            if (LayerApplicable(e, l, slot) && !LayerExists(e, l, slot)) {
                addable.push_back(l);
            }
        }
        if (!addable.empty()) {
            ImGui::Dummy(ImVec2(0.0f, 3.0f));
            ImGui::SetNextItemWidth(sfs * 24.0f);
            const bool comboOpen = ImGui::BeginCombo("##addover", "Add override layer...");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(e.npc ? "add a new NPC slot layer" : "add a new slot layer");
            }
            if (comboOpen) {
                for (Layer l : addable) {
                    char cid[64];
                    std::snprintf(cid, sizeof(cid), "%s - %s##add%d", LayerNum(l), LayerName(l), static_cast<int>(l));
                    if (ImGui::Selectable(cid)) {
                        Slots::Transform seed;
                        if (auto* t = LayerActive(e, slot)) {
                            seed = *t;
                        }
                        seed.hidden = false;
                        s.sel = l;
                        s.slotLayer[slot] = l;
                        work = seed;
                        orig = seed;
                        s.captured[slot] = true;
                        s.dirty[slot] = false;
                        WriteLayer(e, l, slot, seed);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", LayerDesc(e, l, slot).c_str());
                    }
                }
                ImGui::EndCombo();
            }
        }

        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::SeparatorText("Positioner");
        LabeledValue("Slot Layer", LayerNum(s.sel), true);
        LabeledValue("Layer Name", LayerName(s.sel), false);
        if (s.sel != winner) {
            ImGui::TextColored(ImVec4(0.95f, 0.77f, 0.20f, 1.0f),
                "Editing inactive layer - %s (%s) currently overrides it", LayerNum(winner), LayerName(winner));
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("the highest-numbered layer that applies wins - to change what you see\nright now, edit the green layer instead");
            }
        }
        bool changed = false;
        float pos[3]{ work.px, work.py, work.pz };
        if (ImGui::DragFloat3("Position", pos, 0.05f)) {
            work.px = pos[0];
            work.py = pos[1];
            work.pz = pos[2];
            changed = true;
        }
        float rot[3]{ work.rx * RAD2DEG, work.ry * RAD2DEG, work.rz * RAD2DEG };
        if (ImGui::DragFloat3("Rotation", rot, 0.25f)) {
            work.rx = rot[0] / RAD2DEG;
            work.ry = rot[1] / RAD2DEG;
            work.rz = rot[2] / RAD2DEG;
            changed = true;
        }
        float sc = work.scale;
        if (ImGui::DragFloat("Scale", &sc, 0.005f, 0.05f, 10.0f)) {
            work.scale = sc;
            changed = true;
        }
        if (changed) {
            edited();
        }

        bool anyDirty = false;
        for (int i = 0; i < s.count; ++i) {
            anyDirty = anyDirty || s.dirty[i];
        }
        if (ImGui::Button("Save Layer")) {
            WriteLayer(e, s.sel, slot, work);
            orig = work;
            s.dirty[slot] = false;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("save settings for the current layer");
        }
        ImGui::SameLine();
        if (!anyDirty) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Save All")) {
            SaveAllDirty(s);
        }
        if (!anyDirty) {
            ImGui::EndDisabled();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("save settings for all layers (every edited slot *)");
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset Layer")) {
            if (auto* t = LayerGet(e, s.sel, slot)) {
                work = *t;
            } else {
                work = orig;
            }
            s.dirty[slot] = false;
            s.slotLayer[slot] = s.sel;
            Preview(s, slot);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("reset the layer to its previously saved settings");
        }
        ImGui::SameLine();
        if (!anyDirty) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Reset All")) {
            for (int i = 0; i < s.count; ++i) {
                if (s.captured[i] && s.dirty[i]) {
                    s.work[i] = s.orig[i];
                    s.dirty[i] = false;
                    Preview(s, i);
                }
            }
        }
        if (!anyDirty) {
            ImGui::EndDisabled();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("reset all layers to their previously saved settings");
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear Layer")) {
            const bool keepHidden = work.hidden;
            work = Slots::Transform{};
            work.hidden = keepHidden;
            edited();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("zero all position values for this layer");
        }

        auto* selected = LayerGet(e, s.sel, slot);
        const bool selHidden = selected && selected->hidden;
        if (!selected) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button(selHidden ? "Show Layer" : "Hide Layer")) {
            selected->hidden = !selHidden;
            work.hidden = selected->hidden;
            orig.hidden = selected->hidden;
            SaveDeferred();
            Visibility(s);
        }
        if (!selected) {
            ImGui::EndDisabled();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(selHidden ?
                                  "show the object when the current layer is active" :
                                  "hide the object when the current layer is active");
        }
        bool hasSavedLayers = false;
        bool allLayersHidden = true;
        for (Layer l : LAYER_ORDER) {
            if (const auto* entry = LayerGet(e, l, slot)) {
                hasSavedLayers = true;
                allLayersHidden = allLayersHidden && entry->hidden;
            }
        }
        ImGui::SameLine();
        if (!hasSavedLayers) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button(allLayersHidden ? "Show All Layers" : "Hide All Layers")) {
            for (Layer l : LAYER_ORDER) {
                if (auto* entry = LayerGet(e, l, slot)) {
                    entry->hidden = !allLayersHidden;
                }
            }
            if (auto* entry = LayerGet(e, s.sel, slot)) {
                work.hidden = entry->hidden;
                orig.hidden = entry->hidden;
            }
            SaveDeferred();
            Visibility(s);
        }
        if (!hasSavedLayers) {
            ImGui::EndDisabled();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(allLayersHidden ?
                                  "show the object across all layers" :
                                  "hide the object across all layers");
        }
        ImGui::SameLine();
        const bool canDelete = s.sel != Layer::Default && s.sel != Layer::PABase &&
                               s.sel != Layer::PackAny && LayerExists(e, s.sel, slot);
        if (!canDelete) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Delete Layer")) {
            EraseLayer(e, s.sel, slot);
            s.sel = s.dirty[slot] ? s.slotLayer[slot] : Winner(e, slot);
            if (auto* t = LayerActive(e, slot)) {
                work = *t;
            }
            orig = work;
            s.dirty[slot] = false;
        }
        if (!canDelete) {
            ImGui::EndDisabled();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(canDelete ?
                                  "delete the current layer" :
                                  "base, Power Armor Default, and Backpack Default cannot be deleted");
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy")) {
            g_clip = work;
            g_hasClip = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("copy all settings from this layer");
        }
        ImGui::SameLine();
        if (!g_hasClip) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Paste")) {
            work = g_clip;
            edited();
        }
        if (!g_hasClip) {
            ImGui::EndDisabled();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("paste the copied settings");
        }
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
    }

    LayerEditor PlayerEditor() {
        return { g_workAll, g_origAll, g_slotDirty, g_slotCaptured, g_slotLayer, g_layerSel, Slots::MAX_INDEX, false };
    }

    void SaveAllPlayerDirty() {
        LayerEditor editor = PlayerEditor();
        SaveAllDirty(editor);
    }

    //============= NPC Slots Page =============
    int g_npcSel = 4;
    Layer g_npcLayerSel = Layer::Default;
    std::uint32_t g_npcTarget = 0;
    std::uint32_t g_npcSeenGen = 0;
    struct NpcEnvKey
    {
        std::uint32_t id{ 0 };
        std::uint32_t cfgKey{ 0 };
        int cfgIdx{ -1 };
        Npc::WearCtx wear;
        bool operator==(const NpcEnvKey&) const = default;
    };
    NpcEnvKey g_npcEnvKey;
    int g_npcLastMask[Slots::FAV_SLOTS]{};
    Slots::Transform g_npcWorkAll[Slots::FAV_SLOTS]{};
    Slots::Transform g_npcOrigAll[Slots::FAV_SLOTS]{};
    bool g_npcDirty[Slots::FAV_SLOTS]{};
    bool g_npcCapt[Slots::FAV_SLOTS]{};
    Layer g_npcSlotLayer[Slots::FAV_SLOTS]{};

    struct PickNpc
    {
        std::uint32_t formID{ 0 };
        std::string name;
    };
    bool g_npcAll = false;
    std::uint32_t g_npcPickKey = 0;
    std::vector<PickNpc> g_pickNpcs;
    bool g_pickNpcsBuilt = false;
    bool g_pickNpcsBusy = false;
    char g_npcFilter[64] = {};

    void BuildNpcList() {
        g_pickNpcs.clear();
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            return;
        }
        for (auto* npc : dh->GetFormArray<RE::TESNPC>()) {
            if (!npc || !npc->IsUnique() || npc->IsPreset() || npc->GetFormID() == 0x7) {
                continue;
            }
            const auto nm = RE::TESFullName::GetFullName(*npc);
            if (!nm.empty()) {
                g_pickNpcs.push_back({ npc->GetFormID(), Slots::IniSafe(std::string(nm)) });
            }
        }
        std::sort(g_pickNpcs.begin(), g_pickNpcs.end(), [](const PickNpc& a, const PickNpc& b) {
            return a.name != b.name ? a.name < b.name : a.formID < b.formID;
        });
        logger::info("all-NPCs picker built: {} unique actors", g_pickNpcs.size());
    }

    struct NpcSel
    {
        std::uint32_t cfgKey{ 0 };
        std::uint32_t id{ 0 };
        Npc::WearCtx wear;
    };

    NpcSel SelectedNpc() {
        if (g_npcAll) {
            return { g_npcPickKey, 0, {} };
        }
        for (const auto& t : Npc::CachedTargets()) {
            if (t.id == g_npcTarget) {
                return { t.cfgKey, t.id, t.wear };
            }
        }
        return {};
    }

    LayerEnv NpcEnvFor(int slot) {
        const NpcSel sel = SelectedNpc();
        LayerEnv e{ Display::NpcTablesFor(Slots::NpcSetFor(sel.cfgKey)) };
        e.npc = true;
        e.wear = sel.wear;
        e.item = sel.id ? Npc::SlotItemOf(sel.id, slot) : 0;
        return e;
    }

    void KickNpcReapply() {
        Display::Schedule([]() { Npc::ReapplyTransforms(); }, 0);
    }

    std::string NpcFamiliesLabel(int mask) {
        std::string s;
        for (int fam = 0; fam < Npc::FamilyCount(); ++fam) {
            if (mask & (1 << fam)) {
                s += s.empty() ? "" : "+";
                s += Npc::FamilyLabel(fam);
            }
        }
        return s.empty() ? std::string("Disabled") : s;
    }

    void NPreview(int slot) {
        const LayerEnv e = NpcEnvFor(slot);
        if (auto* cell = LayerGet(e, g_npcSlotLayer[slot], slot)) {
            Npc::PreviewTransforms(cell, g_npcWorkAll[slot]);
        }
    }

    bool AnyNpcDirty() {
        for (int i = 0; i < Slots::FAV_SLOTS; ++i) {
            if (g_npcDirty[i]) {
                return true;
            }
        }
        return false;
    }

    void DiscardNpcEdits() {
        for (int i = 0; i < Slots::FAV_SLOTS; ++i) {
            g_npcWorkAll[i] = g_npcOrigAll[i];
            g_npcDirty[i] = false;
        }
    }

    LayerEditor NpcEditor() {
        return { g_npcWorkAll, g_npcOrigAll, g_npcDirty, g_npcCapt, g_npcSlotLayer, g_npcLayerSel,
            Slots::FAV_SLOTS, true };
    }

    void SaveAllNpcDirty() {
        LayerEditor editor = NpcEditor();
        SaveAllDirty(editor);
    }

    void DrawNpcPage() {
        const float sfs = ImGui::GetFontSize();
        ImGui::SeparatorText("NPC Slots");
        if (g_npcSeenGen != Npc::TargetGen()) {
            g_npcSeenGen = Npc::TargetGen();
            if (const auto sug = Npc::SuggestedTarget()) {
                g_npcTarget = sug;
            }
        }
        const auto& targets = Npc::CachedTargets();
        const Npc::TargetInfo* tgt = nullptr;
        for (const auto& t : targets) {
            if (t.id == g_npcTarget) {
                tgt = &t;
                break;
            }
        }
        if (!tgt && !targets.empty()) {
            g_npcTarget = targets.front().id;
            tgt = &targets.front();
        }
        const NpcSel sel = SelectedNpc();
        const int cfgIdx = Slots::NpcConfigIndexFor(sel.cfgKey);
        Slots::NpcConfigGroup* cfg = cfgIdx >= 0 ? &Slots::g_npcConfigs[cfgIdx] : nullptr;
        auto& editSet = cfg ? cfg->tables : Slots::g_npcSet;
        const auto refreshNpcs = []() { Display::Schedule([]() { Npc::RefreshAll(); }, 0); };
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        if (ImGui::BeginTable("npcslots", 3, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("sel", ImGuiTableColumnFlags_WidthFixed, sfs * 7.0f);
            ImGui::TableSetupColumn("fam", ImGuiTableColumnFlags_WidthFixed, sfs * 8.5f);
            ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthStretch);
            int rowOrder[Slots::FAV_SLOTS];
            for (int i = 0; i < Slots::FAV_SLOTS; ++i) {
                rowOrder[i] = i;
            }
            std::sort(std::begin(rowOrder), std::end(rowOrder), [&editSet](int a, int b) {
                const int pa = editSet.slots[a].prio;
                const int pb = editSet.slots[b].prio;
                return pa != pb ? pa < pb : a < b;
            });
            for (int ri = 0; ri < Slots::FAV_SLOTS; ++ri) {
                const int i = rowOrder[ri];
                auto& def = editSet.slots[i];
                ImGui::TableNextRow();
                if (g_npcSel == i) {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(40, 72, 122, 130));
                }
                ImGui::TableNextColumn();
                char rid[48];
                std::snprintf(rid, sizeof(rid), "%s##npcsel%d", Slots::g_slots[i].label.c_str(), i);
                if (ImGui::RadioButton(rid, g_npcSel == i)) {
                    g_npcSel = i;
                }
                if (ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("VF_NPCPRIO", &i, sizeof(int));
                    ImGui::TextUnformatted(Slots::g_slots[i].label.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const auto* payload = ImGui::AcceptDragDropPayload("VF_NPCPRIO")) {
                        const int src = *static_cast<const int*>(payload->Data);
                        if (src != i) {
                            Slots::MoveNpcSlot(editSet, src, i);
                            SaveDeferred();
                            Display::Schedule([]() { Npc::RefreshAll(); }, 0);
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("drag rows to reorder - highest priority wins\nequipped weapon always takes the top priority");
                }
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(sfs * 8.0f);
                char cid[24];
                std::snprintf(cid, sizeof(cid), "##npcfam%d", i);
                if (def.families) {
                    g_npcLastMask[i] = def.families;
                }
                const auto famPreview = NpcFamiliesLabel(def.families);
                if (ImGui::BeginCombo(cid, famPreview.c_str())) {
                    for (int fam = 0; fam < Npc::FamilyCount(); ++fam) {
                        bool on = (def.families & (1 << fam)) != 0;
                        char fid[40];
                        std::snprintf(fid, sizeof(fid), "%s##fchk%d_%d", Npc::FamilyLabel(fam), i, fam);
                        if (ImGui::Checkbox(fid, &on)) {
                            if (on && !def.families && g_npcLastMask[i] && !(g_npcLastMask[i] & (1 << fam))) {
                                Slots::ResetNpcSlotData(editSet, i);
                                g_npcCapt[i] = false;
                                g_npcDirty[i] = false;
                            }
                            def.families = on ? def.families | (1 << fam) : def.families & ~(1 << fam);
                            SaveDeferred();
                            Display::Schedule([]() { Npc::RefreshAll(); }, 0);
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::TableNextColumn();
                if (def.families) {
                    if (const auto itemID = sel.id ? Npc::SlotItemOf(sel.id, i) : 0u) {
                        const auto nm = Slots::FriendlyName(itemID);
                        ImGui::TextDisabled("%.40s", nm.c_str());
                    } else {
                        ImGui::TextDisabled("-");
                    }
                } else {
                    ImGui::TextDisabled("-");
                }
            }
            ImGui::EndTable();
        }
        if (ImGui::Button("Reset All Slots")) {
            Slots::ResetAllNpcSlots(editSet);
            std::fill(std::begin(g_npcCapt), std::end(g_npcCapt), false);
            std::fill(std::begin(g_npcDirty), std::end(g_npcDirty), false);
            SaveDeferred();
            Display::Schedule([]() { Npc::RefreshAll(); }, 0);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("reset every NPC slot to defaults - types, priority order, positions\nand all their layers");
        }
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::SeparatorText("Display");
        int& modeRef = cfg ? cfg->mode : Slots::g_npcDisplayMode;
        const int first = cfg ? -1 : 0;
        const int cur = std::clamp(modeRef, first, 2);
        ImGui::TextUnformatted("Show on NPCs");
        ImGui::SetNextItemWidth(sfs * 9.5f);
        if (ImGui::BeginCombo("##npcmode", DISPLAY_MODES[cur + 1])) {
            for (int m = first; m <= 2; ++m) {
                if (ImGui::Selectable(DISPLAY_MODES[m + 1], cur == m)) {
                    modeRef = m;
                    SaveDeferred();
                    refreshNpcs();
                }
            }
            ImGui::EndCombo();
        }

        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::SeparatorText("NPC Group Override");
        if (ImGui::RadioButton("Nearby", !g_npcAll)) {
            g_npcAll = false;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("All NPCs", g_npcAll)) {
            g_npcAll = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("every named unique NPC in your load order - add the ones who aren't standing next to you\nno live preview for these, positions still edit their group");
        }
        char tgtPreview[96];
        ImGui::SetNextItemWidth(sfs * 18.0f);
        if (g_npcAll) {
            if (!g_pickNpcsBuilt && !g_pickNpcsBusy) {
                g_pickNpcsBusy = true;
                Display::Schedule([]() {
                    std::lock_guard lock(Display::g_tablesMutex);
                    BuildNpcList();
                    g_pickNpcsBuilt = true;
                    g_pickNpcsBusy = false;
                }, 0);
            }
            const auto pickName = Slots::FriendlyName(g_npcPickKey);
            if (g_npcPickKey) {
                std::snprintf(tgtPreview, sizeof(tgtPreview), "%.48s [%s]", pickName.c_str(), Slots::SpecKeyFor(g_npcPickKey).c_str());
            } else {
                std::snprintf(tgtPreview, sizeof(tgtPreview), g_pickNpcsBuilt ? "pick an NPC..." : "building the NPC list...");
            }
            if (ImGui::BeginCombo("##npcpick", tgtPreview)) {
                ImGui::SetNextItemWidth(sfs * 14.0f);
                ImGui::InputTextWithHint("##npcfilter", "filter...", g_npcFilter, sizeof(g_npcFilter));
                static std::vector<int> vis;
                static char visFilter[sizeof(g_npcFilter)] = {};
                static std::size_t visCount = 0;
                if (visCount != g_pickNpcs.size() || std::strcmp(visFilter, g_npcFilter) != 0) {
                    visCount = g_pickNpcs.size();
                    std::snprintf(visFilter, sizeof(visFilter), "%s", g_npcFilter);
                    vis.clear();
                    for (int n = 0; n < static_cast<int>(g_pickNpcs.size()); ++n) {
                        if (!g_npcFilter[0] || ContainsNoCase(g_pickNpcs[n].name, g_npcFilter)) {
                            vis.push_back(n);
                        }
                    }
                }
                ImGuiListClipper clip;
                clip.Begin(static_cast<int>(vis.size()));
                while (clip.Step()) {
                    for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
                        const auto& n = g_pickNpcs[vis[row]];
                        char nid[128];
                        std::snprintf(nid, sizeof(nid), "%.48s [%s]##np%u", n.name.c_str(), Slots::SpecKeyFor(n.formID).c_str(), n.formID);
                        if (ImGui::Selectable(nid, n.formID == g_npcPickKey)) {
                            g_npcPickKey = n.formID;
                        }
                    }
                }
                ImGui::EndCombo();
            }
        } else {
            if (tgt) {
                std::snprintf(tgtPreview, sizeof(tgtPreview), "%.48s [%08X]", tgt->name.c_str(), tgt->id);
            } else {
                std::snprintf(tgtPreview, sizeof(tgtPreview), "no NPCs tracked nearby");
            }
            if (ImGui::BeginCombo("##npctarget", tgtPreview)) {
                for (const auto& t : targets) {
                    char tid[96];
                    std::snprintf(tid, sizeof(tid), "%.48s [%08X]##tgt%u", t.name.c_str(), t.id, t.id);
                    if (ImGui::Selectable(tid, t.id == g_npcTarget)) {
                        g_npcTarget = t.id;
                    }
                }
                ImGui::EndCombo();
            }
        }
        if (sel.cfgKey) {
            const ImVec2 wide(sfs * 18.0f, 0.0f);
            const bool unsaved = AnyNpcDirty();
            const bool full = static_cast<int>(Slots::g_npcConfigs.size()) >= Slots::MAX_NPC_CONFIGS;
            if (unsaved || full) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Create New Override Group", wide)) {
                Slots::NewNpcConfig(sel.cfgKey);
                SaveDeferred();
                refreshNpcs();
                return;
            }
            if (unsaved || full) {
                ImGui::EndDisabled();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(unsaved ? "save or reset your layer edits (*) first" :
                                  full    ? "group limit reached" :
                                            "create an override group - copies all of the baseline NPC settings and lets you\noverride them for any NPC in this group, e.g. a custom setup for companions,\nsettlers etc. add any nearby NPC to the group");
            }
            const bool none = Slots::g_npcConfigs.empty();
            if (none || unsaved) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Add to Override Group", wide)) {
                ImGui::OpenPopup("##addtogroup");
            }
            if (none || unsaved) {
                ImGui::EndDisabled();
            }
            if (unsaved && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("save or reset your layer edits (*) first");
            }
            if (ImGui::BeginPopup("##addtogroup")) {
                for (int gi = 0; gi < static_cast<int>(Slots::g_npcConfigs.size()); ++gi) {
                    char cid[96];
                    std::snprintf(cid, sizeof(cid), "%.48s##acfg%d",
                        Slots::g_npcConfigs[gi].label.c_str(), gi);
                    if (ImGui::Selectable(cid)) {
                        Slots::AddNpcToConfig(gi, sel.cfgKey);
                        SaveDeferred();
                        refreshNpcs();
                        ImGui::EndPopup();
                        return;
                    }
                }
                ImGui::EndPopup();
            }
            if (cfg) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Override Group");
                ImGui::SameLine();
                char lbuf[64];
                std::snprintf(lbuf, sizeof(lbuf), "%.60s", cfg->label.c_str());
                ImGui::SetNextItemWidth(sfs * 9.0f);
                if (ImGui::InputText("##cfgname", lbuf, sizeof(lbuf))) {
                    cfg->label = lbuf;
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    SaveDeferred();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("click to rename");
                }
                ImGui::SameLine();
                if (unsaved) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Button("Remove from Group")) {
                    Slots::RemoveNpcFromConfigs(sel.cfgKey);
                    SaveDeferred();
                    refreshNpcs();
                    return;
                }
                if (unsaved) {
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        ImGui::SetTooltip("save or reset your layer edits (*) first");
                    }
                }
            }
        }

        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        const LayerEnv env = NpcEnvFor(g_npcSel);
        const NpcEnvKey ek{ sel.id, sel.cfgKey, cfgIdx, env.wear };
        if (ek != g_npcEnvKey) {
            g_npcEnvKey = ek;
            std::fill(std::begin(g_npcCapt), std::end(g_npcCapt), false);
            std::fill(std::begin(g_npcDirty), std::end(g_npcDirty), false);
            std::fill(std::begin(g_npcLastMask), std::end(g_npcLastMask), 0);
            KickNpcReapply();
        }
        const int ns = g_npcSel;
        static int lastNs = -1;
        if (!g_npcCapt[ns]) {
            g_npcCapt[ns] = true;
            if (auto* t = LayerActive(env, ns)) {
                g_npcWorkAll[ns] = *t;
            }
            g_npcOrigAll[ns] = g_npcWorkAll[ns];
            lastNs = -1;
        }
        if (lastNs != ns) {
            lastNs = ns;
            g_npcLayerSel = g_npcDirty[ns] ? g_npcSlotLayer[ns] : Winner(env, ns);
        }
        if (!LayerApplicable(env, g_npcLayerSel, ns) || !LayerExists(env, g_npcLayerSel, ns)) {
            g_npcLayerSel = Winner(env, ns);
        }

        ImGui::SeparatorText("Slot Info");
        const std::string npcItemValue = env.item ? Slots::FriendlyName(env.item) : std::string("Empty");
        LabeledValue("Item", npcItemValue.c_str(), false);
        const std::string npcSpecValue = env.item ? Slots::SpecKeyFor(env.item) : std::string("-");
        LabeledValue("Form ID", npcSpecValue.c_str(), false);
        LabeledValue("Slot Name", Slots::g_slots[ns].label.c_str(), false);
        LabeledValue("Bone", editSet.slots[ns].bone.c_str(), false);
        const auto npcFamStr = NpcFamiliesLabel(editSet.slots[ns].families);
        LabeledValue("Family", npcFamStr.c_str(), false);
        DrawHiddenParts(sel.id, ns, env.item, 0);
        ImGui::Dummy(ImVec2(0.0f, 6.0f));

        if (cfg) {
            ImGui::TextColored(ImVec4(0.43f, 0.88f, 0.43f, 1.0f), "Editing override group [%s]", cfg->label.c_str());
        }
        ImGui::SeparatorText("NPC Slot Position");
        LayerEditor editor = NpcEditor();
        DrawLayerEditor(env, ns, editor);
    }

    //============= Settings Page =============
    void DrawSettingsPage() {
        const float fs = ImGui::GetFontSize();
        const float labelCol = fs * 11.0f;
        ImGui::SeparatorText("Display");
        if (ImGui::Checkbox("Hide displays when no body armor is worn", &Slots::g_hideWhenNoBodyArmor)) {
            SaveDeferred();
            Display::g_dirty = true;
            KickReconcile();
        }
        if (ImGui::Checkbox("Display non-weapon favorites", &Slots::g_displayAnyItemType)) {
            SaveDeferred();
            Display::g_dirty = true;
            KickReconcile();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("show favorited aid, food and other non-weapon items on your body\n(untick for weapons only - custom slots are unaffected)");
        }
        if (ImGui::Checkbox("Show weapon effects", &Slots::g_showWeaponFX)) {
            SaveDeferred();
            Display::Schedule([]() { Display::ReattachAllDisplays(); }, 0);
            Display::Schedule([]() { Npc::RefreshAll(); }, 0);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("flames, glows and beams the game keeps hidden until the weapon is drawn\nthey do not animate on a holstered weapon");
        }
        if (ImGui::Checkbox("Hide while sleeping", &Npc::HideSleepingRef())) {
            SaveDeferred();
        }
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::SeparatorText("Hotkeys");
        BindKeyRow("Open/Close Panel", labelCol, -3, Slots::g_openKey, false, false, true,
            "opens and closes this panel - combos work, including modifier pairs\nlike Shift+Right Ctrl (press the keys, then release)");
        BindKeyRow("Hide All", labelCol, -5, Slots::g_hideAllKey, false, true, true,
            "toggles every display off/on in the world - combos work (default Shift+H)");
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::SeparatorText("NPC Override Groups");
        char cfgPreview[32];
        const int groupCount = static_cast<int>(Slots::g_npcConfigs.size());
        std::snprintf(cfgPreview, sizeof(cfgPreview), "%d group%s", groupCount, groupCount == 1 ? "" : "s");
        ImGui::SetNextItemWidth(fs * 14.0f);
        if (ImGui::BeginCombo("##npcgrouplist", cfgPreview)) {
            bool rosterChanged = false;
            for (int gi = 0; gi < static_cast<int>(Slots::g_npcConfigs.size()) && !rosterChanged; ++gi) {
                auto& cfg = Slots::g_npcConfigs[gi];
                ImGui::PushID(gi);
                char lbuf[64];
                std::snprintf(lbuf, sizeof(lbuf), "%.60s", cfg.label.c_str());
                ImGui::SetNextItemWidth(fs * 9.0f);
                if (ImGui::InputText("##cfglabel", lbuf, sizeof(lbuf))) {
                    cfg.label = lbuf;
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    SaveDeferred();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Delete")) {
                    Slots::DeleteNpcConfig(gi);
                    rosterChanged = true;
                    ImGui::PopID();
                    break;
                }
                for (int mi = 0; !rosterChanged && mi < static_cast<int>(cfg.memberSpecs.size()); ++mi) {
                    const auto& spec = cfg.memberSpecs[mi];
                    const auto memberID = Slots::SpecToForm(spec);
                    const auto memberName = memberID ? Slots::FriendlyName(memberID) : std::string{};
                    ImGui::Indent();
                    char mrid[24];
                    std::snprintf(mrid, sizeof(mrid), "Remove##m%d", mi);
                    if (ImGui::SmallButton(mrid)) {
                        cfg.memberSpecs.erase(cfg.memberSpecs.begin() + mi);
                        rosterChanged = true;
                        ImGui::Unindent();
                        break;
                    }
                    ImGui::SameLine();
                    if (memberID) {
                        ImGui::Text("%.48s [%08X]", memberName.c_str(), memberID);
                    } else {
                        ImGui::TextDisabled("%s (not loaded)", spec.c_str());
                    }
                    ImGui::Unindent();
                }
                if (cfg.memberSpecs.empty()) {
                    ImGui::Indent();
                    ImGui::TextDisabled("no members");
                    ImGui::Unindent();
                }
                ImGui::PopID();
            }
            if (Slots::g_npcConfigs.empty()) {
                ImGui::TextDisabled("none - pick an NPC on the NPC Slots tab and hit Create New Override Group");
            }
            if (rosterChanged) {
                Slots::InvalidateNpcConfigMembership();
                SaveDeferred();
                Display::Schedule([]() { Npc::RefreshAll(); }, 0);
            }
            ImGui::EndCombo();
        }

        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::SeparatorText("Item Blacklist");
        if (const auto eq = Display::g_eqWeapUiForm; eq) {
            const bool dbl = Slots::IsDisplayBlacklisted(eq);
            auto dnm = Slots::FriendlyName(eq);
            if (dnm.empty()) {
                dnm = Slots::SpecKeyFor(eq);
            }
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(fmt::format("Equipped: {}", dnm).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(dbl ? "Allow##eqbl" : "Blacklist##eqbl")) {
                Slots::SetDisplayBlacklisted(eq, !dbl);
                SaveDeferred();
                Display::g_dirty = true;
                Npc::MarkDirty();
                KickReconcile();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(dbl ? "let this weapon display again" :
                                        "blacklist the weapon you are holding right now");
            }
        } else {
            ImGui::TextDisabled("equip a weapon to quick-blacklist it");
        }
        if (ImGui::Button("Pick any item...##blpick") && !g_pickBusy.exchange(true)) {
            Display::Schedule([]() {
                std::lock_guard lock(Display::g_tablesMutex);
                BuildItemLists();
                g_pickBusy = false;
            }, 0);
            g_blPopupPending = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("browse every item in the game - modded included - and blacklist it\nwithout ever equipping or displaying it (use for broken models that crash)");
        }
        if (g_blPopupPending && !g_pickBusy.load()) {
            g_blPopupPending = false;
            ImGui::OpenPopup("Blacklist Item");
        }
        if (ImGui::BeginPopupModal("Blacklist Item", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::BeginChild("##bllist", ImVec2(fs * 28.0f, fs * 18.0f), true);
            const int p = DrawItemPickList(fs);
            ImGui::EndChild();
            if (p >= 0) {
                const auto& src = g_pickAll ? g_pickItemsAll : g_pickItems;
                if (src[static_cast<std::size_t>(p)].object) {
                    Slots::SetDisplayBlacklisted(src[static_cast<std::size_t>(p)].object->GetFormID(), true);
                    SaveDeferred();
                    Display::g_dirty = true;
                    Npc::MarkDirty();
                    KickReconcile();
                }
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::Button("Cancel##blcancel")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        for (std::size_t i = 0; i < Slots::g_displayBlacklistSpecs.size(); ++i) {
            const auto& spec = Slots::g_displayBlacklistSpecs[i];
            const auto id = Slots::SpecToForm(spec);
            auto nm = id ? Slots::FriendlyName(id) : std::string{};
            if (nm.empty()) {
                nm = spec;
            }
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(nm.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(fmt::format("Remove##dwl{}", i).c_str())) {
                if (id) {
                    Slots::SetDisplayBlacklisted(id, false);
                } else {
                    Slots::g_displayBlacklistSpecs.erase(Slots::g_displayBlacklistSpecs.begin() +
                                                         static_cast<std::ptrdiff_t>(i));
                }
                SaveDeferred();
                Display::g_dirty = true;
                Npc::MarkDirty();
                KickReconcile();
                break;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("remove from the blacklist - this item can display again");
            }
        }
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::SeparatorText("Backpack Blacklist");
        for (const auto wornId : Display::g_packClaimants) {
            const bool wbl = Slots::IsPackBlacklisted(wornId);
            auto wnm = Slots::FriendlyName(wornId);
            if (wnm.empty()) {
                wnm = Slots::SpecKeyFor(wornId);
            }
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(fmt::format("Worn: {}", wnm).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(fmt::format("{}##pwn{:X}", wbl ? "Whitelist" : "Blacklist", wornId).c_str())) {
                Slots::SetPackBlacklisted(wornId, !wbl);
                SaveDeferred();
                Display::g_dirty = true;
                KickReconcile();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(wbl ? "treat this worn item as a backpack again" :
                                        "stop treating this worn item as a backpack\n(back slots keep their normal positions)");
            }
        }
        if (Display::g_packClaimants.empty() && Slots::g_blacklistSpecs.empty()) {
            ImGui::TextDisabled("wear a pack and it appears here for blacklisting");
        }
        for (std::size_t i = 0; i < Slots::g_blacklistSpecs.size(); ++i) {
            const auto& spec = Slots::g_blacklistSpecs[i];
            const auto id = Slots::SpecToForm(spec);
            auto nm = id ? Slots::FriendlyName(id) : std::string{};
            if (nm.empty()) {
                nm = spec;
            }
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(nm.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(fmt::format("Whitelist##pwl{}", i).c_str())) {
                if (id) {
                    Slots::SetPackBlacklisted(id, false);
                } else {
                    Slots::g_blacklistSpecs.erase(Slots::g_blacklistSpecs.begin() +
                                                  static_cast<std::ptrdiff_t>(i));
                }
                SaveDeferred();
                Display::g_dirty = true;
                KickReconcile();
                break;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("remove from the blacklist - this item behaves as a backpack again");
            }
        }
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::SeparatorText("Model Config (edit in VisibleFavorites.ini)");
        std::string paList, packList;
        for (std::size_t i = 0; i < Slots::g_paTorsoBipedSlots.size(); ++i) {
            paList += fmt::format("{}{}", i ? ", " : "", Slots::g_paTorsoBipedSlots[i]);
        }
        for (std::size_t i = 0; i < Slots::g_backpackBipedSlots.size(); ++i) {
            packList += fmt::format("{}{}", i ? ", " : "", Slots::g_backpackBipedSlots[i]);
        }
        ImGui::TextUnformatted("PA torso slots:");
        ImGui::SameLine(labelCol);
        ImGui::TextColored(ImVec4(0.43f, 0.88f, 0.43f, 1.0f), "%s", paList.c_str());
        ImGui::TextUnformatted("Backpack slots:");
        ImGui::SameLine(labelCol);
        ImGui::TextColored(ImVec4(0.43f, 0.88f, 0.43f, 1.0f), "%s", packList.c_str());
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::SeparatorText("Debug");
        if (ImGui::Button("Clean Displays")) {
            Display::Schedule([]() { Display::ReattachAllDisplays(); }, 0);
        }
        ImGui::SameLine();
        if (ImGui::Button("Reload All Models")) {
            Display::Schedule([]() {
                using namespace Display;
                std::lock_guard lock(g_tablesMutex);
                auto* p3d = Player3D();
                for (int i = 0; i < Slots::MAX_INDEX; ++i) {
                    if (p3d) {
                        RetireSlot(p3d, i);
                    }
                    g_state[i] = SlotState{};
                }
                Reconcile();
            }, 50);
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset All to Default")) {
            Slots::RestoreBaselineTransforms();
            SaveDeferred();
            Display::Schedule([]() { Display::ReattachAllDisplays(); }, 0);
            g_needCapture.store(true);
            Display::g_dirty = true;
            Display::Schedule([]() { Npc::RefreshAll(); }, 0);
        }
        if (ImGui::Checkbox("Verbose log", &Slots::g_verboseLog)) {
            SaveDeferred();
        }
        if (ImGui::Checkbox("Enable overlay", &Slots::g_enableOverlayPending)) {
            SaveDeferred();
        }
        if (Slots::g_enableOverlayPending != Slots::g_enableOverlay) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.40f, 1.0f), "(takes effect after a game restart)");
        }
        ImGui::SetNextItemWidth(fs * 10.0f);
        if (ImGui::SliderFloat("Panel scale", &Slots::g_overlayScale, 0.8f, 3.0f, "%.2f")) {
            ImGui::GetIO().FontGlobalScale = Slots::g_overlayScale;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            SaveDeferred();
        }
    }

    //============= Main Panel =============
    void CollectPartNames(RE::NiAVObject* root, std::vector<std::string>& out) {
        Display::Walk(root, [&](RE::NiAVObject* obj) {
            if (obj == root) {
                return true;
            }
            if (obj->GetAppCulled()) {
                return false;
            }
            const std::string_view nm = obj->name.c_str() ? obj->name.c_str() : "";
            if (!nm.starts_with("VF_")) {
                return true;
            }
            const std::string canon{ Slots::CanonicalNodeName(nm.substr(3)) };
            if (!canon.empty() && !Slots::NameInList(out, canon)) {
                out.push_back(canon);
            }
            return true;
        });
    }

    void DrawHiddenParts(std::uint32_t npcID, int slot, std::uint32_t form, std::uint64_t omod) {
        using namespace Display;
        if (!form) {
            return;
        }
        static std::uint32_t partNpc = 0;
        static int partSlot = -1;
        static std::uint32_t partForm = 0;
        static std::uint64_t partOmod = 0;
        static std::vector<std::string> partNames;
        static char partFilter[64] = {};
        if (partNpc != npcID || partSlot != slot || partForm != form || partOmod != omod) {
            partNpc = npcID;
            partSlot = slot;
            partForm = form;
            partOmod = omod;
            partNames.clear();
            partFilter[0] = 0;
            Schedule([npcID, slot, form]() {
                std::lock_guard lock(g_tablesMutex);
                if (form != partForm) {
                    return;
                }
                auto* p3d = npcID ? nullptr : Player3D();
                auto* node = npcID ? Npc::DisplayNode(npcID, slot) : p3d ? p3d->GetObjectByName(SlotNodeName(slot).c_str()) : nullptr;
                if (!node) {
                    partForm = 0;
                    return;
                }
                std::vector<std::string> names;
                CollectPartNames(node, names);
                if (const auto it = Slots::g_hiddenParts.find(form); it != Slots::g_hiddenParts.end()) {
                    for (const auto& e : it->second) {
                        if (!Slots::NameInList(names, e)) {
                            names.push_back(e);
                        }
                    }
                }
                partNames = std::move(names);
            }, 0);
        }
        if (partNames.empty()) {
            return;
        }
        const auto isHidden = [&](const std::string& nm) {
            const auto it = Slots::g_hiddenParts.find(form);
            return it != Slots::g_hiddenParts.end() && Slots::NameInList(it->second, nm);
        };
        int hiddenCount = 0;
        for (const auto& n : partNames) {
            hiddenCount += isHidden(n) ? 1 : 0;
        }
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Parts");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("hide any part of this weapon while holstered, e.g. a laser sight\nper weapon: applies wherever this weapon shows, player or NPC");
        }
        ImGui::SameLine(ImGui::GetFontSize() * 4.5f);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        const auto preview = fmt::format("{} of {} hidden", hiddenCount, partNames.size());
        if (ImGui::BeginCombo("##parts", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
            ImGui::InputTextWithHint("##partfilter", "filter... (laser, sight, glow)", partFilter, sizeof(partFilter));
            for (std::size_t i = 0; i < partNames.size(); ++i) {
                const auto& nm = partNames[i];
                if (partFilter[0] && !ContainsNoCase(nm, partFilter)) {
                    continue;
                }
                bool hidden = isHidden(nm);
                char cb[160];
                std::snprintf(cb, sizeof(cb), "%.120s##part%zu", nm.c_str(), i);
                if (ImGui::Checkbox(cb, &hidden)) {
                    auto& lst = Slots::g_hiddenParts[form];
                    if (hidden) {
                        lst.push_back(nm);
                    } else {
                        std::erase_if(lst, [&](const std::string& e) { return Slots::SameName(e, nm); });
                        if (lst.empty()) {
                            Slots::g_hiddenParts.erase(form);
                        }
                    }
                    SaveDeferred();
                    Schedule([form]() { ReattachCarrying(form); }, 0);
                    Schedule([form]() { Npc::RefreshCarrying(form); }, 0);
                }
            }
            ImGui::EndCombo();
        }
    }

    void DrawPanel() {
        using namespace Display;
        bool stayOpen = true;
        g_kbOwned.store(ImGui::GetIO().WantTextInput || g_bindSlot != -1);
        {
            bool anyUnsaved = AnyNpcDirty();
            for (int i = 0; i < Slots::MAX_INDEX; ++i) {
                anyUnsaved = anyUnsaved || g_slotDirty[i];
            }
            g_unsaved.store(anyUnsaved);
        }
        const float fs0 = ImGui::GetFontSize();
        if (Slots::g_panelW > 0.0f) {
            ImGui::SetNextWindowPos(ImVec2(Slots::g_panelX, Slots::g_panelY), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(Slots::g_panelW, Slots::g_panelH), ImGuiCond_FirstUseEver);
        } else {
            ImGui::SetNextWindowSize(ImVec2(fs0 * 75.8f, fs0 * 60.2f), ImGuiCond_FirstUseEver);
        }
        if (!ImGui::Begin("VisibleFavorites", &stayOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_MenuBar)) {
            ImGui::End();
            if (!stayOpen) {
                g_open = false;
                logger::info("panel closed from the panel (collapsed)");
            }
            return;
        }
        {
            const auto ws = ImGui::GetWindowSize();
            const auto wp = ImGui::GetWindowPos();
            g_lastW = ws.x;
            g_lastH = ws.y;
            g_lastX = wp.x;
            g_lastY = wp.y;
        }

        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("Slot Manager")) {
                if (ImGui::MenuItem("Favorites", nullptr, g_page == 0)) {
                    g_page = 0;
                    if (g_slot >= Slots::FAV_SLOTS) {
                        g_slot = 3;
                    }
                }
                if (ImGui::MenuItem("NPC Slots", nullptr, g_page == 3)) {
                    g_page = 3;
                }
                if (ImGui::MenuItem("Custom Slots", nullptr, g_page == 1)) {
                    g_page = 1;
                    if (g_slot < Slots::FAV_SLOTS && Slots::CustomCount()) {
                        g_slot = Slots::FAV_SLOTS;
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Settings", nullptr, g_page == 2)) {
                g_page = 2;
            }
            ImGui::EndMenuBar();
        }

        if (g_page == 2) {
            DrawSettingsPage();
            EndPanel(stayOpen);
            return;
        }
        if (g_page == 3) {
            DrawNpcPage();
            EndPanel(stayOpen);
            return;
        }
        if (g_page == 1) {
            DrawCustomStrip();
            if (!Slots::IsCustom(g_slot)) {
                ImGui::TextDisabled("no custom slot selected - use [ + Add ] above");
                EndPanel(stayOpen);
                return;
            }
        } else {
            ImGui::SeparatorText("Favorites");
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            DrawSlotMap();
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            ImGui::SeparatorText("Display");
            const int pMode = std::clamp(Slots::g_playerDisplayMode, 0, 2);
            ImGui::TextUnformatted("Show on Player");
            ImGui::SetNextItemWidth(fs0 * 9.5f);
            if (ImGui::BeginCombo("##playermode", DISPLAY_MODES[pMode + 1])) {
                for (int m = 0; m <= 2; ++m) {
                    if (ImGui::Selectable(DISPLAY_MODES[m + 1], pMode == m)) {
                        Slots::g_playerDisplayMode = m;
                        SaveDeferred();
                        g_dirty = true;
                        KickReconcile();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
        }

        if (g_needCapture.exchange(false)) {
            std::fill(std::begin(g_slotCaptured), std::end(g_slotCaptured), false);
            std::fill(std::begin(g_slotDirty), std::end(g_slotDirty), false);
        }
        const int gv = Slots::IsCustom(g_slot) ? Slots::GroupVSlot(Slots::CustomOf(g_slot).group) : -1;
        const int ps = gv >= 0 ? gv : g_slot;
        const LayerEnv penv = PlayerEnv(ps);
        static int lastPs = -1;
        if (!g_slotCaptured[ps]) {
            g_slotCaptured[ps] = true;
            g_workAll[ps] = *ActiveTransform(ps);
            g_origAll[ps] = g_workAll[ps];
            lastPs = -1;
        }
        if (lastPs != ps) {
            lastPs = ps;
            g_layerSel = g_slotDirty[ps] ? g_slotLayer[ps] : Winner(penv, ps);
        }
        if (!LayerApplicable(penv, g_layerSel, ps) || !LayerExists(penv, g_layerSel, ps)) {
            g_layerSel = Winner(penv, ps);
            if (!g_slotDirty[ps]) {
                g_workAll[ps] = *ActiveTransform(ps);
                g_origAll[ps] = g_workAll[ps];
            }
        }
        const std::string slotValue = std::to_string(g_slot);

        ImGui::SeparatorText("Slot Info");
        const float labelCol = ImGui::GetFontSize() * 4.5f;
        if (!Slots::IsCustom(g_slot)) {
            const std::string itemValue = g_state[g_slot].object ?
                                              SafeName(g_state[g_slot].object) :
                                              std::string("Empty");
            LabeledValue("Item", itemValue.c_str(), false);
            const std::string specValue = g_state[g_slot].object ?
                                              Slots::SpecKeyFor(g_state[g_slot].object->GetFormID()) :
                                              std::string("-");
            LabeledValue("Form ID", specValue.c_str(), false);
            LabeledValue("Fav Slot", slotValue.c_str(), false);
            LabeledValue("Slot Name", Slots::g_slots[g_slot].label.c_str(), false);
            LabeledValue("Bone", Slots::g_slots[g_slot].bone.c_str(), false);
            if (ImGui::Checkbox("Slot enabled", &Slots::g_slotEnabled[g_slot])) {
                logger::info("slot {} {} by the user", g_slot, Slots::g_slotEnabled[g_slot] ? "enabled" : "disabled");
                SaveDeferred();
                Display::g_dirty = true;
                KickReconcile();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("off = the favorites wheel is ignored for this slot, nothing is displayed here\nthe equipped weapon still uses its regular slot");
            }
        } else {
            auto& c = Slots::CustomOf(g_slot);
            LabeledValue("Fav Slot", slotValue.c_str(), false);
            LabeledValue("Form ID", c.itemSpec.c_str(), false);
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Item");
            ImGui::SameLine(labelCol);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
            ImGui::SetNextWindowSizeConstraints(
                ImVec2(ImGui::GetFontSize() * 26.0f, ImGui::GetFontSize() * 10.0f),
                ImVec2(ImGui::GetFontSize() * 34.0f, ImGui::GetFontSize() * 30.0f));
            if (ImGui::BeginCombo("##csitem", c.label.c_str())) {
                if (ImGui::IsWindowAppearing() && !g_pickBusy.exchange(true)) {
                    Display::Schedule([]() {
                        std::lock_guard lock(Display::g_tablesMutex);
                        BuildItemLists();
                        g_pickBusy = false;
                    }, 0);
                }
                if (const int p = DrawItemPickList(ImGui::GetFontSize()); p >= 0) {
                    const auto& src = g_pickAll ? g_pickItemsAll : g_pickItems;
                    c.itemSpec = Slots::FormToSpec(src[p].object->GetFormID());
                    c.fingerprint = src[p].hash;
                    c.label = SafeName(src[p].object).substr(0, 24);
                    Slots::g_slots[g_slot].label = c.label;
                    SaveDeferred();
                    RebuildSlotDisplay(g_slot);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndCombo();
            }
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Node");
            ImGui::SameLine(labelCol);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
            if (ImGui::BeginCombo("##csnode", Slots::g_slots[g_slot].bone.c_str())) {
                if (ImGui::IsWindowAppearing() && !g_pickBusy.exchange(true)) {
                    Display::Schedule([]() {
                        std::lock_guard lock(Display::g_tablesMutex);
                        BuildNodeList();
                        g_pickBusy = false;
                    }, 0);
                }
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
                ImGui::InputTextWithHint("##csnodef", "filter...", g_nodeFilter, sizeof(g_nodeFilter));
                ImGui::Checkbox("Show all bone nodes##csall", &g_showAllBones);
                for (std::size_t n = 0; n < g_pickNodes.size(); ++n) {
                    if (g_nodeFilter[0] && !ContainsNoCase(g_pickNodes[n], g_nodeFilter)) {
                        continue;
                    }
                    if (!g_showAllBones && !g_nodeFilter[0] && !MainBone(g_pickNodes[n]) &&
                        g_pickNodes[n] != Slots::g_slots[g_slot].bone) {
                        continue;
                    }
                    char nn[160];
                    std::snprintf(nn, sizeof(nn), "%.120s##csn%zu", g_pickNodes[n].c_str(), n);
                    if (ImGui::Selectable(nn, g_pickNodes[n] == Slots::g_slots[g_slot].bone)) {
                        Slots::g_slots[g_slot].bone = g_pickNodes[n];
                        SaveDeferred();
                        RebuildSlotDisplay(g_slot);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            const auto boundID = Slots::SpecToForm(c.itemSpec);
            auto* boundForm = boundID ? RE::TESForm::GetFormByID(boundID) : nullptr;
            const bool equippable = boundForm &&
                                    (boundForm->As<RE::TESObjectWEAP>() || boundForm->As<RE::TESObjectARMO>() ||
                                        boundForm->As<RE::AlchemyItem>());
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Hotkey");
            ImGui::SameLine(labelCol);
            if (g_bindSlot == g_slot) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.85f, 0.40f, 1.0f));
                if (ImGui::Button("press a key or pad button...##hkbind")) {
                    g_bindSlot = -1;
                }
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("esc cancels; hold Ctrl/Alt/Shift while pressing a key for a combo");
                }
                bool done = false;
                if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                    g_bindSlot = -1;
                    done = true;
                }
                for (int vk = 0x08; vk <= 0xFE && !done; ++vk) {
                    if (vk == VK_ESCAPE || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
                        (vk >= VK_LSHIFT && vk <= VK_RMENU)) {
                        continue;
                    }
                    if (GetAsyncKeyState(vk) & 0x8000) {
                        c.hotkey = vk | Slots::CurrentModsRaw();
                        if (Slots::StealHotkey(c.hotkey, false, c.id)) {
                            Display::Hud("VisibleFavorites: key taken from its previous owner", "UIMenuOK");
                        }
                        g_bindSlot = -1;
                        SaveDeferred();
                        done = true;
                    }
                }
                if (!done && g_bindSlot == g_slot) {
                    if (const auto pads = Display::ReadPadButtons(); pads != 0) {
                        c.hotkey = 0x10000 | static_cast<int>(pads & (~pads + 1));
                        if (Slots::StealHotkey(c.hotkey, false, c.id)) {
                            Display::Hud("VisibleFavorites: key taken from its previous owner", "UIMenuOK");
                        }
                        g_bindSlot = -1;
                        SaveDeferred();
                    }
                }
            } else {
                if (!equippable) {
                    ImGui::BeginDisabled();
                }
                if (c.hotkey) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.88f, 0.45f, 1.0f));
                }
                char hkBtn[96];
                std::snprintf(hkBtn, sizeof(hkBtn), "%s##hkbind",
                    c.hotkey ? Display::HotkeyName(c.hotkey).c_str() : "Bind");
                if (ImGui::Button(hkBtn)) {
                    g_bindSlot = g_slot;
                }
                if (c.hotkey) {
                    ImGui::PopStyleColor();
                }
                if (!equippable) {
                    ImGui::EndDisabled();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip(equippable ?
                                          "bind a key (Ctrl/Alt/Shift combos work) or a gamepad button that\nequips/unequips this item - aid items are used on press" :
                                          "only weapons, armor and aid can be equipped by hotkey");
                }
                ImGui::SameLine();
                const bool unbound = c.hotkey == 0;
                if (unbound) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Button("Clear##hkclear")) {
                    c.hotkey = 0;
                    SaveDeferred();
                }
                if (unbound) {
                    ImGui::EndDisabled();
                }
            }
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            bool condChanged = false;
            condChanged |= ImGui::Checkbox("Hide when not in inventory", &c.hideNotInInventory);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("player only");
            }
            ImGui::SameLine();
            condChanged |= ImGui::Checkbox("Hide when equipped", &c.hideWhenEquipped);
            ImGui::SameLine();
            if (ImGui::Checkbox("Show on NPCs", &c.showOnNpc)) {
                SaveDeferred();
                Display::Schedule([]() { Npc::RefreshAll(); }, 0);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("NPC must have at least one of this item in their inventory");
            }
            if (condChanged) {
                if (c.group != 0) {
                    for (int gi = 0; gi < Slots::CustomCount(); ++gi) {
                        auto& other = Slots::g_custom[gi];
                        if (other.group == c.group) {
                            other.hideNotInInventory = c.hideNotInInventory;
                            other.hideWhenEquipped = c.hideWhenEquipped;
                        }
                    }
                }
                SaveDeferred();
                g_dirty = true;
                KickReconcile();
            }
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Group");
            ImGui::SameLine(labelCol);
            auto* grp = Slots::FindGroup(c.group);
            int action = 0;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
            if (ImGui::BeginCombo("##csgrp", grp ? grp->label.c_str() : "No Group")) {
                if (ImGui::Selectable("No Group", !grp) && grp) {
                    action = -1;
                }
                for (const auto& g : Slots::g_groups) {
                    int n = 0;
                    for (const auto& cc : Slots::g_custom) {
                        n += cc.group == g.id ? 1 : 0;
                    }
                    char gl[96];
                    std::snprintf(gl, sizeof(gl), "%s (%d)##g%d", g.label.c_str(), n, g.id);
                    if (ImGui::Selectable(gl, grp && grp->id == g.id) && (!grp || grp->id != g.id)) {
                        action = g.id;
                    }
                }
                if (Slots::GroupCount() < Slots::MAX_GROUPS && ImGui::Selectable("+ New Group")) {
                    action = -2;
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("grouped slots move together (Move whole group in the Positioner), share\none loadout hotkey, and can hide together");
            }
            if (action > 0) {
                for (const auto& other : Slots::g_custom) {
                    if (&other != &c && other.group == action) {
                        c.hideNotInInventory = other.hideNotInInventory;
                        c.hideWhenEquipped = other.hideWhenEquipped;
                        break;
                    }
                }
            }
            if (action != 0) {
                Slots::LeaveGroup(c);
                if (action == -2) {
                    Slots::CustomGroup g;
                    g.id = Slots::NewGroupId();
                    g.label = fmt::format("Group {}", g.id);
                    g.color = (g.id - 1) % static_cast<int>(std::size(GROUP_COLORS));
                    c.group = g.id;
                    Slots::g_groups.push_back(std::move(g));
                    const int v = Slots::MAX_SLOTS + Slots::GroupCount() - 1;
                    Slots::g_slots[v] = Slots::SlotDef{};
                    Slots::g_slots[v].label = Slots::g_groups.back().label;
                    Slots::g_slotsPA[v] = Slots::Transform{};
                } else if (action > 0) {
                    c.group = action;
                }
                g_needCapture.store(true);
                SaveDeferred();
                g_dirty = true;
                KickReconcile();
                ReapplyAllTransforms();
            }
            grp = Slots::FindGroup(c.group);
            if (grp) {
                ImGui::Dummy(ImVec2(0.0f, 2.0f));
                const int prevGroupHk = grp->hotkey;
                BindKeyRow("Group Hotkey", labelCol, -1000 - grp->id, grp->hotkey, true, true, false,
                    "one key for the whole group: press = equip every carried member,\npress again = take them all off (a loadout key)");
                if (grp->hotkey != prevGroupHk && grp->hotkey) {
                    if (Slots::StealHotkey(grp->hotkey, true, grp->id)) {
                        Display::Hud("VisibleFavorites: key taken from its previous owner", "UIMenuOK");
                        SaveDeferred();
                    }
                }
            }
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            if (grp) {
                if (ImGui::Button("Delete Group##gdel")) {
                    Slots::DeleteGroupData(grp->id);
                    g_needCapture.store(true);
                    SaveDeferred();
                    g_dirty = true;
                    KickReconcile();
                    ReapplyAllTransforms();
                    EndPanel(stayOpen);
                    return;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("disband this group: members stay as individual slots and return to their\nown saved offsets (the group's layers and loadout key are discarded)");
                }
                ImGui::SameLine();
            }
            if (ImGui::Button("Delete Slot##csdel")) {
                DeleteCustomSlot(g_slot);
                EndPanel(stayOpen);
                return;
            }
        }
        DrawHiddenParts(0, g_slot, g_state[g_slot].source ? g_state[g_slot].formID : 0, g_state[g_slot].omodHash);
        ImGui::Dummy(ImVec2(0.0f, 6.0f));

        if (ps != g_slot) {
            ImGui::SeparatorText(fmt::format("Slot Position - Group: {}",
                Slots::g_groups[ps - Slots::MAX_SLOTS].label)
                                     .c_str());
        } else {
            ImGui::SeparatorText("Slot Position");
        }
        LayerEditor editor = PlayerEditor();
        const bool slotOff = !Slots::IsCustom(g_slot) && !Slots::g_slotEnabled[g_slot];
        ImGui::BeginDisabled(slotOff);
        DrawLayerEditor(penv, ps, editor);
        ImGui::EndDisabled();
        EndPanel(stayOpen);
    }
}
