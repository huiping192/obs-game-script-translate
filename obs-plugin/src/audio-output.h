#pragma once
#include <obs-module.h>
#include <cstdint>
#include <vector>

// Decodes audio_bytes (mp3/wav) via miniaudio and pushes PCM to OBS source.
void push_audio_to_obs_source(obs_source_t *source,
                               const std::vector<uint8_t> &audio_bytes);
