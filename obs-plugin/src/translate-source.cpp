#include "translate-source.h"
#include "llm-provider.h"
#include "image-encode.h"
#include "voice-cache.h"
#include "voice-analysis.h"
#include "tts-replicate.h"
#include "tts-gemini.h"
#include "audio-output.h"
#include <obs-module.h>
#include <util/bmem.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static const int OVERLAY_PADDING = 12;

// ── Source private data ───────────────────────────────────────────────────

struct TranslateData {
    obs_source_t *source;

    std::string api_key;
    std::string llm_provider;     // "claude" | "glm"
    std::string target_language;  // "zh" | "ja" | "en"
    std::mutex  result_mutex;
    std::string translation;
    std::atomic<bool> translating{false};
    std::thread worker;

    // empty = current scene (raw video callback), otherwise named source
    std::string target_source_name;
    std::atomic<bool> manual_capture_requested{false};

    // Capture graphics resources (specific source mode)
    gs_texrender_t *texrender    = nullptr;
    gs_stagesurf_t *stagesurface = nullptr;
    uint32_t        capture_cx   = 0;
    uint32_t        capture_cy   = 0;

    obs_hotkey_id hotkey_id       = OBS_INVALID_HOTKEY_ID;
    obs_hotkey_id clear_hotkey_id = OBS_INVALID_HOTKEY_ID;

    int auto_clear_seconds = 5;
    std::chrono::steady_clock::time_point translation_time;
    bool has_translation = false;

    // Text overlay rendering
    obs_source_t *text_source  = nullptr;
    std::string   pending_text;
    bool          text_dirty   = false;
    int           bg_opacity   = 80;    // 0–100 %
    uint32_t      custom_width = 800;   // fixed source width (pixels)

    // ── Voice (F8) ───────────────────────────────────────────────────────
    std::string tts_provider;      // "replicate" | "gemini"
    std::string replicate_api_key;
    std::string gemini_api_key;
    VoiceCache  voice_cache;
    std::string voice_cache_path;
    std::thread voice_worker;
    std::atomic<bool> voice_running{false};
    std::atomic<bool> voice_capture_requested{false};
    obs_hotkey_id voice_hotkey_id = OBS_INVALID_HOTKEY_ID;
};

// Forward declarations
static void trigger_translate(TranslateData *data);
static void trigger_voice(TranslateData *data);
static void clear_translation(TranslateData *data);
static void translate_hotkey_pressed(void *priv, obs_hotkey_id, obs_hotkey_t *, bool pressed);
static void clear_hotkey_pressed(void *priv, obs_hotkey_id, obs_hotkey_t *, bool pressed);
static void voice_hotkey_pressed(void *priv, obs_hotkey_id, obs_hotkey_t *, bool pressed);

// ── Worker thread (translate) ─────────────────────────────────────────────

static void start_translation_worker(TranslateData *data,
                                     std::vector<uint8_t> jpeg,
                                     std::string api_key,
                                     std::string llm_provider,
                                     std::string target_language)
{
    data->translating.store(true);
    if (data->worker.joinable())
        data->worker.join();

    obs_source_t *source = data->source;

    data->worker = std::thread([data, source,
                                jpeg            = std::move(jpeg),
                                api_key         = std::move(api_key),
                                llm_provider    = std::move(llm_provider),
                                target_language = std::move(target_language)]() mutable {
        auto t0 = std::chrono::steady_clock::now();
        std::string result = analyze_image_data(jpeg, "image/jpeg", api_key, llm_provider, target_language);
        auto t1 = std::chrono::steady_clock::now();
        long ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        blog(LOG_INFO, "[game-translator] API 调用耗时 %ld ms（含 base64 编码）", ms);
        blog(LOG_INFO, "[game-translator] 翻译结果:\n%s", result.c_str());

        {
            std::lock_guard<std::mutex> lock(data->result_mutex);
            data->translation      = result;
            data->pending_text     = result;
            data->text_dirty       = true;
            data->has_translation  = true;
            data->translation_time = std::chrono::steady_clock::now();
        }

        obs_data_t *settings = obs_source_get_settings(source);
        obs_data_set_string(settings, "translation_result", result.c_str());
        obs_data_release(settings);

        obs_source_update_properties(source);

        data->translating.store(false);
    });
}

