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

    // Decode to float32 stereo 48000 Hz (OBS internal rate)
    ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, 2, 48000);
    ma_decoder decoder;
    if (ma_decoder_init_memory(audio_bytes.data(), audio_bytes.size(),
                               &dc, &decoder) != MA_SUCCESS) {
        blog(LOG_ERROR, "[game-translator] audio: miniaudio decode init failed");
        return;
    }

    // Stream-decode in 4096-frame chunks to avoid large pre-allocation
    const ma_uint64 DECODE_CHUNK = 4096;
    std::vector<float> tmp(DECODE_CHUNK * 2);
    std::vector<float> pcm;
    pcm.reserve(48000 * 2 * 10); // rough 10s pre-alloc

    ma_uint64 got = 0;
    while (true) {
        ma_result r = ma_decoder_read_pcm_frames(&decoder, tmp.data(), DECODE_CHUNK, &got);
        if (got == 0) break;
        pcm.insert(pcm.end(), tmp.begin(), tmp.begin() + (size_t)(got * 2));
        if (r != MA_SUCCESS) break;
    }
    ma_decoder_uninit(&decoder);

    ma_uint64 total_frames = pcm.size() / 2;
    if (total_frames == 0) return;

    blog(LOG_INFO, "[game-translator] audio: pushing %llu PCM frames to OBS",
         (unsigned long long)total_frames);

    // Push to OBS in 1024-frame chunks with monotonic timestamps
    const uint32_t PUSH_CHUNK = 1024;
    uint64_t ts = os_gettime_ns();

    for (ma_uint64 off = 0; off < total_frames; off += PUSH_CHUNK) {
        uint32_t chunk = (uint32_t)(std::min)((ma_uint64)PUSH_CHUNK, total_frames - off);

        struct obs_source_audio osa = {};
        osa.data[0]        = reinterpret_cast<const uint8_t *>(pcm.data() + off * 2);
        osa.frames         = chunk;
        osa.speakers       = SPEAKERS_STEREO;
        osa.format         = AUDIO_FORMAT_FLOAT;
        osa.samples_per_sec = 48000;
        osa.timestamp      = ts;

        obs_source_output_audio(source, &osa);

        ts += (uint64_t)chunk * 1000000000ULL / 48000;
    }
}
