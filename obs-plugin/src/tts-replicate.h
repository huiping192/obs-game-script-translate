#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Calls Replicate qwen/qwen3-tts, returns raw audio bytes (wav/mp3).
// Returns empty on error.
std::vector<uint8_t> synthesize_speech(const std::string &replicate_api_key,
                                        const std::string &text,
                                        const std::string &speaker,
                                        const std::string &instruct,
                                        const std::string &target_language);
