#include "voice-analysis.h"
#include "llm-provider.h"
#include <nlohmann/json.hpp>
#include <obs-module.h>

static const char *SPEAKER_LIST =
    "Vivian (bright energetic young female), "
    "Serena (warm gentle female), "
    "Uncle_Fu (low mellow older male), "
    "Dylan (youthful natural male), "
    "Eric (lively slightly husky male), "
    "Ryan (dynamic heroic male), "
    "Aiden (sunny American male), "
    "Ono_Anna (playful Japanese female), "
    "Sohee (Korean female)";

static const char *GEMINI_SPEAKER_LIST =
    "Zephyr (bright), Puck (upbeat), Charon (informative), "
    "Kore (firm), Fenrir (excitable), Leda (youthful), "
    "Orus (firm), Aoede (breezy), Callirrhoe (easy-going), "
    "Autonoe (bright), Enceladus (breathy), Iapetus (clear), "
    "Umbriel (easy-going), Algieba (smooth), Despina (smooth), "
    "Erinome (clear), Algenib (gravelly), Rasalgethi (informative), "
    "Laomedeia (upbeat), Achernar (soft), Alnilam (firm), "
    "Schedar (even), Gacrux (mature), Pulcherrima (forward), "
    "Achird (friendly), Zubenelgenubi (casual), Vindemiatrix (gentle), "
    "Sadachbia (lively), Sadaltager (knowledgeable), Sulafat (warm)";

static std::string build_voice_system_prompt(const std::string &tts_provider)
{
    const char *speakers = (tts_provider == "gemini")
                               ? GEMINI_SPEAKER_LIST
                               : SPEAKER_LIST;

    return std::string(
        "You are a game screen analyzer. Your tasks:\n"
        "1. Identify story dialog or narration text. Ignore UI, HUD, menus, item/skill names.\n"
        "2. Extract the ORIGINAL text exactly as it appears on screen into \"original_text\".\n"
        "3. Detect the language of the original text. \"language\" must be exactly one of: "
        "Chinese, English, Japanese, Korean, German, French, Russian, Portuguese, Spanish, Italian.\n"
        "4. Determine the speaker character name. Use \"narrator\" if not identifiable.\n"
        "5. Select ONE voice speaker from this list that best fits the character's type:\n"
        "   " + std::string(speakers) + "\n"
        "6. Write a short instruct string for the delivery style (e.g. \"Speak in a deep, menacing tone\").\n\n"
        "Return ONLY a strict JSON object. No markdown, no extra text:\n"
        "{\"character\":\"\",\"original_text\":\"\",\"language\":\"Chinese\",\"speaker\":\"\",\"instruct\":\"\"}\n\n"
        "If no story dialog is detected, return:\n"
        "{\"character\":\"\",\"original_text\":\"\",\"language\":\"Japanese\",\"speaker\":\"Ryan\",\"instruct\":\"Speak clearly\"}");
}

VoiceAnalysis run_voice_analysis(const std::string &api_key,
                                  const std::string &llm_provider,
                                  const std::vector<uint8_t> &jpeg_bytes,
                                  const std::string &tts_provider)
{
    if (api_key.empty() || jpeg_bytes.empty()) return {};

    std::string sys = build_voice_system_prompt(tts_provider);
    std::string content = analyze_image_custom(
        jpeg_bytes, "image/jpeg", api_key, llm_provider, sys,
        "Analyze this game screenshot.");

    if (content.empty()) {
        blog(LOG_ERROR, "[game-translator] voice analysis: LLM returned empty");
        return {};
    }

    auto fence = content.find("```");
    if (fence != std::string::npos) {
        auto start = content.find('{', fence);
        auto end   = content.rfind('}');
        if (start != std::string::npos && end != std::string::npos)
            content = content.substr(start, end - start + 1);
    }

    VoiceAnalysis result;
    try {
        auto j = nlohmann::json::parse(content);
        result.character        = j.value("character",        "");
        result.original_text    = j.value("original_text",    "");
        result.detected_language = j.value("language",        "Japanese");
        const char *default_speaker = (tts_provider == "gemini") ? "Kore" : "Ryan";
        result.speaker          = j.value("speaker",          default_speaker);
        result.instruct         = j.value("instruct",         "Speak clearly");
    } catch (...) {
        blog(LOG_ERROR, "[game-translator] voice analysis parse error: %s",
             content.c_str());
        return {};
    }

    return result;
}
