#pragma once
#include <cstdint>
#include <string>
#include <vector>

std::vector<uint8_t> synthesize_speech_gemini(const std::string &gemini_api_key,
                                               const std::string &text,
                                               const std::string &speaker,
                                               const std::string &instruct,
                                               const std::string &target_language);
