#include "Config.h"

#include "DisplayManager.h"
#include "Logger.h"
#include "NpcDisplay.h"
#include "SlotManager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace Slots
{
    static IniLines g_partsUnresolved;

    static std::string SlotToken(int slot) {
        if (slot >= MAX_SLOTS) {
            return fmt::format("G{}", g_groups[slot - MAX_SLOTS].id);
        }
        return slot < FAV_SLOTS ? std::to_string(slot)
                                  : fmt::format("C{}", g_custom[slot - FAV_SLOTS].id);
    }

    static int ParseSlotToken(const std::string& tok) {
        if (!tok.empty() && (tok[0] == 'C' || tok[0] == 'c')) {
            const int id = std::atoi(tok.c_str() + 1);
            for (int i = 0; i < CustomCount(); ++i) {
                if (g_custom[i].id == id) {
                    return FAV_SLOTS + i;
                }
            }
            return -1;
        }
        if (!tok.empty() && (tok[0] == 'G' || tok[0] == 'g')) {
            return GroupVSlot(std::atoi(tok.c_str() + 1));
        }
        const int s = std::atoi(tok.c_str());
        return s >= 0 && s < FAV_SLOTS ? s : -1;
    }

    static void RememberSpec(std::uint32_t id, const std::string& spec) {
        if (spec.find('|') != std::string::npos) {
            g_specOf[id] = spec;
        }
    }

    static std::string CtxSpec(std::uint32_t id) {
        switch (id) {
        case ANY_ARMOR:
            return "AnyArmor";
        case ANY_PACK:
            return "AnyBackpack";
        case ANY_OVER_ARMOR:
            return "AnyOverArmor";
        case ANY_PA:
            return "AnyPowerArmor";
        default:
            return SpecKeyFor(id);
        }
    }

    static std::uint32_t CtxSpecToForm(const std::string& spec) {
        if (spec == "AnyArmor") {
            return ANY_ARMOR;
        }
        if (spec == "AnyBackpack") {
            return ANY_PACK;
        }
        if (spec == "AnyOverArmor") {
            return ANY_OVER_ARMOR;
        }
        if (spec == "AnyPowerArmor") {
            return ANY_PA;
        }
        return SpecToForm(spec);
    }

    std::string IniSafe(std::string s) {
        for (auto& ch : s) {
            if (static_cast<unsigned char>(ch) < 0x20) {
                ch = ' ';
            }
        }
        return s;
    }

    //============= Hotkey Names =============

    struct VkIniEntry
    {
        int vk;
        const char* name;
    };

    constexpr VkIniEntry VK_INI_NAMES[] = {
        { 0x08, "Backspace" }, { 0x09, "Tab" }, { 0x0D, "Enter" },
        { 0x10, "Shift" }, { 0x11, "Ctrl" }, { 0x12, "Alt" },
        { 0x13, "Pause" }, { 0x14, "CapsLock" }, { 0x20, "Space" },
        { 0x21, "PgUp" }, { 0x22, "PgDn" }, { 0x23, "End" }, { 0x24, "Home" },
        { 0x25, "Left" }, { 0x26, "Up" }, { 0x27, "Right" }, { 0x28, "Down" },
        { 0x2C, "PrintScreen" }, { 0x2D, "Insert" }, { 0x2E, "Delete" },
        { 0x5B, "LWin" }, { 0x5C, "RWin" }, { 0x5D, "Apps" },
        { 0x6A, "NumpadStar" }, { 0x6B, "NumpadPlus" }, { 0x6D, "NumpadMinus" },
        { 0x6E, "NumpadDot" }, { 0x6F, "NumpadSlash" },
        { 0x90, "NumLock" }, { 0x91, "ScrollLock" },
        { 0xA0, "LShift" }, { 0xA1, "RShift" }, { 0xA2, "LCtrl" }, { 0xA3, "RCtrl" },
        { 0xA4, "LAlt" }, { 0xA5, "RAlt" },
        { 0xBA, "Semicolon" }, { 0xBB, "Equals" }, { 0xBC, "Comma" }, { 0xBD, "Minus" },
        { 0xBE, "Period" }, { 0xBF, "Slash" }, { 0xC0, "Tilde" },
        { 0xDB, "LBracket" }, { 0xDC, "Backslash" }, { 0xDD, "RBracket" }, { 0xDE, "Quote" }
    };

    static std::string VkIniName(int vk) {
        if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
            return std::string(1, static_cast<char>(vk));
        }
        if (vk >= 0x70 && vk <= 0x87) {
            return fmt::format("F{}", vk - 0x70 + 1);
        }
        if (vk >= 0x60 && vk <= 0x69) {
            return fmt::format("Numpad{}", vk - 0x60);
        }
        for (const auto& e : VK_INI_NAMES) {
            if (e.vk == vk) {
                return e.name;
            }
        }
        return {};
    }

    static int VkFromIniName(const std::string& tok) {
        if (tok.empty()) {
            return 0;
        }
        if (tok.size() == 1) {
            const char c = tok[0];
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) {
                return c;
            }
            if (c >= 'a' && c <= 'z') {
                return c - 'a' + 'A';
            }
            return 0;
        }
        if ((tok[0] == 'F' || tok[0] == 'f') && tok[1] >= '0' && tok[1] <= '9') {
            const int n = std::atoi(tok.c_str() + 1);
            return n >= 1 && n <= 24 ? 0x70 + n - 1 : 0;
        }
        if (tok.size() == 7 && !_strnicmp(tok.c_str(), "Numpad", 6) && tok[6] >= '0' && tok[6] <= '9') {
            return 0x60 + (tok[6] - '0');
        }
        for (const auto& e : VK_INI_NAMES) {
            if (!_stricmp(tok.c_str(), e.name)) {
                return e.vk;
            }
        }
        return 0;
    }

    static std::string HotkeyToIni(int hk) {
        if (hk == 0) {
            return "None";
        }
        if (hk & 0x10000) {
            const std::string pad = Display::PadButtonName(static_cast<std::uint32_t>(hk) & 0xFFFF);
            return pad == "Pad ?" ? std::to_string(hk) : pad;
        }
        const auto keyName = VkIniName(hk & 0xFF);
        if (keyName.empty()) {
            return std::to_string(hk);
        }
        std::string s;
        if (hk & 0x20000) {
            s += "Ctrl+";
        }
        if (hk & 0x80000) {
            s += "Alt+";
        }
        if (hk & 0x40000) {
            s += "Shift+";
        }
        s += keyName;
        if (s.find_first_not_of("0123456789") == std::string::npos) {
            return std::to_string(hk);
        }
        return s;
    }

    static int HotkeyFromIni(const std::string& val) {
        std::string v = val;
        while (!v.empty() && v.front() == ' ') {
            v.erase(v.begin());
        }
        while (!v.empty() && v.back() == ' ') {
            v.pop_back();
        }
        if (v.empty()) {
            return 0;
        }
        if (v.front() == '-' || (v.front() >= '0' && v.front() <= '9')) {
            return std::atoi(v.c_str());
        }
        if (!_stricmp(v.c_str(), "None")) {
            return 0;
        }
        for (std::uint32_t mask = 1; mask <= 0x8000u; mask <<= 1) {
            const char* pad = Display::PadButtonName(mask);
            if (std::strcmp(pad, "Pad ?") != 0 && !_stricmp(v.c_str(), pad)) {
                return 0x10000 | static_cast<int>(mask);
            }
        }
        int mods = 0;
        std::size_t start = 0;
        for (;;) {
            const auto plus = v.find('+', start);
            std::string tok = plus == std::string::npos ? v.substr(start) : v.substr(start, plus - start);
            while (!tok.empty() && tok.front() == ' ') {
                tok.erase(tok.begin());
            }
            while (!tok.empty() && tok.back() == ' ') {
                tok.pop_back();
            }
            if (plus == std::string::npos) {
                const int vk = VkFromIniName(tok);
                return vk ? (vk | (mods & ~ModClassOfVk(vk))) : 0;
            }
            if (!_stricmp(tok.c_str(), "Ctrl")) {
                mods |= 0x20000;
            } else if (!_stricmp(tok.c_str(), "Alt")) {
                mods |= 0x80000;
            } else if (!_stricmp(tok.c_str(), "Shift")) {
                mods |= 0x40000;
            } else {
                return 0;
            }
            start = plus + 1;
        }
    }

    //============= INI Persistence =============

    static std::string IniPath() {
        return (std::filesystem::current_path() / "Data\\F4SE\\Plugins\\VisibleFavorites\\VisibleFavorites.ini").string();
    }

    static std::string TLine(const Transform& t) {
        return fmt::format("{:.4f},{:.4f},{:.4f},{:.5f},{:.5f},{:.5f},{:.3f}{}",
            t.px, t.py, t.pz, t.rx, t.ry, t.rz, t.scale, t.hidden ? ",1" : "");
    }

    static void WriteLadder(std::ostream& f, const std::string& pre, const LadderSet& L) {
        f << "[" << pre << "BackpackGeneric]\n";
        for (const auto& [slot, t] : L.packGeneric) {
            f << slot << "=" << TLine(t) << "\n";
        }
        f << "\n[" << pre << "BackpackOverrides]\n";
        for (const auto& [pack, slots] : L.packOverrides) {
            for (const auto& [slot, t] : slots) {
                f << SpecKeyFor(pack) << "." << SlotToken(slot) << "=" << TLine(t) << "\n";
            }
        }
        for (const auto& [k, v] : L.packUnresolved) {
            f << k << "=" << v << "\n";
        }
        f << "\n[" << pre << "ArmorOverrides]\n";
        for (const auto& [armor, slots] : L.armorOverrides) {
            for (const auto& [slot, t] : slots) {
                f << SpecKeyFor(armor) << "." << SlotToken(slot) << "=" << TLine(t) << "\n";
            }
        }
        for (const auto& [k, v] : L.armorUnresolved) {
            f << k << "=" << v << "\n";
        }
        f << "\n[" << pre << "WeaponOverrides]\n";
        for (const auto& [wid, entries] : L.weaponOverrides) {
            for (const auto& [wkey, t] : entries) {
                const auto [slot, aid] = wkey;
                f << SpecKeyFor(wid) << (aid ? ">" + CtxSpec(aid) : std::string{}) << "." << SlotToken(slot)
                  << "=" << TLine(t) << "\n";
            }
        }
        for (const auto& [k, v] : L.weaponUnresolved) {
            f << k << "=" << v << "\n";
        }
        f << "\n";
    }

    void Save() {
        std::ostringstream f;
        const auto csv = [&](const char* key, const auto& list) {
            f << key << "=";
            for (std::size_t i = 0; i < list.size(); ++i) {
                f << (i ? "," : "") << list[i];
            }
            f << "\n";
        };
        f << "[General]\n";
        f << "bDisplayAnyItemType=" << (g_displayAnyItemType ? 1 : 0) << "\n";
        f << "bShowWeaponFX=" << (g_showWeaponFX ? 1 : 0) << "\n";
        f << "bHideWhenNoBodyArmor=" << (g_hideWhenNoBodyArmor ? 1 : 0) << "\n";
        f << "iPlayerDisplay=" << g_playerDisplayMode << "\n";
        csv("DisplayBlacklist", g_displayBlacklistSpecs);
        f << "bEnableOverlay=" << (g_enableOverlayPending ? 1 : 0) << "\n";
        f << "bVerboseLog=" << (g_verboseLog ? 1 : 0) << "\n";
        if (g_panelW > 0.0f) {
            f << fmt::format("fPanelX={:.0f}\nfPanelY={:.0f}\nfPanelW={:.0f}\nfPanelH={:.0f}\n",
                g_panelX, g_panelY, g_panelW, g_panelH);
        }
        f << fmt::format("fOverlayScale={:.2f}\n", g_overlayScale);
        csv("PowerArmorTorsoSlots", g_paTorsoBipedSlots);
        csv("BackpackSlots", g_backpackBipedSlots);
        csv("BackpackBlacklist", g_blacklistSpecs);
        f << "\n[Hotkeys]\n";
        f << "iOpenKey=" << HotkeyToIni(g_openKey) << "\n";
        f << "iHideAllKey=" << HotkeyToIni(g_hideAllKey) << "\n";
        Npc::WriteIni(f);
        f << "\n";
        const auto writeT = [&](const Transform& t) {
            f << fmt::format("Pos={:.4f},{:.4f},{:.4f}\n", t.px, t.py, t.pz);
            f << fmt::format("Rot={:.5f},{:.5f},{:.5f}\n", t.rx, t.ry, t.rz);
            f << fmt::format("Scale={:.3f}\n", t.scale);
            if (t.hidden) {
                f << "Hidden=1\n";
            }
            f << "\n";
        };
        for (int i = 0; i < FAV_SLOTS; ++i) {
            const auto& s = g_slots[i];
            f << "[Slot" << i << "]\n";
            f << "Bone=" << IniSafe(s.bone) << "\n";
            f << "Fav=" << g_slotFav[i] << "\n";
            if (!g_slotEnabled[i]) {
                f << "Enabled=0\n";
            }
            writeT(s.t);
        }
        for (int i = 0; i < FAV_SLOTS; ++i) {
            f << "[Slot" << i << ".PA]\n";
            writeT(g_slotsPA[i]);
        }
        f << "[CustomSlots]\n";
        f << "NextId=" << g_nextCustomId << "\n\n";
        for (int i = 0; i < CustomCount(); ++i) {
            const auto& c = g_custom[i];
            const int slot = FAV_SLOTS + i;
            f << "[CustomSlot" << c.id << "]\n";
            f << "Label=" << c.label << "\n";
            f << "Bone=" << IniSafe(g_slots[slot].bone) << "\n";
            f << "Item=" << c.itemSpec << "\n";
            f << fmt::format("Fingerprint={:X}\n", c.fingerprint);
            f << "HideNotInInventory=" << (c.hideNotInInventory ? 1 : 0) << "\n";
            f << "HideWhenEquipped=" << (c.hideWhenEquipped ? 1 : 0) << "\n";
            f << "ShowOnNPC=" << (c.showOnNpc ? 1 : 0) << "\n";
            f << "Group=" << c.group << "\n";
            f << "Hotkey=" << HotkeyToIni(c.hotkey) << "\n";
            writeT(g_slots[slot].t);
            f << "[CustomSlot" << c.id << ".PA]\n";
            writeT(g_slotsPA[slot]);
        }
        for (int gi = 0; gi < GroupCount(); ++gi) {
            const auto& g = g_groups[gi];
            f << "[Group" << g.id << "]\n";
            f << "Label=" << g.label << "\n";
            f << "Color=" << g.color << "\n";
            f << "Hotkey=" << HotkeyToIni(g.hotkey) << "\n";
            writeT(g_slots[MAX_SLOTS + gi].t);
            f << "[Group" << g.id << ".PA]\n";
            writeT(g_slotsPA[MAX_SLOTS + gi]);
        }
        WriteLadder(f, "", g_player);
        const auto writeNpcSet = [&](const NpcTableSet& set, const std::string& pre) {
            for (int i = 0; i < FAV_SLOTS; ++i) {
                const auto& s = set.slots[i];
                f << "[" << pre << "Slot" << i << "]\n";
                f << "Bone=" << IniSafe(s.bone) << "\n";
                f << "Families=";
                bool famFirst = true;
                for (int fam = 0; fam < Npc::FamilyCount(); ++fam) {
                    if (s.families & (1 << fam)) {
                        f << (famFirst ? "" : ",") << fam;
                        famFirst = false;
                    }
                }
                f << "\n";
                f << "Priority=" << s.prio << "\n";
                writeT(s.t);
            }
            for (int i = 0; i < FAV_SLOTS; ++i) {
                f << "[" << pre << "Slot" << i << ".PA]\n";
                writeT(set.slotsPA[i]);
            }
            WriteLadder(f, pre, set);
        };
        writeNpcSet(g_npcSet, "NPC");
        for (std::size_t gi = 0; gi < g_npcConfigs.size(); ++gi) {
            const auto& cfg = g_npcConfigs[gi];
            f << "[NPCConfig" << gi << "]\n";
            f << "Label=" << IniSafe(cfg.label) << "\n";
            f << "Mode=" << cfg.mode << "\n";
            f << "Members=";
            for (std::size_t mi = 0; mi < cfg.memberSpecs.size(); ++mi) {
                f << (mi ? "," : "") << cfg.memberSpecs[mi];
            }
            f << "\n\n";
            writeNpcSet(cfg.tables, fmt::format("NPCConfig{}.", gi));
        }
        if (!g_hiddenParts.empty() || !g_partsUnresolved.empty()) {
            f << "[WeaponFX]\n";
            for (const auto& [id, names] : g_hiddenParts) {
                f << SpecKeyFor(id) << "=";
                for (std::size_t i = 0; i < names.size(); ++i) {
                    f << (i ? "," : "") << IniSafe(names[i]);
                }
                f << "\n";
            }
            for (const auto& [k, v] : g_partsUnresolved) {
                f << k << "=" << v << "\n";
            }
            f << "\n";
        }

        const std::string text = f.str();
        const std::string dst = IniPath();
        if (std::ifstream cur(dst); cur.is_open()) {
            std::string existing((std::istreambuf_iterator<char>(cur)), std::istreambuf_iterator<char>());
            if (existing == text) {
                logger::info("INI unchanged ({} bytes) - not rewritten", text.size());
                return;
            }
        }
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(dst).parent_path(), ec);
        if (ec) {
            logger::error("could not create {} ({})", std::filesystem::path(dst).parent_path().string(), ec.message());
        }
        const std::string tmpPath = dst + ".tmp";
        std::ofstream tmp(tmpPath, std::ios::trunc);
        if (!tmp.is_open() || !(tmp << text) || !tmp.flush()) {
            logger::error("write to {} failed - live INI untouched", tmpPath);
            return;
        }
        tmp.close();
        const DWORD attrs = GetFileAttributesA(dst.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY)) {
            if (SetFileAttributesA(dst.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY)) {
                logger::info("cleared read-only attribute on {}", dst);
            } else {
                logger::error("could not clear read-only attribute on {} (error {})", dst, GetLastError());
            }
        }
        const bool backedUp = attrs != INVALID_FILE_ATTRIBUTES && CopyFileA(dst.c_str(), (dst + ".bak").c_str(), FALSE);
        if (attrs != INVALID_FILE_ATTRIBUTES && !backedUp) {
            logger::warn("could not back up {} to .bak (error {})", dst, GetLastError());
        }
        if (!CopyFileA(tmpPath.c_str(), dst.c_str(), FALSE)) {
            logger::error("INI write-in-place failed (error {}) - new config kept in {}{}", GetLastError(), tmpPath,
                backedUp ? ", previous in .bak" : "");
            return;
        }
        if (!DeleteFileA(tmpPath.c_str())) {
            logger::warn("could not delete {} (error {})", tmpPath, GetLastError());
        }
        std::string written;
        if (std::ifstream back(dst); back.is_open()) {
            written.assign(std::istreambuf_iterator<char>(back), std::istreambuf_iterator<char>());
        }
        if (written != text) {
            logger::error("INI read-back mismatch after write to {} ({} bytes written, {} read)", dst, text.size(), written.size());
            return;
        }
        logger::info("positions saved to {} ({} bytes, read back OK)", dst, text.size());
    }

    static void ReadVec3(const std::string& val, float& x, float& y, float& z) {
        float vx, vy, vz;
        if (sscanf_s(val.c_str(), "%f,%f,%f", &vx, &vy, &vz) == 3 &&
            std::isfinite(vx) && std::isfinite(vy) && std::isfinite(vz)) {
            x = vx;
            y = vy;
            z = vz;
        } else {
            logger::warn("INI: could not parse '{}' as x,y,z - keeping the previous value", val);
        }
    }

    static void ReadScale(const std::string& val, float& out) {
        float s;
        if (sscanf_s(val.c_str(), "%f", &s) == 1 && std::isfinite(s)) {
            out = s;
        } else {
            logger::warn("INI: could not parse '{}' as a scale - keeping the previous value", val);
        }
    }

    static bool ParseTransform7(const std::string& val, Transform& t) {
        int hid = 0;
        const int got = sscanf_s(val.c_str(), "%f,%f,%f,%f,%f,%f,%f,%d",
            &t.px, &t.py, &t.pz, &t.rx, &t.ry, &t.rz, &t.scale, &hid);
        t.hidden = got == 8 && hid != 0;
        const bool finite = std::isfinite(t.px) && std::isfinite(t.py) && std::isfinite(t.pz) &&
                            std::isfinite(t.rx) && std::isfinite(t.ry) && std::isfinite(t.rz) &&
                            std::isfinite(t.scale);
        return got >= 7 && finite;
    }

    static void ReadIntList(const std::string& val, std::vector<int>& out) {
        out.clear();
        for (const auto& tok : SplitCsv(val)) {
            out.push_back(std::atoi(tok.c_str()));
        }
    }

    static void ReadSpecList(const std::string& val, std::vector<std::string>& specs, std::vector<std::uint32_t>& ids) {
        specs = SplitCsv(val);
        ids.clear();
        for (const auto& tok : specs) {
            if (const auto id = SpecToForm(tok); id) {
                ids.push_back(id);
            }
        }
    }

    void Load() {
        std::ifstream f(IniPath());
        if (!f.is_open()) {
            logger::info("no INI yet ({}); writing defaults", IniPath());
            Save();
            return;
        }
        f.seekg(0, std::ios::end);
        logger::info("INI loaded: {} bytes from {}", static_cast<long long>(f.tellg()), IniPath());
        f.seekg(0);
        enum class LSect
        {
            None,
            Generic,
            Pack,
            Armor,
            Weapon
        };
        int cur = -1;
        bool paSection = false;
        bool partsSection = false;
        int npcSlotCur = -1;
        int npcSlotPaCur = -1;
        NpcTableSet* npcSet = &g_npcSet;
        int npcCfgCur = -1;
        LSect lsect = LSect::None;
        LadderSet* ladder = &g_player;
        int ladderLimit = MAX_INDEX;
        std::string line;
        while (std::getline(f, line)) {
            std::string trimmed = line;
            while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) {
                trimmed.erase(trimmed.begin());
            }
            while (!trimmed.empty() && (trimmed.back() == '\r' || trimmed.back() == ' ' || trimmed.back() == '\t')) {
                trimmed.pop_back();
            }
            if (trimmed.empty() || trimmed.front() == ';') {
                continue;
            }
            const bool isSection = trimmed.front() == '[' && trimmed.find('=') == std::string::npos;
            const auto sectIs = [&](const char* name) { return isSection && trimmed == name; };
            const auto sectStarts = [&](const char* prefix) { return isSection && trimmed.rfind(prefix, 0) == 0; };
            if (isSection) {
                cur = -1;
                paSection = partsSection = false;
                npcSlotCur = -1;
                npcSlotPaCur = -1;
                npcSet = &g_npcSet;
                npcCfgCur = -1;
                lsect = LSect::None;
                ladder = &g_player;
                ladderLimit = MAX_INDEX;
            }
            if (sectStarts("[NPCConfig")) {
                const int n = std::atoi(trimmed.c_str() + 10);
                if (n < 0 || n >= MAX_NPC_CONFIGS) {
                    continue;
                }
                EnsureNpcConfigs(n + 1);
                npcSet = &g_npcConfigs[n].tables;
                ladder = npcSet;
                ladderLimit = FAV_SLOTS;
                const auto dot = trimmed.find('.');
                if (dot == std::string::npos) {
                    npcCfgCur = n;
                } else if (trimmed.compare(dot, 5, ".Slot") == 0) {
                    const int s = std::atoi(trimmed.c_str() + dot + 5);
                    if (s >= 0 && s < FAV_SLOTS) {
                        if (trimmed.find(".PA]") != std::string::npos) {
                            npcSlotPaCur = s;
                        } else {
                            npcSlotCur = s;
                        }
                    }
                } else if (trimmed.compare(dot, std::string::npos, ".BackpackGeneric]") == 0) {
                    lsect = LSect::Generic;
                } else if (trimmed.compare(dot, std::string::npos, ".BackpackOverrides]") == 0) {
                    lsect = LSect::Pack;
                } else if (trimmed.compare(dot, std::string::npos, ".ArmorOverrides]") == 0) {
                    lsect = LSect::Armor;
                } else if (trimmed.compare(dot, std::string::npos, ".WeaponOverrides]") == 0) {
                    lsect = LSect::Weapon;
                }
                continue;
            }
            if (sectStarts("[NPCSlot")) {
                const int n = std::atoi(trimmed.c_str() + 8);
                if (n >= 0 && n < FAV_SLOTS) {
                    if (trimmed.find(".PA]") != std::string::npos) {
                        npcSlotPaCur = n;
                    } else {
                        npcSlotCur = n;
                    }
                }
                continue;
            }
            if (sectIs("[NPCWeaponOverrides]")) {
                ladder = &g_npcSet;
                ladderLimit = FAV_SLOTS;
                lsect = LSect::Weapon;
                continue;
            }
            if (sectIs("[NPCBackpackGeneric]")) {
                ladder = &g_npcSet;
                ladderLimit = FAV_SLOTS;
                lsect = LSect::Generic;
                continue;
            }
            if (sectIs("[NPCBackpackOverrides]")) {
                ladder = &g_npcSet;
                ladderLimit = FAV_SLOTS;
                lsect = LSect::Pack;
                continue;
            }
            if (sectIs("[NPCArmorOverrides]")) {
                ladder = &g_npcSet;
                ladderLimit = FAV_SLOTS;
                lsect = LSect::Armor;
                continue;
            }
            if (sectIs("[General]") || sectIs("[Hotkeys]") || sectIs("[NPC]")) {
                continue;
            }
            if (sectIs("[BackpackOverrides]")) {
                lsect = LSect::Pack;
                continue;
            }
            if (sectIs("[ArmorOverrides]")) {
                lsect = LSect::Armor;
                continue;
            }
            if (sectIs("[WeaponOverrides]")) {
                lsect = LSect::Weapon;
                continue;
            }
            if (sectIs("[WeaponFX]")) {
                partsSection = true;
                g_hiddenParts.clear();
                g_partsUnresolved.clear();
                continue;
            }
            if (sectIs("[BackpackGeneric]")) {
                lsect = LSect::Generic;
                continue;
            }
            if (sectIs("[CustomSlots]")) {
                cur = -3;
                continue;
            }
            if (sectStarts("[Group")) {
                const int id = std::clamp(std::atoi(trimmed.c_str() + 6), 0, 99999);
                int idx = -1;
                for (int i = 0; i < GroupCount(); ++i) {
                    if (g_groups[i].id == id) {
                        idx = i;
                        break;
                    }
                }
                if (idx < 0 && GroupCount() < MAX_GROUPS) {
                    CustomGroup g;
                    g.id = id;
                    g_groups.push_back(std::move(g));
                    idx = GroupCount() - 1;
                }
                cur = idx >= 0 ? -1000 - idx : MAX_SLOTS;
                paSection = trimmed.find(".PA]") != std::string::npos;
                continue;
            }
            if (sectStarts("[CustomSlot")) {
                const int id = std::clamp(std::atoi(trimmed.c_str() + 11), 0, 99999);
                int idx = -1;
                for (int i = 0; i < CustomCount(); ++i) {
                    if (g_custom[i].id == id) {
                        idx = i;
                        break;
                    }
                }
                if (idx < 0 && CustomCount() < MAX_CUSTOM) {
                    CustomSlot c;
                    c.id = id;
                    g_custom.push_back(std::move(c));
                    idx = CustomCount() - 1;
                    g_nextCustomId = std::max(g_nextCustomId, id + 1);
                }
                cur = idx >= 0 ? FAV_SLOTS + idx : MAX_SLOTS;
                paSection = trimmed.find(".PA]") != std::string::npos;
                continue;
            }
            if (sectStarts("[Slot")) {
                const int s = std::atoi(trimmed.c_str() + 5);
                cur = s >= 0 && s < FAV_SLOTS ? s : MAX_SLOTS;
                paSection = trimmed.find(".PA]") != std::string::npos;
                if (paSection) {
                    g_paLoaded = true;
                }
                continue;
            }
            if (isSection) {
                cur = MAX_SLOTS;
                continue;
            }
            const auto eq = trimmed.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            const std::string key = trimmed.substr(0, eq);
            const std::string val = trimmed.substr(eq + 1);
            if (cur == -3) {
                if (key == "NextId") {
                    g_nextCustomId = std::max(g_nextCustomId, std::atoi(val.c_str()));
                }
                continue;
            }
            if (cur <= -1000) {
                const int gidx = -1000 - cur;
                auto& g = g_groups[static_cast<std::size_t>(gidx)];
                Transform& t = paSection ? g_slotsPA[MAX_SLOTS + gidx] : g_slots[MAX_SLOTS + gidx].t;
                if (key == "Label") {
                    g.label = val;
                    g_slots[MAX_SLOTS + gidx].label = val;
                } else if (key == "Color") {
                    g.color = std::atoi(val.c_str());
                } else if (key == "Hotkey") {
                    g.hotkey = HotkeyFromIni(val);
                } else if (key == "Pos") {
                    ReadVec3(val, t.px, t.py, t.pz);
                } else if (key == "Rot") {
                    ReadVec3(val, t.rx, t.ry, t.rz);
                } else if (key == "Scale") {
                    ReadScale(val, t.scale);
                } else if (key == "Hidden") {
                    t.hidden = std::atoi(val.c_str()) != 0;
                }
                continue;
            }
            if (lsect != LSect::None) {
                Transform t;
                if (lsect == LSect::Generic) {
                    const int slot = std::atoi(key.c_str());
                    if (slot >= 0 && slot < std::min(ladderLimit, MAX_SLOTS) && ParseTransform7(val, t)) {
                        ladder->packGeneric[slot] = t;
                    }
                    continue;
                }
                const auto dot = key.rfind('.');
                if (dot == std::string::npos || !ParseTransform7(val, t)) {
                    continue;
                }
                const std::string specPart = key.substr(0, dot);
                const int slot = ParseSlotToken(key.substr(dot + 1));
                if (lsect == LSect::Weapon) {
                    if (slot < 0 || slot >= ladderLimit) {
                        ladder->weaponUnresolved.emplace_back(key, val);
                        continue;
                    }
                    const auto gt = specPart.find('>');
                    const std::string weaponSpec = gt == std::string::npos ? specPart : specPart.substr(0, gt);
                    const std::string armorSpec = gt == std::string::npos ? std::string{} : specPart.substr(gt + 1);
                    const auto wid = SpecToForm(weaponSpec);
                    const auto aid = armorSpec.empty() ? 0u : CtxSpecToForm(armorSpec);
                    if (wid && (armorSpec.empty() || aid)) {
                        ladder->weaponOverrides[wid][{ slot, aid }] = t;
                        RememberSpec(wid, weaponSpec);
                        if (aid) {
                            RememberSpec(aid, armorSpec);
                        }
                    } else {
                        ladder->weaponUnresolved.emplace_back(key, val);
                    }
                    continue;
                }
                auto& table = lsect == LSect::Pack ? ladder->packOverrides : ladder->armorOverrides;
                auto& unresolved = lsect == LSect::Pack ? ladder->packUnresolved : ladder->armorUnresolved;
                if (slot < 0 || slot >= ladderLimit) {
                    unresolved.emplace_back(key, val);
                    continue;
                }
                if (const auto id = SpecToForm(specPart); id) {
                    table[id][slot] = t;
                    RememberSpec(id, specPart);
                } else {
                    unresolved.emplace_back(key, val);
                }
                continue;
            }
            if (npcCfgCur >= 0) {
                auto& cfg = g_npcConfigs[npcCfgCur];
                if (key == "Label") {
                    cfg.label = val;
                } else if (key == "Mode") {
                    cfg.mode = std::clamp(std::atoi(val.c_str()), -1, 2);
                } else if (key == "Members") {
                    cfg.memberSpecs = SplitCsv(val);
                }
                continue;
            }
            if (npcSlotCur >= 0) {
                auto& def = npcSet->slots[npcSlotCur];
                if (key == "Bone") {
                    def.bone = val;
                } else if (key == "Families") {
                    def.families = 0;
                    for (const auto& tok : SplitCsv(val)) {
                        const int fam = std::atoi(tok.c_str());
                        if (fam >= 0 && fam < Npc::FamilyCount()) {
                            def.families |= 1 << fam;
                        }
                    }
                } else if (key == "Priority") {
                    def.prio = std::atoi(val.c_str());
                } else if (key == "Pos") {
                    ReadVec3(val, def.t.px, def.t.py, def.t.pz);
                } else if (key == "Rot") {
                    ReadVec3(val, def.t.rx, def.t.ry, def.t.rz);
                } else if (key == "Scale") {
                    ReadScale(val, def.t.scale);
                } else if (key == "Hidden") {
                    def.t.hidden = std::atoi(val.c_str()) != 0;
                }
                continue;
            }
            if (npcSlotPaCur >= 0) {
                auto& t = npcSet->slotsPA[npcSlotPaCur];
                if (key == "Pos") {
                    ReadVec3(val, t.px, t.py, t.pz);
                } else if (key == "Rot") {
                    ReadVec3(val, t.rx, t.ry, t.rz);
                } else if (key == "Scale") {
                    ReadScale(val, t.scale);
                } else if (key == "Hidden") {
                    t.hidden = std::atoi(val.c_str()) != 0;
                }
                continue;
            }
            if (partsSection) {
                if (key == "Global") {
                    continue;
                }
                std::vector<std::string> names;
                for (const auto& tok : SplitCsv(val)) {
                    names.emplace_back(CanonicalNodeName(tok));
                }
                if (const auto id = SpecToForm(key); id) {
                    g_hiddenParts[id] = std::move(names);
                    RememberSpec(id, key);
                } else {
                    g_partsUnresolved.emplace_back(key, val);
                }
                continue;
            }
            if (cur < 0) {
                if (Npc::TryLoadKey(key, val)) {
                    continue;
                }
                if (key == "bDisplayAnyItemType") {
                    g_displayAnyItemType = std::atoi(val.c_str()) != 0;
                } else if (key == "bShowWeaponFX") {
                    g_showWeaponFX = std::atoi(val.c_str()) != 0;
                } else if (key == "iOpenKey") {
                    if (const int k = HotkeyFromIni(val); k) {
                        g_openKey = k;
                    }
                } else if (key == "iHideAllKey") {
                    g_hideAllKey = HotkeyFromIni(val);
                } else if (key == "bHideWhenNoBodyArmor") {
                    g_hideWhenNoBodyArmor = std::atoi(val.c_str()) != 0;
                } else if (key == "iPlayerDisplay") {
                    const int m = std::atoi(val.c_str());
                    g_playerDisplayMode = m >= 0 && m <= 2 ? m : 0;
                } else if (key == "bEnableOverlay") {
                    g_enableOverlay = std::atoi(val.c_str()) != 0;
                    g_enableOverlayPending = g_enableOverlay;
                } else if (key == "bVerboseLog") {
                    g_verboseLog = std::atoi(val.c_str()) != 0;
                } else if (key == "fPanelX") {
                    g_panelX = static_cast<float>(std::atof(val.c_str()));
                } else if (key == "fPanelY") {
                    g_panelY = static_cast<float>(std::atof(val.c_str()));
                } else if (key == "fPanelW") {
                    g_panelW = static_cast<float>(std::atof(val.c_str()));
                } else if (key == "fPanelH") {
                    g_panelH = static_cast<float>(std::atof(val.c_str()));
                } else if (key == "fOverlayScale") {
                    g_overlayScale = std::clamp(static_cast<float>(std::atof(val.c_str())), 0.8f, 3.0f);
                } else if (key == "PowerArmorTorsoSlots") {
                    ReadIntList(val, g_paTorsoBipedSlots);
                } else if (key == "BackpackSlots") {
                    ReadIntList(val, g_backpackBipedSlots);
                } else if (key == "BackpackBlacklist") {
                    ReadSpecList(val, g_blacklistSpecs, g_packBlacklist);
                } else if (key == "DisplayBlacklist") {
                    ReadSpecList(val, g_displayBlacklistSpecs, g_displayBlacklist);
                }
                continue;
            }
            if (cur >= MAX_SLOTS) {
                continue;
            }
            if (IsCustom(cur) && !paSection) {
                auto& c = CustomOf(cur);
                if (key == "Label") {
                    c.label = val;
                    g_slots[cur].label = c.label;
                    continue;
                } else if (key == "Item") {
                    c.itemSpec = val;
                    continue;
                } else if (key == "Fingerprint") {
                    c.fingerprint = std::strtoull(val.c_str(), nullptr, 16);
                    continue;
                } else if (key == "HideNotInInventory") {
                    c.hideNotInInventory = std::atoi(val.c_str()) != 0;
                    continue;
                } else if (key == "HideWhenEquipped") {
                    c.hideWhenEquipped = std::atoi(val.c_str()) != 0;
                    continue;
                } else if (key == "ShowOnNPC") {
                    c.showOnNpc = std::atoi(val.c_str()) != 0;
                    continue;
                } else if (key == "Group") {
                    c.group = std::atoi(val.c_str());
                    continue;
                } else if (key == "Hotkey") {
                    c.hotkey = HotkeyFromIni(val);
                    continue;
                }
            }
            Transform& t = paSection ? g_slotsPA[cur] : g_slots[cur].t;
            if (key == "Bone" && !paSection) {
                g_slots[cur].bone = val;
            } else if (key == "Fav" && !paSection && cur < FAV_SLOTS) {
                g_slotFav[cur] = std::atoi(val.c_str());
            } else if (key == "Enabled" && !paSection && cur < FAV_SLOTS) {
                g_slotEnabled[cur] = std::atoi(val.c_str()) != 0;
            } else if (key == "Pos") {
                ReadVec3(val, t.px, t.py, t.pz);
            } else if (key == "Rot") {
                ReadVec3(val, t.rx, t.ry, t.rz);
            } else if (key == "Scale") {
                ReadScale(val, t.scale);
            } else if (key == "Hidden") {
                t.hidden = std::atoi(val.c_str()) != 0;
            }
        }
        if (!g_paLoaded) {
            for (int i = 0; i < FAV_SLOTS; ++i) {
                g_slotsPA[i] = PA_DEFAULTS[i];
            }
        }
        SanitizeHotkeys();
        SanitizeFavMap();
        InvalidateNpcConfigMembership();
        for (int i = 0; i < FAV_SLOTS; ++i) {
            const auto& s = g_slots[i];
            logger::info("slot {} loaded: bone={} pos=({:.2f},{:.2f},{:.2f}) rot=({:.3f},{:.3f},{:.3f}) scale={:.3f}{}{}",
                i, s.bone, s.t.px, s.t.py, s.t.pz, s.t.rx, s.t.ry, s.t.rz, s.t.scale, s.t.hidden ? " hidden" : "",
                g_slotEnabled[i] ? "" : " DISABLED");
        }
        logger::info("anchors loaded (anyItemType={} nakedHide={} packGeneric={} packOverrides={} unresolvedPacks={} paAuthored={} showFX={} hiddenParts={} npcConfigs={} verbose={} overlay={})",
            g_displayAnyItemType, g_hideWhenNoBodyArmor, g_player.packGeneric.size(), g_player.packOverrides.size(), g_player.packUnresolved.size(), g_paLoaded,
            g_showWeaponFX, g_hiddenParts.size(), g_npcConfigs.size(), g_verboseLog, g_enableOverlay);
    }
}
