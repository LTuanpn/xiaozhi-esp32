#pragma once

#include <cstdint>
#include <initializer_list>

namespace jarvis {
inline int SecondsLeft(int64_t deadline, int64_t now) {
    return deadline > now ? static_cast<int>((deadline - now + 999999) / 1000000) : 0;
}

// Equal deadlines are announced in the order supplied. Clear only after playback accepts.
inline int64_t* FirstDue(int64_t now, std::initializer_list<int64_t*> sources) {
    int64_t* first = nullptr;
    for (auto* deadline : sources) {
        if (*deadline && *deadline <= now && (!first || *deadline < *first))
            first = deadline;
    }
    return first;
}
}  // namespace jarvis
