#include "translate-source.h"
#include "llm-provider.h" // analyze_image_data
#include "image-encode.h"
#include <obs-module.h>
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
};

// Forward declarations
static void trigger_translate(TranslateData *data);
static void clear_translation(TranslateData *data);
static void translate_hotkey_pressed(void *priv, obs_hotkey_id, obs_hotkey_t *, bool pressed);
static void clear_hotkey_pressed(void *priv, obs_hotkey_id, obs_hotkey_t *, bool pressed);

// ── Worker thread ─────────────────────────────────────────────────────────

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

// ── Current scene: raw video callback ─────────────────────────────────────

static void on_raw_video(void *param, struct video_data *frame)
{
    auto *data = static_cast<TranslateData *>(param);

    if (!data->target_source_name.empty())
        return;
    if (data->translating.load())
        return;
    if (!data->manual_capture_requested.exchange(false))
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

    std::string api_key;
    {
        std::lock_guard<std::mutex> lock(data->result_mutex);
        api_key = data->api_key;
    }

    auto enc0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> jpeg = encode_bgra_to_jpeg(pixels.data(), cx, cy);
    auto enc1 = std::chrono::steady_clock::now();
    if (jpeg.empty())
        return;

    blog(LOG_INFO, "[game-translator] 捕获当前场景 %zu bytes，encode 耗时 %lld ms，开始翻译",
         jpeg.size(),
         std::chrono::duration_cast<std::chrono::milliseconds>(enc1 - enc0).count());
    start_translation_worker(data, std::move(jpeg), std::move(api_key), data->llm_provider, data->target_language);
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
}

static void *translate_create(obs_data_t *settings, obs_source_t *source)
{
    auto *data = new TranslateData{};
    data->source             = source;
    data->api_key            = obs_data_get_string(settings, "api_key");
    data->llm_provider       = obs_data_get_string(settings, "llm_provider");
    data->target_language    = obs_data_get_string(settings, "target_language");
    data->target_source_name = obs_data_get_string(settings, "target_source");
    blog(LOG_INFO, "[game-translator] 加载设置: llm_provider=%s target_language=%s",
         data->llm_provider.c_str(), data->target_language.c_str());
    data->bg_opacity         = (int)obs_data_get_int(settings, "overlay_bg_opacity");
    data->custom_width       = (uint32_t)obs_data_get_int(settings, "overlay_custom_width");
    data->auto_clear_seconds = (int)obs_data_get_int(settings, "auto_clear_seconds");

    struct video_scale_info vsi = {};
    vsi.format = VIDEO_FORMAT_BGRA;
    obs_add_raw_video_callback(&vsi, on_raw_video, data);

    data->hotkey_id = obs_hotkey_register_source(
        source, "game_translator_translate", obs_module_text("Hotkey.Translate"),
        translate_hotkey_pressed, data);

    // Default F9 if no hotkey saved yet
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

    data->clear_hotkey_id = obs_hotkey_register_source(
        source, "game_translator_clear", obs_module_text("Hotkey.Clear"),
        clear_hotkey_pressed, data);

    // Default F10 if no hotkey saved yet
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
    std::lock_guard<std::mutex> lock(data->result_mutex);
    data->api_key            = obs_data_get_string(settings, "api_key");
    data->llm_provider       = obs_data_get_string(settings, "llm_provider");
    data->target_language    = obs_data_get_string(settings, "target_language");
    data->target_source_name = obs_data_get_string(settings, "target_source");
    data->bg_opacity         = (int)obs_data_get_int(settings, "overlay_bg_opacity");
    data->custom_width       = (uint32_t)obs_data_get_int(settings, "overlay_custom_width");
    data->auto_clear_seconds = (int)obs_data_get_int(settings, "auto_clear_seconds");

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
        obs_data_set_int(ts, "custom_width", obs_data_get_int(settings, "overlay_custom_width"));
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

    // Apply new translation text on the main thread (safe for obs_source_update)
    if (dirty && data->text_source) {
        obs_data_t *ts = obs_data_create();
        obs_data_set_string(ts, "text", pending.c_str());
        obs_source_update(data->text_source, ts);
        obs_data_release(ts);
    }

    // Auto-clear after configured seconds
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

    // Height is content-driven; width is fixed to custom_width.
    // text_ft2_source_v2 computes its size lazily on the first render call,
    // so drive a render when th==0 to break the chicken-and-egg.
    uint32_t th = obs_source_get_height(data->text_source);

    if (th == 0) {
        obs_source_video_render(data->text_source);
        return;
    }

    uint32_t total_w = data->custom_width + 2 * (uint32_t)OVERLAY_PADDING;
    uint32_t total_h = th                 + 2 * (uint32_t)OVERLAY_PADDING;

    // Semi-transparent background rectangle (full fixed width)
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

    // Text left-aligned with padding
    gs_matrix_push();
    gs_matrix_translate3f((float)OVERLAY_PADDING, (float)OVERLAY_PADDING, 0.0f);
    obs_source_video_render(data->text_source);
    gs_matrix_pop();
}

// Width is fixed to custom_width so the source stays stable in the scene.
// Height adapts to text content (number of lines).
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

    return props;
}

// ── Manual trigger ────────────────────────────────────────────────────────

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

static void translate_hotkey_pressed(void *priv, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
    if (!pressed)
        return;
    trigger_translate(static_cast<TranslateData *>(priv));
}

static void clear_hotkey_pressed(void *priv, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
    if (!pressed)
        return;
    clear_translation(static_cast<TranslateData *>(priv));
}

// ── Registration ──────────────────────────────────────────────────────────

static struct obs_source_info s_translate_info = {};

void register_translate_source()
{
    s_translate_info.id             = "game_translator_source";
    s_translate_info.type           = OBS_SOURCE_TYPE_INPUT;
    s_translate_info.output_flags   = OBS_SOURCE_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE;
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
