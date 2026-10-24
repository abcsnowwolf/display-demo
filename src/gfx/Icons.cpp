#include "Icons.h"

#include <cstring>

namespace {

struct IconName {
    const char* name;
    uint32_t cp;
};

// Sorted by name (generated).
const IconName ICONS[] = {
#include "IconNames.inc"
};

}  // namespace

uint32_t iconCodepoint(const char* name) {
    if (!name) return 0;
    int lo = 0, hi = int(sizeof(ICONS) / sizeof(ICONS[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(name, ICONS[mid].name);
        if (c == 0) return ICONS[mid].cp;
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return 0;
}

std::string iconNameList() {
    std::string out;
    for (const auto& icon : ICONS) {
        if (!out.empty()) out += ',';
        out += icon.name;
    }
    return out;
}