// ── Worker thread (voice) ─────────────────────────────────────────────────

static void start_voice_worker(TranslateData *data,
                                std::vector<uint8_t> jpeg,
                                std::string api_key,
                                std::string llm_provider,
                                std::string replicate_api_key,
                                std::string gemini_api_key,
                                std::string tts_provider)
{
    data->voice_running.store(true);
    if (data->voice_worker.joinable())
        data->voice_worker.join();

    obs_source_t *source = data->source;

    data->voice_worker = std::thread([data, source,
                                      jpeg              = std::move(jpeg),
                                      api_key           = std::move(api_key),
                                      llm_provider      = std::move(llm_provider),
                                      replicate_api_key = std::move(replicate_api_key),
                                      gemini_api_key    = std::move(gemini_api_key),
                                      tts_provider      = std::move(tts_provider)]() mutable {
        blog(LOG_INFO, "[game-translator] voice pipeline start (tts=%s)", tts_provider.c_str());

        VoiceAnalysis analysis = run_voice_analysis(api_key, llm_provider, jpeg, tts_provider);
        jpeg.clear();

        if (analysis.original_text.empty()) {
            blog(LOG_INFO, "[game-translator] voice: no text detected");
            data->voice_running.store(false);
            return;
        }

        blog(LOG_INFO, "[game-translator] voice: character=%s speaker=%s lang=%s",
             analysis.character.c_str(), analysis.speaker.c_str(), analysis.detected_language.c_str());

        VoiceProfile profile = {analysis.speaker, analysis.instruct};
        {
            std::lock_guard<std::mutex> lock(data->result_mutex);
            VoiceProfile cached;
            if (data->voice_cache.get(analysis.character, cached, tts_provider)) {
                profile = cached;
            } else {
                data->voice_cache.set(analysis.character, profile, tts_provider);
                data->voice_cache.save(data->voice_cache_path);
            }
        }

        std::vector<uint8_t> audio;
        if (tts_provider == "gemini") {
            audio = synthesize_speech_gemini(gemini_api_key,
                                             analysis.original_text,
                                             profile.speaker,
                                             profile.instruct,
                                             analysis.detected_language);
        } else {
            audio = synthesize_speech(replicate_api_key,
                                      analysis.original_text,
                                      profile.speaker,
                                      profile.instruct,
                                      analysis.detected_language);
        }
        if (audio.empty()) {
            blog(LOG_ERROR, "[game-translator] voice: TTS returned empty audio");
            data->voice_running.store(false);
            return;
        }

        push_audio_to_obs_source(source, audio);

        data->voice_running.store(false);
    });
}

// ── Current scene: raw video callback ─────────────────────────────────────

