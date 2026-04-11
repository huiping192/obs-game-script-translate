#pragma once
#include <cstdint>
#include <string>
#include <vector>

// provider: "claude" | "glm"
// api_key: must be set; returns error string if empty.
// media_type: "image/jpeg", "image/png", etc.
std::string analyze_image_data(const std::vector<uint8_t> &image_data,
                                const std::string &media_type,
                                const std::string &api_key,
                                const std::string &provider);
