// Resolve the running executable's own directory, so the contest tooling
// binaries (built directly into the repo root, alongside sakgd/approach1)
// can find data/, results/, and each other regardless of the caller's cwd.
#pragma once

#include <climits>
#include <string>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace paths {

inline std::string realpathOf(const std::string& p) {
    char buf[PATH_MAX];
    if (realpath(p.c_str(), buf)) return std::string(buf);
    return p;
}

inline std::string exePath() {
#if defined(__APPLE__)
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) return realpathOf(buf);
    return "";
#elif defined(__linux__)
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = '\0'; return std::string(buf); }
    return "";
#else
    return "";
#endif
}

inline std::string exeDir() {
    std::string p = exePath();
    if (p.empty()) return ".";
    auto pos = p.find_last_of('/');
    return pos == std::string::npos ? "." : p.substr(0, pos);
}

} // namespace paths