static void on_raw_video(void *param, struct video_data *frame)
{
    auto *data = static_cast<TranslateData *>(param);

    if (!data->target_source_name.empty())
        return;

    bool want_translate = !data->translating.load()   && data->manual_capture_requested.load();
    bool want_voice     = !data->voice_running.load() && data->voice_capture_requested.load();

    if (!want_translate && !want_voice)
        return;

    struct obs_video_info ovi;
    if (!obs_get_video_info(&ovi))
        return;
    uint32_t cx = ovi.output_width;
    uint32_t cy = ovi.output_height;
    if (cx == 0 || cy == 0 || !frame->data[0])
        return;

    std::vector<uint8_t> pixels((size_t)cy * cx * 4);
    for (uint32_t y = 0; y < cy; y++) {
        memcpy(pixels.data() + (size_t)y * cx * 4,
               frame->data[0] + (size_t)y * frame->linesize[0],
               (size_t)cx * 4);
    }

    auto enc0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> jpeg = encode_bgra_to_jpeg(pixels.data(), cx, cy);
    auto enc1 = std::chrono::steady_clock::now();
    if (jpeg.empty())
        return;

    blog(LOG_INFO, "[game-translator] 捕获当前场景 %zu bytes，encode 耗时 %lld ms",
         jpeg.size(),
         std::chrono::duration_cast<std::chrono::milliseconds>(enc1 - enc0).count());

    std::string api_key, replicate_api_key, gemini_api_key, llm_provider, tts_provider, target_language;
    {
        std::lock_guard<std::mutex> lock(data->result_mutex);
        api_key           = data->api_key;
        replicate_api_key = data->replicate_api_key;
        gemini_api_key    = data->gemini_api_key;
        llm_provider      = data->llm_provider;
        tts_provider      = data->tts_provider;
        target_language   = data->target_language;
    }

    if (want_translate && want_voice) {
        data->manual_capture_requested.store(false);
        data->voice_capture_requested.store(false);
        auto jpeg_for_voice = jpeg;
        start_translation_worker(data, std::move(jpeg), api_key, llm_provider, target_language);
        start_voice_worker(data, std::move(jpeg_for_voice), api_key, llm_provider, replicate_api_key, gemini_api_key, tts_provider);
    } else if (want_translate) {
        data->manual_capture_requested.store(false);
        start_translation_worker(data, std::move(jpeg), api_key, llm_provider, target_language);
    } else {
        data->voice_capture_requested.store(false);
        start_voice_worker(data, std::move(jpeg), api_key, llm_provider, replicate_api_key, gemini_api_key, tts_provider);
    }
}

// ── Specific source: texrender frame capture ───────────────────────────────

static std::vector<uint8_t> capture_source_frame(TranslateData *data)
{
    obs_source_t *target = obs_get_source_by_name(data->target_source_name.c_str());
    if (!target)
        return {};

    uint32_t cx = obs_source_get_width(target);
    uint32_t cy = obs_source_get_height(target);
    if (cx == 0 || cy == 0) {
        obs_source_release(target);
        return {};
    }

    obs_enter_graphics();

    if (!data->texrender)
        data->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);

    if (!data->stagesurface || data->capture_cx != cx || data->capture_cy != cy) {
        gs_stagesurface_destroy(data->stagesurface);
        data->stagesurface = gs_stagesurface_create(cx, cy, GS_BGRA);
        data->capture_cx   = cx;
        data->capture_cy   = cy;
    }

    gs_texrender_reset(data->texrender);
    bool rendered = false;
    if (gs_texrender_begin(data->texrender, cx, cy)) {
        struct vec4 clear_color;
        vec4_zero(&clear_color);
        gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
        gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
        obs_source_video_render(target);
        gs_texrender_end(data->texrender);
        rendered = true;
    }

    obs_source_release(target);

    if (!rendered) {
        obs_leave_graphics();
        return {};
    }

    gs_texture_t *tex = gs_texrender_get_texture(data->texrender);
    if (!tex) {
        obs_leave_graphics();
        return {};
    }
    gs_stage_texture(data->stagesurface, tex);

    uint8_t *pixel_data = nullptr;
    uint32_t linesize   = 0;
    std::vector<uint8_t> pixels;

    if (gs_stagesurface_map(data->stagesurface, &pixel_data, &linesize)) {
        pixels.resize((size_t)cy * cx * 4);
        for (uint32_t y = 0; y < cy; y++) {
            memcpy(pixels.data() + (size_t)y * cx * 4,
                   pixel_data + (size_t)y * linesize,
                   (size_t)cx * 4);
        }
        gs_stagesurface_unmap(data->stagesurface);
    }

    obs_leave_graphics();

    if (pixels.empty())
        return {};

    auto enc0 = std::chrono::steady_clock::now();
    auto jpeg = encode_bgra_to_jpeg(pixels.data(), cx, cy);
    auto enc1 = std::chrono::steady_clock::now();
    blog(LOG_INFO, "[game-translator] source 帧 encode 耗时 %lld ms，%zu bytes",
         std::chrono::duration_cast<std::chrono::milliseconds>(enc1 - enc0).count(),
         jpeg.size());
    return jpeg;
}

