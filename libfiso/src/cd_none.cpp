// Platforms without a CD adapter yet. SPDX-License-Identifier: Apache-2.0
#ifndef __APPLE__
#include "cd.h"

namespace fiso {
json platformDrives() {
    return {{"platform", "unsupported"}, {"drives", json::array()}};
}
std::unique_ptr<cd::Reader> platformOpenReader(const std::string &) {
    fail(FISO_E_UNSUPPORTED, "[platform] CD drive access is implemented for macOS only in FISO 1.0");
}
void platformEject(const std::string &) {
    fail(FISO_E_UNSUPPORTED, "[platform] not supported on this platform");
}
}  // namespace fiso
#endif
