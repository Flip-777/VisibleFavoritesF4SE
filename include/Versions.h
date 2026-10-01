#pragma once

//============= Runtime Generations =============
namespace Versions
{
    struct IDTable
    {
        std::uint64_t i3dmCtor;
        std::uint64_t begin3D;
        std::uint64_t unloadItem;
        std::uint64_t loadItem;
        std::uint64_t weaponBlood;
        std::uint64_t niClone;
    };

    const char* GenName();
    bool RuntimeSupported();
    const IDTable& Table();

    RE::BSTEventSource<RE::TESEquipEvent>* EquipEventSource();
    RE::BSTEventSource<RE::TESObjectLoadedEvent>* ObjectLoadedEventSource();
}