// ── OBS source callbacks ──────────────────────────────────────────────────

static const char *translate_get_name(void *)
{
    return obs_module_text("SourceName");
}

static void translate_get_defaults(obs_data_t *settings)
{
    obs_data_t *font_obj = obs_data_create();
#ifdef __APPLE__
    obs_data_set_string(font_obj, "face", "Hiragino Sans");
#elif defined(_WIN32)
    obs_data_set_string(font_obj, "face", "Microsoft YaHei");
#else
    obs_data_set_string(font_obj, "face", "Sans");
#endif
    obs_data_set_int(font_obj, "size", 36);
    obs_data_set_obj(settings, "overlay_font", font_obj);
    obs_data_release(font_obj);

    obs_data_set_default_string(settings, "llm_provider",         "claude");
    obs_data_set_default_string(settings, "target_language",      "zh");
    obs_data_set_default_int(settings,    "overlay_color",        0xFFFFFFFF);
    obs_data_set_default_int(settings,    "overlay_bg_opacity",   80);
    obs_data_set_default_int(settings,    "overlay_custom_width", 800);
    obs_data_set_default_int(settings,    "auto_clear_seconds",   5);
    obs_data_set_default_string(settings, "replicate_api_key",    "");
    obs_data_set_default_string(settings, "tts_provider",         "replicate");
    obs_data_set_default_string(settings, "gemini_api_key",       "");
}

static void *translate_create(obs_data_t *settings, obs_source_t *source)
{
    auto *data = new TranslateData{};
    data->source             = source;
    data->api_key            = obs_data_get_string(settings, "api_key");
    data->llm_provider       = obs_data_get_string(settings, "llm_provider");
    data->target_language    = obs_data_get_string(settings, "target_language");
    data->target_source_name = obs_data_get_string(settings, "target_source");
    data->replicate_api_key  = obs_data_get_string(settings, "replicate_api_key");
    blog(LOG_INFO, "[game-translator] 加载设置: llm_provider=%s target_language=%s",
         data->llm_provider.c_str(), data->target_language.c_str());
    data->bg_opacity         = (int)obs_data_get_int(settings, "overlay_bg_opacity");
    data->custom_width       = (uint32_t)obs_data_get_int(settings, "overlay_custom_width");
    data->auto_clear_seconds = (int)obs_data_get_int(settings, "auto_clear_seconds");

    // Load voice cache from plugin config dir
    char *cfg = obs_module_config_path("voice_cache.json");
    if (cfg) {
        data->voice_cache_path = cfg;
        bfree(cfg);
    }
    data->voice_cache.load(data->voice_cache_path);

    struct video_scale_info vsi = {};
    vsi.format = VIDEO_FORMAT_BGRA;
    obs_add_raw_video_callback(&vsi, on_raw_video, data);

    // F9: translate hotkey
    data->hotkey_id = obs_hotkey_register_source(
        source, "game_translator_translate", obs_module_text("Hotkey.Translate"),
        translate_hotkey_pressed, data);

    obs_data_array_t *saved = obs_hotkey_save(data->hotkey_id);
    if (obs_data_array_count(saved) == 0) {
        obs_data_array_t *defaults = obs_data_array_create();
        obs_data_t *item = obs_data_create();
        obs_data_set_string(item, "key", "OBS_KEY_F9");
        obs_data_array_push_back(defaults, item);
        obs_data_release(item);
        obs_hotkey_load(data->hotkey_id, defaults);
        obs_data_array_release(defaults);
    }
    obs_data_array_release(saved);

    // F10: clear hotkey
    data->clear_hotkey_id = obs_hotkey_register_source(
        source, "game_translator_clear", obs_module_text("Hotkey.Clear"),
        clear_hotkey_pressed, data);

    obs_data_array_t *saved_clear = obs_hotkey_save(data->clear_hotkey_id);
    if (obs_data_array_count(saved_clear) == 0) {
        obs_data_array_t *defaults_clear = obs_data_array_create();
        obs_data_t *item_clear = obs_data_create();
        obs_data_set_string(item_clear, "key", "OBS_KEY_F10");
        obs_data_array_push_back(defaults_clear, item_clear);
        obs_data_release(item_clear);
        obs_hotkey_load(data->clear_hotkey_id, defaults_clear);
        obs_data_array_release(defaults_clear);
    }
    obs_data_array_release(saved_clear);

    // F8: voice hotkey
    data->voice_hotkey_id = obs_hotkey_register_source(
        source, "game_translator_voice", obs_module_text("Hotkey.Voice"),
        voice_hotkey_pressed, data);

    obs_data_array_t *saved_voice = obs_hotkey_save(data->voice_hotkey_id);
    if (obs_data_array_count(saved_voice) == 0) {
        obs_data_array_t *dv = obs_data_array_create();
        obs_data_t *iv = obs_data_create();
        obs_data_set_string(iv, "key", "OBS_KEY_F8");
        obs_data_array_push_back(dv, iv);
        obs_data_release(iv);
        obs_hotkey_load(data->voice_hotkey_id, dv);
        obs_data_array_release(dv);
    }
    obs_data_array_release(saved_voice);

    // Create private text source for on-screen rendering
    obs_data_t *ts = obs_data_create();
    obs_data_set_string(ts, "text", "");

    obs_data_t *font_obj = obs_data_get_obj(settings, "overlay_font");
    if (font_obj) {
        obs_data_set_obj(ts, "font", font_obj);
        obs_data_release(font_obj);
    }
    long long color = obs_data_get_int(settings, "overlay_color");
    obs_data_set_int(ts, "color1", color);
    obs_data_set_int(ts, "color2", color);
    obs_data_set_bool(ts, "word_wrap", true);
    obs_data_set_int(ts, "custom_width", obs_data_get_int(settings, "overlay_custom_width"));

    data->text_source = obs_source_create_private("text_ft2_source_v2", nullptr, ts);
    if (!data->text_source)
        blog(LOG_WARNING, "[game-translator] text_ft2_source_v2 不可用，翻译将只显示在属性面板");
    obs_data_release(ts);

    return data;
}

