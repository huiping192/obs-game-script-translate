#include "translate-source.h"
#include "claude-api.h"
#include "image-encode.h"
#include <obs-module.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ── Source private data ───────────────────────────────────────────────────

struct TranslateData {
    obs_source_t *source;

    std::string api_key;
    std::mutex        result_mutex;
    std::string       translation;
    std::atomic<bool> translating{false};
    std::thread       worker;

    // 空字符串 = 当前场景（raw video callback），否则按名字捕获具体 source
    std::string target_source_name;
    int  capture_interval_sec = 10;
    bool auto_capture_enabled = false;

    // 当前场景模式：时间戳节流 + 手动触发标志
    std::atomic<int64_t> last_capture_ns{0};
    std::atomic<bool>    manual_capture_requested{false};

    // 具体 source 模式：video_tick 累计秒数
    float elapsed_since_capture = 0.0f;

    // 具体 source 模式：图形资源（在图形线程创建/销毁）
    gs_texrender_t *texrender    = nullptr;
    gs_stagesurf_t *stagesurface = nullptr;
    uint32_t        capture_cx   = 0;
    uint32_t        capture_cy   = 0;
};

// Forward declaration
static bool translate_clicked(obs_properties_t *props, obs_property_t *, void *);

// ── Worker thread ─────────────────────────────────────────────────────────

static void start_translation_worker(TranslateData *data,
                                     std::vector<uint8_t> jpeg,
                                     std::string api_key)
{
    data->translating.store(true);
    if (data->worker.joinable())
        data->worker.join();

    obs_source_t *source = data->source;

    data->worker = std::thread([data, source,
                                jpeg    = std::move(jpeg),
                                api_key = std::move(api_key)]() mutable {
        std::string result = claude_analyze_image_data(jpeg, "image/jpeg", api_key);

        blog(LOG_INFO, "[game-translator] 翻译结果:\n%s", result.c_str());

        {
            std::lock_guard<std::mutex> lock(data->result_mutex);
            data->translation = result;
        }

        obs_data_t *settings = obs_source_get_settings(source);
        obs_data_set_string(settings, "translation_result", result.c_str());
        obs_data_release(settings);

        obs_source_update_properties(source);

        data->translating.store(false);
    });
}

// ── 当前场景：raw video callback ──────────────────────────────────────────
// OBS 在输出帧准备好后调用此函数（与录播/推流拿到的是同一帧）。

static void on_raw_video(void *param, struct video_data *frame)
{
    auto *data = static_cast<TranslateData *>(param);

    // 只在"当前场景"模式下使用
    if (!data->target_source_name.empty())
        return;
    if (data->translating.load())
        return;

    bool manual = data->manual_capture_requested.exchange(false);
    if (!manual) {
        if (!data->auto_capture_enabled)
            return;
        int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        int64_t interval_ns = (int64_t)data->capture_interval_sec * 1'000'000'000LL;
        int64_t last = data->last_capture_ns.load(std::memory_order_relaxed);
        if (now - last < interval_ns)
            return;
        data->last_capture_ns.store(now, std::memory_order_relaxed);
    }

    // 获取输出分辨率
    struct obs_video_info ovi;
    if (!obs_get_video_info(&ovi))
        return;
    uint32_t cx = ovi.output_width;
    uint32_t cy = ovi.output_height;
    if (cx == 0 || cy == 0 || !frame->data[0])
        return;

    // 拷贝像素（BGRA，OBS 在注册时已指定格式转换）
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

    std::vector<uint8_t> jpeg = encode_bgra_to_jpeg(pixels.data(), cx, cy);
    if (jpeg.empty())
        return;

    blog(LOG_INFO, "[game-translator] 捕获当前场景 %zu bytes，开始翻译", jpeg.size());
    start_translation_worker(data, std::move(jpeg), std::move(api_key));
}

// ── 具体 source：texrender 帧捕获 ─────────────────────────────────────────

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

    return encode_bgra_to_jpeg(pixels.data(), cx, cy);
}

// ── OBS source callbacks ──────────────────────────────────────────────────

static const char *translate_get_name(void *)
{
    return "Game Translator";
}

static void translate_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, "capture_interval", 10);
    obs_data_set_default_bool(settings, "auto_capture", false);
}

static void *translate_create(obs_data_t *settings, obs_source_t *source)
{
    auto *data = new TranslateData{};
    data->source = source;
    data->api_key              = obs_data_get_string(settings, "api_key");
    data->target_source_name   = obs_data_get_string(settings, "target_source");
    data->capture_interval_sec = (int)obs_data_get_int(settings, "capture_interval");
    data->auto_capture_enabled = obs_data_get_bool(settings, "auto_capture");

    // 注册 raw video callback，用于"当前场景"模式
    struct video_scale_info vsi = {};
    vsi.format = VIDEO_FORMAT_BGRA;
    obs_add_raw_video_callback(&vsi, on_raw_video, data);

    return data;
}

