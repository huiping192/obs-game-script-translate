#include "translate-source.h"
#include "claude-api.h"
#include <obs-module.h>
#include <atomic>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

// Translation result is written to this file so the user can verify it.
static const char *OUTPUT_FILE = "/tmp/obs-translation.txt";

// ── Source private data ───────────────────────────────────────────────────

struct TranslateData {
    obs_source_t *source;

    std::string image_path;
    std::string api_key;

    std::mutex        result_mutex;
    std::string       translation;
    std::atomic<bool> translating{false};

    std::thread worker;
};

// ── Button callback ───────────────────────────────────────────────────────

static bool translate_clicked(obs_properties_t *props, obs_property_t *, void *)
{
    auto *data = static_cast<TranslateData *>(obs_properties_get_param(props));
    if (!data || data->translating.load())
        return false;

    std::string image_path, api_key;
    {
        std::lock_guard<std::mutex> lock(data->result_mutex);
        image_path = data->image_path;
        api_key    = data->api_key;
    }

    if (image_path.empty()) {
        blog(LOG_WARNING, "[game-translator] 截图路径未设置");
        return false;
    }

    blog(LOG_INFO, "[game-translator] 开始分析: %s", image_path.c_str());
    data->translating.store(true);

    if (data->worker.joinable())
        data->worker.join();

    data->worker = std::thread([data, image_path, api_key]() {
        std::string result = claude_analyze_image(image_path, api_key);

        // Write result to file
        std::ofstream f(OUTPUT_FILE);
        if (f.is_open()) {
            f << result << "\n";
            f.close();
        }

        // Also log to OBS log
        blog(LOG_INFO, "[game-translator] 翻译结果:\n%s", result.c_str());

        {
            std::lock_guard<std::mutex> lock(data->result_mutex);
            data->translation = result;
        }
        data->translating.store(false);
    });

    return false;
}

// ── OBS source callbacks ──────────────────────────────────────────────────

static const char *translate_get_name(void *)
{
    return "Game Translator";
}

static void *translate_create(obs_data_t *settings, obs_source_t *source)
{
    auto *data       = new TranslateData{};
    data->source     = source;
    data->image_path = obs_data_get_string(settings, "image_path");
    data->api_key    = obs_data_get_string(settings, "api_key");
    return data;
}

static void translate_destroy(void *priv)
{
    auto *data = static_cast<TranslateData *>(priv);
    if (data->worker.joinable())
        data->worker.join();
    delete data;
}

static void translate_update(void *priv, obs_data_t *settings)
{
    auto *data = static_cast<TranslateData *>(priv);
    std::lock_guard<std::mutex> lock(data->result_mutex);
    data->image_path = obs_data_get_string(settings, "image_path");
    data->api_key    = obs_data_get_string(settings, "api_key");
}

static obs_properties_t *translate_get_properties(void *priv)
{
    obs_properties_t *props = obs_properties_create();
    obs_properties_set_param(props, priv, nullptr);

    obs_properties_add_path(props, "image_path", "截图路径",
                            OBS_PATH_FILE,
                            "Images (*.png *.jpg *.jpeg *.webp)",
                            nullptr);

    obs_properties_add_text(props, "api_key",
                            "Anthropic API Key（留空则读取 ANTHROPIC_API_KEY 环境变量）",
                            OBS_TEXT_PASSWORD);

    obs_properties_add_button(props, "translate_btn",
                              "分析 & 翻译", translate_clicked);
    return props;
}

// ── Registration ──────────────────────────────────────────────────────────

static struct obs_source_info s_translate_info = {};

void register_translate_source()
{
    s_translate_info.id             = "game_translator_source";
    s_translate_info.type           = OBS_SOURCE_TYPE_INPUT;
    s_translate_info.output_flags   = OBS_SOURCE_DO_NOT_DUPLICATE;
    s_translate_info.get_name       = translate_get_name;
    s_translate_info.create         = translate_create;
    s_translate_info.destroy        = translate_destroy;
    s_translate_info.update         = translate_update;
    s_translate_info.get_properties = translate_get_properties;
    obs_register_source(&s_translate_info);
}
