#pragma once
#include <cstdint>
#include <vector>

// Encodes BGRA pixel data to JPEG.
// Downscales to max_width (preserving aspect ratio) before encoding when width exceeds it.
// Returns empty vector on failure.
std::vector<uint8_t> encode_bgra_to_jpeg(const uint8_t *bgra,
                                          uint32_t width, uint32_t height,
                                          uint32_t max_width = 960,
                                          int quality = 50);
