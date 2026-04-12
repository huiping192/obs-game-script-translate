#include <catch2/catch_test_macros.hpp>
#include "image-encode.h"
#include <vector>

TEST_CASE("encode_bgra_to_jpeg returns empty on null input",
          "[image-encode]")
{
	auto jpeg = encode_bgra_to_jpeg(nullptr, 0, 0);
	REQUIRE(jpeg.empty());
}

TEST_CASE("encode_bgra_to_jpeg returns empty on zero dimensions",
          "[image-encode]")
{
	uint8_t dummy = 0;
	auto jpeg = encode_bgra_to_jpeg(&dummy, 0, 10);
	REQUIRE(jpeg.empty());
	jpeg = encode_bgra_to_jpeg(&dummy, 10, 0);
	REQUIRE(jpeg.empty());
}

TEST_CASE("encode_bgra_to_jpeg produces valid JPEG from 2x2 red pixels",
          "[image-encode]")
{
	std::vector<uint8_t> bgra = {
		0, 0, 255, 255,
		0, 0, 255, 255,
		0, 0, 255, 255,
		0, 0, 255, 255,
	};
	auto jpeg = encode_bgra_to_jpeg(bgra.data(), 2, 2);
	REQUIRE_FALSE(jpeg.empty());
	CHECK(jpeg[0] == static_cast<uint8_t>(0xFF));
	CHECK(jpeg[1] == static_cast<uint8_t>(0xD8));
}

TEST_CASE("encode_bgra_to_jpeg downscales large image",
          "[image-encode]")
{
	std::vector<uint8_t> bgra(1920 * 1080 * 4, 128);
	auto jpeg = encode_bgra_to_jpeg(bgra.data(), 1920, 1080, 480, 50);
	REQUIRE_FALSE(jpeg.empty());
	CHECK(jpeg.size() < bgra.size());
}

TEST_CASE("encode_bgra_to_jpeg does not downscale when width < max_width",
          "[image-encode]")
{
	std::vector<uint8_t> bgra(8 * 8 * 4, 64);
	auto jpeg = encode_bgra_to_jpeg(bgra.data(), 8, 8);
	REQUIRE_FALSE(jpeg.empty());
	CHECK(jpeg[0] == static_cast<uint8_t>(0xFF));
	CHECK(jpeg[1] == static_cast<uint8_t>(0xD8));
}
