#pragma once
#include <string>
#include <unordered_map>

struct VoiceProfile {
    std::string speaker;
    std::string instruct;
};

class VoiceCache {
public:
    void load(const std::string &path);
    void save(const std::string &path) const;
    bool get(const std::string &character, VoiceProfile &out) const;
    void set(const std::string &character, const VoiceProfile &profile);
    void clear();

    static std::string normalize(const std::string &character);

private:
    std::unordered_map<std::string, VoiceProfile> map_;
};
