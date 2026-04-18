#include "tts-gemini.h"
#include "llm-utils.h"
#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <obs-module.h>
#include <cstring>

static const char *GEMINI_TTS_URL =
    "https://generativelanguage.googleapis.com/v1beta/models/"
    "gemini-3.1-flash-tts-preview:generateContent";

static struct curl_slist *make_gemini_headers(const std::string &api_key)
{
    struct curl_slist *h = nullptr;
    h = curl_slist_append(h, ("x-goog-api-key: " + api_key).c_str());
    h = curl_slist_append(h, "Content-Type: application/json");
    return h;
}

static void write_le16(std::vector<uint8_t> &buf, uint16_t v)
{
    buf.push_back(v & 0xFF);
    buf.push_back((v >> 8) & 0xFF);
}

static void write_le32(std::vector<uint8_t> &buf, uint32_t v)
{
    buf.push_back(v & 0xFF);
    buf.push_back((v >> 8) & 0xFF);
    buf.push_back((v >> 16) & 0xFF);
    buf.push_back((v >> 24) & 0xFF);
}

static std::vector<uint8_t> wrap_pcm_with_wav_header(const std::vector<uint8_t> &pcm)
{
    uint32_t data_size = static_cast<uint32_t>(pcm.size());
    uint32_t file_size = 36 + data_size;

    std::vector<uint8_t> wav;
    wav.reserve(44 + pcm.size());

    const char *riff = "RIFF";
    wav.insert(wav.end(), riff, riff + 4);
    write_le32(wav, file_size);
    const char *wave = "WAVE";
    wav.insert(wav.end(), wave, wave + 4);

    const char *fmt = "fmt ";
    wav.insert(wav.end(), fmt, fmt + 4);
    write_le32(wav, 16);             // chunk size
    write_le16(wav, 1);              // PCM
    write_le16(wav, 1);              // mono
    write_le32(wav, 24000);          // sample rate
    write_le32(wav, 24000 * 2);      // byte rate (s16le mono)
    write_le16(wav, 2);              // block align
    write_le16(wav, 16);             // bits per sample

    const char *data = "data";
    wav.insert(wav.end(), data, data + 4);
    write_le32(wav, data_size);
    wav.insert(wav.end(), pcm.begin(), pcm.end());

    return wav;
}

std::vector<uint8_t> synthesize_speech_gemini(const std::string &gemini_api_key,
                                               const std::string &text,
                                               const std::string &speaker,
                                               const std::string &instruct,
                                               const std::string &)
{
    if (gemini_api_key.empty() || text.empty()) return {};

    std::string prompt;
    if (!instruct.empty())
        prompt = "Say " + instruct + ": " + text;
    else
        prompt = text;

    nlohmann::json body = {
        {"contents", {{
            {"parts", {{{"text", prompt}}}}
        }}},
        {"generationConfig", {
            {"responseModalities", {"AUDIO"}},
            {"speechConfig", {
                {"voiceConfig", {
                    {"prebuiltVoiceConfig", {
                        {"voiceName", speaker.empty() ? std::string("Kore") : speaker}
                    }}
                }}
            }}
        }}
    };

    struct curl_slist *hdrs = make_gemini_headers(gemini_api_key);
    std::string resp_str = do_post(GEMINI_TTS_URL, hdrs, body.dump());
    curl_slist_free_all(hdrs);

    if (resp_str.empty()) {
        blog(LOG_ERROR, "[game-translator] Gemini TTS: empty response");
        return {};
    }

    std::string b64_audio;
    try {
        auto resp = nlohmann::json::parse(resp_str);
        if (resp.contains("error")) {
            blog(LOG_ERROR, "[game-translator] Gemini TTS error: %s",
                 resp["error"].dump().c_str());
            return {};
        }
        b64_audio = resp["candidates"][0]["content"]["parts"][0]["inlineData"]["data"]
                        .get<std::string>();
    } catch (const std::exception &e) {
        blog(LOG_ERROR, "[game-translator] Gemini TTS parse error: %s (raw: %s)",
             e.what(), resp_str.c_str());
        return {};
    }

    if (b64_audio.empty()) {
        blog(LOG_ERROR, "[game-translator] Gemini TTS: no audio data in response");
        return {};
    }

    auto pcm = base64_decode(b64_audio);
    blog(LOG_INFO, "[game-translator] Gemini TTS: decoded %zu PCM bytes", pcm.size());

    auto wav = wrap_pcm_with_wav_header(pcm);
    blog(LOG_INFO, "[game-translator] Gemini TTS: WAV %zu bytes", wav.size());
    return wav;
}