static void translate_destroy(void *priv)
{
    auto *data = static_cast<TranslateData *>(priv);

    obs_remove_raw_video_callback(on_raw_video, data);

    if (data->worker.joinable())
        data->worker.join();
    if (data->voice_worker.joinable())
        data->voice_worker.join();

    // Persist voice cache on exit
    if (!data->voice_cache_path.empty())
        data->voice_cache.save(data->voice_cache_path);

    obs_source_release(data->text_source);

    obs_enter_graphics();
    gs_texrender_destroy(data->texrender);
    gs_stagesurface_destroy(data->stagesurface);
    obs_leave_graphics();

    delete data;
}

static void translate_update(void *priv, obs_data_t *settings)
{
    auto *data = static_cast<TranslateData *>(priv);

    uint32_t custom_width;
    {
        std::lock_guard<std::mutex> lock(data->result_mutex);
        data->api_key            = obs_data_get_string(settings, "api_key");
        data->llm_provider       = obs_data_get_string(settings, "llm_provider");
        data->target_language    = obs_data_get_string(settings, "target_language");
        data->target_source_name = obs_data_get_string(settings, "target_source");
        data->replicate_api_key  = obs_data_get_string(settings, "replicate_api_key");
        data->tts_provider       = obs_data_get_string(settings, "tts_provider");
        data->gemini_api_key     = obs_data_get_string(settings, "gemini_api_key");
        data->bg_opacity         = (int)obs_data_get_int(settings, "overlay_bg_opacity");
        data->custom_width       = (uint32_t)obs_data_get_int(settings, "overlay_custom_width");
        data->auto_clear_seconds = (int)obs_data_get_int(settings, "auto_clear_seconds");
        custom_width             = data->custom_width;
    }

    if (data->text_source) {
        obs_data_t *ts = obs_data_create();
        obs_data_t *font_obj = obs_data_get_obj(settings, "overlay_font");
        if (font_obj) {
            obs_data_set_obj(ts, "font", font_obj);
            obs_data_release(font_obj);
        }
        long long color = obs_data_get_int(settings, "overlay_color");
        obs_data_set_int(ts, "color1", color);
        obs_data_set_int(ts, "color2", color);
        obs_data_set_bool(ts, "word_wrap", true);
        obs_data_set_int(ts, "custom_width", custom_width);
        obs_source_update(data->text_source, ts);
        obs_data_release(ts);
    }
}

