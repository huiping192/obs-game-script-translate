#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Sends raw image bytes (JPEG/PNG/etc.) to Claude Vision API.
// media_type: "image/jpeg", "image/png", etc.
// api_key: must be set; returns error string if empty.
std::string claude_analyze_image_data(const std::vector<uint8_t> &image_data,
                                      const std::string &media_type,
                                      const std::string &api_key);
