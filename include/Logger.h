#pragma once

void InitializeLog();

#define VF_VLOG(...)                     \
    do {                                 \
        if (Slots::g_verboseLog) {       \
            logger::info(__VA_ARGS__);   \
        }                                \
    } while (0)
