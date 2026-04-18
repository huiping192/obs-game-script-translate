// Decode mp3/wav bytes via miniaudio and push as PCM to an OBS audio source.
// MINIAUDIO_IMPLEMENTATION is defined only here across the entire plugin.
#define MA_NO_DEVICE_IO
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "audio-output.h"
#include <obs-module.h>
#include <util/platform.h>
#include <algorithm>
#include <vector>

void push_audio_to_obs_source(obs_source_t *source,
                               const std::vector<uint8_t> &audio_bytes)
{
    if (audio_bytes.empty()) return;

    ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, 2, 48000);
    ma_decoder decoder;
    if (ma_decoder_init_memory(audio_bytes.data(), audio_bytes.size(),
                               &dc, &decoder) != MA_SUCCESS) {
        blog(LOG_ERROR, "[game-translator] audio: miniaudio decode init failed");
        return;
    }

    const ma_uint64 DECODE_CHUNK = 4096;
    std::vector<float> tmp(DECODE_CHUNK * 2);
    std::vector<float> interleaved;
    interleaved.reserve(48000 * 2 * 10);

    ma_uint64 got = 0;
    while (true) {
        ma_result r = ma_decoder_read_pcm_frames(&decoder, tmp.data(), DECODE_CHUNK, &got);
        if (got == 0) break;
        interleaved.insert(interleaved.end(), tmp.begin(), tmp.begin() + (size_t)(got * 2));
        if (r != MA_SUCCESS) break;
    }
    ma_decoder_uninit(&decoder);

    ma_uint64 total_frames = interleaved.size() / 2;
    if (total_frames == 0) return;

    blog(LOG_INFO, "[game-translator] audio: pushing %llu PCM frames to OBS",
         (unsigned long long)total_frames);

    // Deinterleave to planar (L|L|L... R|R|R...) to match OBS internal format
    std::vector<float> left(total_frames);
    std::vector<float> right(total_frames);
    for (ma_uint64 i = 0; i < total_frames; i++) {
        left[i]  = interleaved[i * 2];
        right[i] = interleaved[i * 2 + 1];
    }

    const uint32_t PUSH_CHUNK = 1024;
    // Keep 300ms of audio buffered ahead of real-time. This prevents Windows
    // WASAPI monitor from overflowing its fixed 1-second buffer: a burst push
    // fills the buffer, GetBuffer() fails, the monitor reconnects and discards
    // all queued audio — leaving only the last segment audible.
    const uint64_t LEAD_NS = 300ULL * 1000000ULL;
    uint64_t start_wall = os_gettime_ns();
    uint64_t ts = start_wall;

    for (ma_uint64 off = 0; off < total_frames; off += PUSH_CHUNK) {
        uint32_t chunk = (uint32_t)(std::min)((ma_uint64)PUSH_CHUNK, total_frames - off);

        struct obs_source_audio osa = {};
        osa.data[0]         = reinterpret_cast<const uint8_t *>(left.data() + off);
        osa.data[1]         = reinterpret_cast<const uint8_t *>(right.data() + off);
        osa.frames          = chunk;
        osa.speakers        = SPEAKERS_STEREO;
        osa.format          = AUDIO_FORMAT_FLOAT_PLANAR;
        osa.samples_per_sec = 48000;
        osa.timestamp       = ts;

        obs_source_output_audio(source, &osa);

        ts += (uint64_t)chunk * 1000000000ULL / 48000;

        uint64_t pushed_ns = ts - start_wall;
        if (pushed_ns > LEAD_NS)
            os_sleepto_ns(start_wall + pushed_ns - LEAD_NS);
    }
}
