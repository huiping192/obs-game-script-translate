#pragma once
#include <string>

// Reads image_path, base64-encodes it, sends to Claude Vision API,
// and returns the Chinese translation text.
// api_key may be empty, in which case ANTHROPIC_API_KEY env var is used.
std::string claude_analyze_image(const std::string &image_path,
                                 const std::string &api_key);
