#pragma once
#include <cstdint>
#include <string>
#include <vector>

enum class TtsStatus { Ok, NoText, Error };

// POST 截图到本地 TTS 服务（OCR + 合成都在服务端做），拿回 wav 字节。
TtsStatus synthesize_from_frame(const std::string &base_url,
                                 const std::vector<uint8_t> &jpeg,
                                 std::vector<uint8_t> &out_wav);
