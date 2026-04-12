#include <catch2/catch_test_macros.hpp>
#include "llm-utils.h"

TEST_CASE("base64_encode empty input returns empty string", "[base64]")
{
	REQUIRE(base64_encode({}) == "");
}

TEST_CASE("base64_encode 'Hello' with 1 padding", "[base64]")
{
	std::vector<uint8_t> data = {'H', 'e', 'l', 'l', 'o'};
	REQUIRE(base64_encode(data) == "SGVsbG8=");
}

TEST_CASE("base64_encode single byte with 2 padding", "[base64]")
{
	std::vector<uint8_t> data = {'A'};
	REQUIRE(base64_encode(data) == "QQ==");
}

TEST_CASE("base64_encode two bytes with 1 padding", "[base64]")
{
	std::vector<uint8_t> data = {'A', 'B'};
	REQUIRE(base64_encode(data) == "QUI=");
}

TEST_CASE("base64_encode three bytes no padding", "[base64]")
{
	std::vector<uint8_t> data = {'A', 'B', 'C'};
	REQUIRE(base64_encode(data) == "QUJD");
}
