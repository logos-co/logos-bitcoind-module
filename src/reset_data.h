#pragma once

#include <cctype>
#include <filesystem>
#include <string>

// Where resetData() moves a network's chain: pure, so a test can drive it without a node.
namespace reset_data {

namespace fs = std::filesystem;

inline std::string trimmed(const fs::path& p) {
    std::string s = p.lexically_normal().string();
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}

/// The directory that holds `network`'s chain and nothing else: the dataDir when it is the
/// module's own for that network, otherwise Core's subdirectory for it. Empty for mainnet in a
/// dataDir set by hand, whose top level Core shares with the other networks' subdirectories.
inline fs::path chainDir(const fs::path& dataDir, const fs::path& ownDataDir, const std::string& network) {
    if (trimmed(dataDir) == trimmed(ownDataDir)) return fs::path(trimmed(dataDir));
    if (network == "mainnet") return {};
    return fs::path(trimmed(dataDir)) / network;
}

/// `<dir>.reset-<stamp>`, or with -2, -3... when that name is taken.
template <typename Exists>
fs::path asideFor(const fs::path& dir, const std::string& stamp, Exists exists) {
    const std::string base = trimmed(dir) + ".reset-" + stamp;
    fs::path p = base;
    for (int n = 2; exists(p); ++n) p = base + "-" + std::to_string(n);
    return p;
}

/// Init errors that mean the stored chain is unreadable, as opposed to a lock, a full disk or a
/// bad setting. The view offers a resync on these; kept here so both sides read the same list.
inline bool namesCorruption(std::string e) {
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* k : {"corrupt", "-reindex", "rebuild the", "error loading block database",
                          "error initializing block database"})
        if (e.find(k) != std::string::npos) return true;
    return false;
}

}  // namespace reset_data
