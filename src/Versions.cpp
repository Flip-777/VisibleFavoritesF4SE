#include "Versions.h"

//============= Runtime Generations =============
namespace Versions
{
    enum class Gen
    {
        OG,
        NG,
        AE,
        Unsupported
    };

    static Gen Generation() {
        static const Gen gen = [] {
            const auto ver = REL::Module::get().version();
            if (ver == REL::Version{ 1, 10, 163, 0 }) {
                return Gen::OG;
            }
            if (ver >= REL::Version{ 1, 11, 0, 0 }) {
                return Gen::AE;
            }
            if (ver >= REL::Version{ 1, 10, 980, 0 }) {
                return Gen::NG;
            }
            return Gen::Unsupported;
        }();
        return gen;
    }

    const char* GenName() {
        switch (Generation()) {
        case Gen::OG:
            return "OG 1.10.163";
        case Gen::NG:
            return "NG 1.10.980/984";
        case Gen::AE:
            return "1.11.x anniversary";
        default:
            return "unsupported";
        }
    }

    bool RuntimeSupported() {
        return Generation() != Gen::Unsupported;
    }

    const IDTable& Table() {
        static const IDTable t = [] {
            switch (Generation()) {
            case Gen::OG:
                return IDTable{ 544927, 662659, 524897, 75489, 914465, 604942 };
            default:
                return IDTable{ 2249082, 2249084, 2249089, 2249088, 2317581, 2269854 };
            }
        }();
        return t;
    }

    RE::BSTEventSource<RE::TESEquipEvent>* EquipEventSource() {
        if (Generation() == Gen::OG) {
            static REL::Relocation<RE::BSTEventSource<RE::TESEquipEvent>> src{ REL::ID(485633) };
            return reinterpret_cast<RE::BSTEventSource<RE::TESEquipEvent>*>(src.address());
        }
        using func_t = RE::BSTEventSource<RE::TESEquipEvent>* (*)();
        static REL::Relocation<func_t> func{ REL::ID(2201842) };
        return func();
    }

    RE::BSTEventSource<RE::TESObjectLoadedEvent>* ObjectLoadedEventSource() {
        if (Generation() == Gen::OG) {
            static REL::Relocation<RE::BSTEventSource<RE::TESObjectLoadedEvent>> src{ REL::ID(416662) };
            return reinterpret_cast<RE::BSTEventSource<RE::TESObjectLoadedEvent>*>(src.address());
        }
        using func_t = RE::BSTEventSource<RE::TESObjectLoadedEvent>* (*)();
        static REL::Relocation<func_t> func{ REL::ID(2201853) };
        return func();
    }
}
