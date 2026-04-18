#include "tts-replicate.h"
#include "llm-utils.h"
#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <obs-module.h>
#include <chrono>
#include <thread>

static const char *REPLICATE_TTS_URL =
    "https://api.replicate.com/v1/models/qwen/qwen3-tts/predictions";

static const char *tts_language_name(const std::string &target_language)
{
    if (target_language == "zh") return "Chinese";
    if (target_language == "ja") return "Japanese";
    return "English";
}

static struct curl_slist *make_replicate_headers(const std::string &api_key,
                                                  bool prefer_wait)
{
    struct curl_slist *h = nullptr;
    std::string auth = "Authorization: Bearer " + api_key;
    h = curl_slist_append(h, auth.c_str());
    h = curl_slist_append(h, "Content-Type: application/json");
    if (prefer_wait)
        h = curl_slist_append(h, "Prefer: wait");
    return h;
}

static std::string extract_audio_url(const nlohmann::json &resp)
{
    if (!resp.contains("output")) return {};
    const auto &output = resp["output"];
    if (output.is_string())
        return output.get<std::string>();
    if (output.is_array() && !output.empty() && output[0].is_string())
        return output[0].get<std::string>();
    return {};
}

std::vector<uint8_t> synthesize_speech(const std::string &replicate_api_key,
                                        const std::string &text,
                                        const std::string &speaker,
                                        const std::string &instruct,
                                        const std::string &target_language)
{
    if (replicate_api_key.empty() || text.empty()) return {};

    nlohmann::json body = {
        {"input", {
            {"text",              text},
            {"speaker",           speaker},
            {"language",          tts_language_name(target_language)},
            {"mode",              "custom_voice"},
            {"style_instruction", instruct}
        }}
    };

    struct curl_slist *hdrs = make_replicate_headers(replicate_api_key, true);
    std::string resp_str = do_post(REPLICATE_TTS_URL, hdrs, body.dump());
    curl_slist_free_all(hdrs);

    std::string audio_url;
    std::string poll_url;

    try {
        auto resp   = nlohmann::json::parse(resp_str);
        std::string status = resp.value("status", "");

        if (status == "succeeded") {
            audio_url = extract_audio_url(resp);
        } else if (status == "processing" || status == "starting") {
            poll_url = resp.at("urls").at("get").get<std::string>();
        } else {
            blog(LOG_ERROR, "[game-translator] TTS prediction %s: %s",
                 status.c_str(), resp_str.c_str());
            return {};
        }
    } catch (...) {
        blog(LOG_ERROR, "[game-translator] TTS response parse error: %s",
             resp_str.c_str());
        return {};
    }

    // Poll until succeeded (max 60 attempts × 1s)
    if (!poll_url.empty()) {
        struct curl_slist *poll_hdrs = make_replicate_headers(replicate_api_key, false);
        for (int i = 0; i < 60 && audio_url.empty(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::string s = do_get(poll_url.c_str(), poll_hdrs);
            try {
                auto resp   = nlohmann::json::parse(s);
                std::string st = resp.value("status", "");
                if (st == "succeeded") {
                    audio_url = extract_audio_url(resp);
                } else if (st == "failed" || st == "canceled") {
                    blog(LOG_ERROR, "[game-translator] TTS failed: %s", s.c_str());
                    break;
                }
            } catch (...) { break; }
        }
        curl_slist_free_all(poll_hdrs);
    }

    if (audio_url.empty()) {
        blog(LOG_ERROR, "[game-translator] TTS: no audio URL obtained");
        return {};
    }

    blog(LOG_INFO, "[game-translator] TTS audio URL: %s", audio_url.c_str());

    // Download the audio bytes
    struct curl_slist *dl_hdrs = make_replicate_headers(replicate_api_key, false);
    auto bytes = do_get_bytes(audio_url.c_str(), dl_hdrs);
    curl_slist_free_all(dl_hdrs);

    blog(LOG_INFO, "[game-translator] TTS downloaded %zu bytes", bytes.size());
    return bytes;
}