// ── Clear translation ─────────────────────────────────────────────────────

static void clear_translation(TranslateData *data)
{
    {
        std::lock_guard<std::mutex> lock(data->result_mutex);
        data->translation    = "";
        data->pending_text   = "";
        data->text_dirty     = true;
        data->has_translation = false;
    }
    obs_data_t *settings = obs_source_get_settings(data->source);
    obs_data_set_string(settings, "translation_result", "");
    obs_data_release(settings);
    obs_source_update_properties(data->source);
}

// ── Video rendering ───────────────────────────────────────────────────────

static void translate_video_tick(void *priv, float)
{
    auto *data = static_cast<TranslateData *>(priv);

    std::string pending;
    bool dirty = false;
    {
        std::lock_guard<std::mutex> lock(data->result_mutex);
        if (data->text_dirty) {
            pending          = data->pending_text;
            data->text_dirty = false;
            dirty            = true;
        }
    }

    if (dirty && data->text_source) {
        obs_data_t *ts = obs_data_create();
        obs_data_set_string(ts, "text", pending.c_str());
        obs_source_update(data->text_source, ts);
        obs_data_release(ts);
    }

    int auto_secs;
    bool has_trans;
    std::chrono::steady_clock::time_point trans_time;
    {
        std::lock_guard<std::mutex> lock(data->result_mutex);
        auto_secs  = data->auto_clear_seconds;
        has_trans  = data->has_translation;
        trans_time = data->translation_time;
    }
    if (has_trans && auto_secs > 0) {
        auto elapsed = std::chrono::steady_clock::now() - trans_time;
        if (elapsed >= std::chrono::seconds(auto_secs))
            clear_translation(data);
    }
}

static void translate_video_render(void *priv, gs_effect_t *)
{
    auto *data = static_cast<TranslateData *>(priv);
    if (!data->text_source)
        return;

    uint32_t th = obs_source_get_height(data->text_source);

    if (th == 0) {
        obs_source_video_render(data->text_source);
        return;
    }

    uint32_t total_w = data->custom_width + 2 * (uint32_t)OVERLAY_PADDING;
    uint32_t total_h = th                 + 2 * (uint32_t)OVERLAY_PADDING;

    gs_effect_t *solid       = obs_get_base_effect(OBS_EFFECT_SOLID);
    gs_eparam_t *color_param = gs_effect_get_param_by_name(solid, "color");

    struct vec4 bg;
    vec4_set(&bg, 0.0f, 0.0f, 0.0f, (float)data->bg_opacity / 100.0f);
    gs_effect_set_vec4(color_param, &bg);

    gs_blend_state_push();
    gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
    while (gs_effect_loop(solid, "Solid")) {
        gs_draw_sprite(nullptr, 0, total_w, total_h);
    }
    gs_blend_state_pop();

    gs_matrix_push();
    gs_matrix_translate3f((float)OVERLAY_PADDING, (float)OVERLAY_PADDING, 0.0f);
    obs_source_video_render(data->text_source);
    gs_matrix_pop();
}

static uint32_t translate_get_width(void *priv)
{
    auto *data = static_cast<TranslateData *>(priv);
    if (!data->text_source)
        return 0;
    uint32_t th = obs_source_get_height(data->text_source);
    return th > 0 ? data->custom_width + 2 * (uint32_t)OVERLAY_PADDING : 0;
}

static uint32_t translate_get_height(void *priv)
{
    auto *data = static_cast<TranslateData *>(priv);
    if (!data->text_source)
        return 0;
    uint32_t th = obs_source_get_height(data->text_source);
    return th > 0 ? th + 2 * (uint32_t)OVERLAY_PADDING : 0;
}