static void translate_destroy(void *priv)
{
    auto *data = static_cast<TranslateData *>(priv);

    obs_remove_raw_video_callback(on_raw_video, data);

    if (data->worker.joinable())
        data->worker.join();

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
    data->api_key              = obs_data_get_string(settings, "api_key");
    data->target_source_name   = obs_data_get_string(settings, "target_source");
    data->capture_interval_sec = (int)obs_data_get_int(settings, "capture_interval");
    data->auto_capture_enabled = obs_data_get_bool(settings, "auto_capture");
    // 切换 source 时重置计时器
    data->elapsed_since_capture = 0.0f;
    data->last_capture_ns.store(0, std::memory_order_relaxed);
}

// ── video_tick — 具体 source 模式的定时捕获 ──────────────────────────────

static void translate_video_tick(void *priv, float seconds)
{
    auto *data = static_cast<TranslateData *>(priv);

    // 当前场景模式由 raw video callback 处理
    if (data->target_source_name.empty())
        return;
    if (!data->auto_capture_enabled)
        return;

    data->elapsed_since_capture += seconds;
    if (data->elapsed_since_capture < (float)data->capture_interval_sec)
        return;

    data->elapsed_since_capture = 0.0f;

    if (data->translating.load())
        return;

    std::string api_key;
    {
        std::lock_guard<std::mutex> lock(data->result_mutex);
        api_key = data->api_key;
    }

    std::vector<uint8_t> jpeg = capture_source_frame(data);
    if (jpeg.empty()) {
        blog(LOG_WARNING, "[game-translator] 帧捕获失败，目标源可能未激活");
        return;
    }

    blog(LOG_INFO, "[game-translator] 自动捕获帧 %zu bytes，开始翻译", jpeg.size());
    start_translation_worker(data, std::move(jpeg), std::move(api_key));
}

// ── Properties panel ──────────────────────────────────────────────────────

static obs_properties_t *translate_get_properties(void *priv)
{
    obs_properties_t *props = obs_properties_create();
    obs_properties_set_param(props, priv, nullptr);

    obs_property_t *src_list = obs_properties_add_list(
        props, "target_source", "捕获源",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(src_list, "当前场景（推流/录制画面）", "");
    obs_enum_sources([](void *param, obs_source_t *src) -> bool {
        if (obs_source_get_output_flags(src) & OBS_SOURCE_VIDEO) {
            auto *list = static_cast<obs_property_t *>(param);
            const char *name = obs_source_get_name(src);
            obs_property_list_add_string(list, name, name);
        }
        return true;
    }, src_list);

    obs_properties_add_int(props, "capture_interval", "捕获间隔（秒）", 3, 60, 1);
    obs_properties_add_bool(props, "auto_capture", "自动捕获");
    obs_properties_add_text(props, "api_key",
                            "Anthropic API Key（留空则读取 ANTHROPIC_API_KEY 环境变量）",
                            OBS_TEXT_PASSWORD);
    obs_properties_add_button(props, "translate_btn", "分析 & 翻译", translate_clicked);
    obs_properties_add_text(props, "translation_result", "翻译结果",
                            OBS_TEXT_MULTILINE);

    return props;
}

// ── Button callback ───────────────────────────────────────────────────────

static bool translate_clicked(obs_properties_t *props, obs_property_t *, void *)
{
    auto *data = static_cast<TranslateData *>(obs_properties_get_param(props));
    if (!data || data->translating.load())
        return false;

    if (data->target_source_name.empty()) {
        // 当前场景：通知 raw video callback 在下一帧捕获
        data->manual_capture_requested.store(true);
    } else {
        // 具体 source：直接捕获
        std::string api_key;
        {
            std::lock_guard<std::mutex> lock(data->result_mutex);
            api_key = data->api_key;
        }

        std::vector<uint8_t> jpeg = capture_source_frame(data);
        if (jpeg.empty()) {
            blog(LOG_WARNING, "[game-translator] 帧捕获失败，请确认捕获源正在运行");
            return false;
        }

        blog(LOG_INFO, "[game-translator] 手动捕获帧 %zu bytes，开始翻译", jpeg.size());
        start_translation_worker(data, std::move(jpeg), std::move(api_key));
    }

    return false;
}

// ── Registration ──────────────────────────────────────────────────────────

static struct obs_source_info s_translate_info = {};

void register_translate_source()
{
    s_translate_info.id             = "game_translator_source";
    s_translate_info.type           = OBS_SOURCE_TYPE_INPUT;
    s_translate_info.output_flags   = OBS_SOURCE_DO_NOT_DUPLICATE;
    s_translate_info.get_name       = translate_get_name;
    s_translate_info.get_defaults   = translate_get_defaults;
    s_translate_info.create         = translate_create;
    s_translate_info.destroy        = translate_destroy;
    s_translate_info.update         = translate_update;
    s_translate_info.video_tick     = translate_video_tick;
    s_translate_info.get_properties = translate_get_properties;
    obs_register_source(&s_translate_info);
}
