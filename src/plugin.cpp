#include "Config.h"
#include "DisplayManager.h"
#include "EventSinks.h"
#include "Hooks.h"
#include "InputHandler.h"
#include "Logger.h"
#include "SlotManager.h"
#include "Versions.h"

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* f4se, F4SE::PluginInfo* info) {
    info->infoVersion = F4SE::PluginInfo::kVersion;
    info->name = Version::PROJECT.data();
    info->version = Version::MAJOR;
    return !f4se->IsEditor();
}

extern "C" DLLEXPORT constinit auto F4SEPlugin_Version = []() noexcept {
    F4SE::PluginVersionData data{};
    data.PluginVersion(REL::Version{ static_cast<std::uint16_t>(Version::MAJOR),
        static_cast<std::uint16_t>(Version::MINOR),
        static_cast<std::uint16_t>(Version::PATCH), 0 });
    data.PluginName(Version::PROJECT);
    data.AuthorName("Flip-777");
    data.UsesSigScanning(false);
    data.UsesAddressLibrary(true);
    data.addressIndependence |= 1 << 2;
    data.HasNoStructUse(false);
    data.IsLayoutDependent(true);
    data.structureIndependence |= 1 << 2;
    return data;
}();

namespace
{
    bool RuntimeSupported() {
        static const bool ok = [] {
            if (!Versions::RuntimeSupported()) {
                logger::warn("unsupported runtime v{} - plugin idle", REL::Module::get().version().string());
                return false;
            }
            logger::info("generation: {}", Versions::GenName());
            return true;
        }();
        return ok;
    }

    void MessageCallback(F4SE::MessagingInterface::Message* msg) {
        switch (msg->type) {
        case F4SE::MessagingInterface::kGameDataReady:
            if (!msg->data || !RuntimeSupported()) {
                break;
            }
            Slots::Load();
            Slots::Save();
            Display::RegisterSinks();
            EngineInput::Install();
            if (Slots::g_enableOverlay) {
                Overlay::Install();
            }
            break;
        case F4SE::MessagingInterface::kPreLoadGame:
            if (RuntimeSupported()) {
                Display::OnPreLoadGame();
            }
            break;
        case F4SE::MessagingInterface::kPostLoadGame:
        case F4SE::MessagingInterface::kNewGame:
            if (RuntimeSupported()) {
                Display::OnPostLoadGame();
            }
            break;
        default:
            break;
        }
    }
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* f4se) {
    F4SE::Init(f4se, false);
    try {
        InitializeLog();
    } catch (...) {
    }
    logger::info("runtime v{}", REL::Module::get().version().string());

    if (!F4SE::GetMessagingInterface()->RegisterListener(MessageCallback)) {
        logger::critical("cannot register messaging listener");
        return false;
    }
    return true;
}