// ── Properties panel ──────────────────────────────────────────────────────

static bool on_clear_voice_cache_clicked(obs_properties_t *, obs_property_t *, void *priv)
{
    auto *data = static_cast<TranslateData *>(priv);
    {
        std::lock_guard<std::mutex> lock(data->result_mutex);
        data->voice_cache.clear();
        data->voice_cache.save(data->voice_cache_path);
    }
    blog(LOG_INFO, "[game-translator] voice cache cleared");
    return false;
}

static bool on_tts_provider_modified(obs_properties_t *props,
                                      obs_property_t *,
                                      obs_data_t *settings)
{
    const char *tts = obs_data_get_string(settings, "tts_provider");
    bool is_gemini = (strcmp(tts, "gemini") == 0);

    obs_property_t *repl = obs_properties_get(props, "replicate_api_key");
    obs_property_t *gem  = obs_properties_get(props, "gemini_api_key");
    if (repl) obs_property_set_visible(repl, !is_gemini);
    if (gem)  obs_property_set_visible(gem,  is_gemini);

    return true;
}

static obs_properties_t *translate_get_properties(void *priv)
{
    obs_properties_t *props = obs_properties_create();
    obs_properties_set_param(props, priv, nullptr);

    // Capture source
    obs_property_t *src_list = obs_properties_add_list(
        props, "target_source", obs_module_text("CaptureSource"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(src_list, obs_module_text("CaptureSource.CurrentScene"), "");
    obs_enum_sources([](void *param, obs_source_t *src) -> bool {
        if (obs_source_get_output_flags(src) & OBS_SOURCE_VIDEO) {
            auto *list = static_cast<obs_property_t *>(param);
            const char *name = obs_source_get_name(src);
            obs_property_list_add_string(list, name, name);
        }
        return true;
    }, src_list);

    obs_property_t *llm_list = obs_properties_add_list(
        props, "llm_provider", obs_module_text("LLMProvider"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(llm_list, obs_module_text("LLMProvider.Claude"), "claude");
    obs_property_list_add_string(llm_list, obs_module_text("LLMProvider.GLM"), "glm");

    obs_property_t *lang_list = obs_properties_add_list(
        props, "target_language", obs_module_text("TargetLanguage"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(lang_list, obs_module_text("TargetLang.Chinese"), "zh");
    obs_property_list_add_string(lang_list, obs_module_text("TargetLang.Japanese"), "ja");
    obs_property_list_add_string(lang_list, obs_module_text("TargetLang.English"), "en");

    obs_properties_add_text(props, "api_key",
                            obs_module_text("APIKey"),
                            OBS_TEXT_PASSWORD);
    obs_properties_add_text(props, "hotkey_hint",
                            obs_module_text("HotkeyHint"),
                            OBS_TEXT_INFO);

    // Text overlay appearance
    obs_properties_add_font(props, "overlay_font", obs_module_text("OverlayFont"));
    obs_properties_add_color(props, "overlay_color", obs_module_text("OverlayColor"));
    obs_properties_add_int_slider(props, "auto_clear_seconds",
                                  obs_module_text("AutoClearSeconds"), 0, 30, 1);
    obs_properties_add_int_slider(props, "overlay_bg_opacity",
                                  obs_module_text("BGOpacity"), 0, 100, 5);
    obs_properties_add_int(props, "overlay_custom_width",
                           obs_module_text("CustomWidth"), 200, 3840, 10);

    obs_properties_add_text(props, "translation_result", obs_module_text("TranslationResult"),
                            OBS_TEXT_MULTILINE);

    // Voice (F8) settings
    obs_property_t *tts_list = obs_properties_add_list(
        props, "tts_provider", obs_module_text("TTSProvider"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(tts_list, obs_module_text("TTSProvider.Replicate"), "replicate");
    obs_property_list_add_string(tts_list, obs_module_text("TTSProvider.Gemini"),    "gemini");
    obs_property_set_modified_callback(tts_list, on_tts_provider_modified);

    obs_properties_add_text(props, "replicate_api_key",
                            obs_module_text("ReplicateAPIKey"),
                            OBS_TEXT_PASSWORD);
    obs_properties_add_text(props, "gemini_api_key",
                            obs_module_text("GeminiAPIKey"),
                            OBS_TEXT_PASSWORD);
    obs_properties_add_button(props, "clear_voice_cache",
                              obs_module_text("ClearVoiceCache"),
                              on_clear_voice_cache_clicked);

    return props;
}

// ── Manual triggers ───────────────────────────────────────────────────────

static void trigger_translate(TranslateData *data)
{
    if (!data || data->translating.load())
        return;

    if (data->target_source_name.empty()) {
        data->manual_capture_requested.store(true);
    } else {
        std::string api_key;
        {
            std::lock_guard<std::mutex> lock(data->result_mutex);
            api_key = data->api_key;
        }
        std::vector<uint8_t> jpeg = capture_source_frame(data);
        if (jpeg.empty()) {
            blog(LOG_WARNING, "[game-translator] 帧捕获失败，请确认捕获源正在运行");
            return;
        }
        blog(LOG_INFO, "[game-translator] 手动捕获帧 %zu bytes，开始翻译", jpeg.size());
        start_translation_worker(data, std::move(jpeg), std::move(api_key), data->llm_provider, data->target_language);
    }
}

static void trigger_voice(TranslateData *data)
{
    if (!data || data->voice_running.load())
        return;

    std::string api_key, llm_provider, replicate_api_key, gemini_api_key, tts_provider;
    {
        std::lock_guard<std::mutex> lock(data->result_mutex);
        api_key           = data->api_key;
        llm_provider      = data->llm_provider;
        replicate_api_key = data->replicate_api_key;
        gemini_api_key    = data->gemini_api_key;
        tts_provider      = data->tts_provider;
    }

    bool is_gemini = (tts_provider == "gemini");
    const std::string &tts_key = is_gemini ? gemini_api_key : replicate_api_key;

    if (api_key.empty() || tts_key.empty()) {
        blog(LOG_WARNING, "[game-translator] voice: API Key 或 TTS API Key 未配置");
        return;
    }

    if (data->target_source_name.empty()) {
        data->voice_capture_requested.store(true);
    } else {
        std::vector<uint8_t> jpeg = capture_source_frame(data);
        if (jpeg.empty()) {
            blog(LOG_WARNING, "[game-translator] voice: 帧捕获失败");
            return;
        }
        start_voice_worker(data, std::move(jpeg), api_key, llm_provider, replicate_api_key, gemini_api_key, tts_provider);
    }
}

static void translate_hotkey_pressed(void *priv, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
    if (!pressed) return;
    trigger_translate(static_cast<TranslateData *>(priv));
}

static void clear_hotkey_pressed(void *priv, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
    if (!pressed) return;
    clear_translation(static_cast<TranslateData *>(priv));
}

static void voice_hotkey_pressed(void *priv, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
    if (!pressed) return;
    trigger_voice(static_cast<TranslateData *>(priv));
}

// ── Registration ──────────────────────────────────────────────────────────

static struct obs_source_info s_translate_info = {};

void register_translate_source()
{
    s_translate_info.id             = "game_translator_source";
    s_translate_info.type           = OBS_SOURCE_TYPE_INPUT;
    s_translate_info.output_flags   = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
    s_translate_info.get_name       = translate_get_name;
    s_translate_info.get_defaults   = translate_get_defaults;
    s_translate_info.create         = translate_create;
    s_translate_info.destroy        = translate_destroy;
    s_translate_info.update         = translate_update;
    s_translate_info.video_tick     = translate_video_tick;
    s_translate_info.video_render   = translate_video_render;
    s_translate_info.get_width      = translate_get_width;
    s_translate_info.get_height     = translate_get_height;
    s_translate_info.get_properties = translate_get_properties;
    obs_register_source(&s_translate_info);
}
