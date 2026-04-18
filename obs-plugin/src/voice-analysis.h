#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct VoiceAnalysis {
    std::string character;
    std::string original_text;
    std::string detected_language;
    std::string translated_text;
    std::string speaker;
    std::string instruct;
    bool empty() const { return original_text.empty(); }
};

VoiceAnalysis run_voice_analysis(const std::string &api_key,
                                  const std::string &llm_provider,
                                  const std::vector<uint8_t> &jpeg_bytes,
                                  const std::string &target_language);
