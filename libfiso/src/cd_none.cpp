// Platforms without a CD drive adapter (CD adapters: cd_macos.cpp, cd_linux.cpp, cd_windows.cpp).
// SPDX-License-Identifier: Apache-2.0
#if !defined(__APPLE__) && !defined(__linux__) && !defined(_WIN32)
#include "cd.h"

namespace fiso {
json platformDrives() {
    return {{"platform", "unsupported"}, {"drives", json::array()}};
}
std::unique_ptr<cd::Reader> platformOpenReader(const std::string &) {
    fail(FISO_E_UNSUPPORTED, "[platform] CD drive access is not available on this operating system");
}
void platformEject(const std::string &) {
    fail(FISO_E_UNSUPPORTED, "[platform] not supported on this platform");
}
}  // namespace fiso
#endif
