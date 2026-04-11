#include "image-encode.h"
#include <cstring>

// stb implementations — defined here once, not in header
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

static void append_to_vector(void *context, void *data, int size)
{
    auto *buf = static_cast<std::vector<uint8_t> *>(context);
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    buf->insert(buf->end(), bytes, bytes + size);
}

std::vector<uint8_t> encode_bgra_to_jpeg(const uint8_t *bgra,
                                          uint32_t width, uint32_t height,
                                          uint32_t max_width,
                                          int quality)
{
    if (!bgra || width == 0 || height == 0)
        return {};

    // BGRA → RGB: OBS GS_BGRA has bytes [B, G, R, A] in memory
    std::vector<uint8_t> rgb((size_t)width * height * 3);
    for (uint32_t i = 0; i < width * height; i++) {
        rgb[i * 3 + 0] = bgra[i * 4 + 2]; // R
        rgb[i * 3 + 1] = bgra[i * 4 + 1]; // G
        rgb[i * 3 + 2] = bgra[i * 4 + 0]; // B
    }

    uint32_t out_w = width;
    uint32_t out_h = height;
    std::vector<uint8_t> scaled;

    if (max_width > 0 && width > max_width) {
        out_w = max_width;
        out_h = (uint32_t)((uint64_t)height * max_width / width);
        if (out_h == 0) out_h = 1;

        scaled.resize((size_t)out_w * out_h * 3);
        stbir_resize_uint8_linear(
            rgb.data(), (int)width, (int)height, 0,
            scaled.data(), (int)out_w, (int)out_h, 0,
            STBIR_RGB);
    }

    const uint8_t *src = scaled.empty() ? rgb.data() : scaled.data();
    std::vector<uint8_t> jpeg;
    stbi_write_jpg_to_func(append_to_vector, &jpeg,
                            (int)out_w, (int)out_h, 3,
                            src, quality);
    return jpeg;
}
