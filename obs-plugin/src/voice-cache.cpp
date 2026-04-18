#include "voice-cache.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

std::string VoiceCache::normalize(const std::string &s)
{
    auto start = s.find_first_not_of(" \t\r\n\"'");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n\"'");
    std::string t = s.substr(start, end - start + 1);

    // Collapse internal whitespace runs to a single space
    std::string out;
    bool sp = false;
    for (char c : t) {
        if (c == ' ' || c == '\t') {
            if (!sp && !out.empty()) { out += ' '; sp = true; }
        } else {
            out += c;
            sp = false;
        }
    }
    return out;
}

void VoiceCache::load(const std::string &path)
{
    std::ifstream f(path);
    if (!f) return;
    try {
        auto j = nlohmann::json::parse(f);
        for (auto &[key, val] : j.items()) {
            map_[key] = {
                val.value("speaker", "Ryan"),
                val.value("instruct", "Speak clearly")
            };
        }
    } catch (...) {}
}

void VoiceCache::save(const std::string &path) const
{
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);

    nlohmann::json j = nlohmann::json::object();
    for (auto &[key, val] : map_)
        j[key] = {{"speaker", val.speaker}, {"instruct", val.instruct}};

    std::ofstream f(path);
    if (f) f << j.dump(2);
}

bool VoiceCache::get(const std::string &character, VoiceProfile &out) const
{
    auto it = map_.find(normalize(character));
    if (it == map_.end()) return false;
    out = it->second;
    return true;
}

void VoiceCache::set(const std::string &character, const VoiceProfile &profile)
{
    map_[normalize(character)] = profile;
}

void VoiceCache::clear()
{
    map_.clear();
}
