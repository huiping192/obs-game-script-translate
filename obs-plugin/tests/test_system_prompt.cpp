#include <catch2/catch_test_macros.hpp>
#include "llm-utils.h"

TEST_CASE("build_system_prompt zh contains language name",
          "[system-prompt]")
{
	auto prompt = build_system_prompt("zh");
	CHECK(prompt.find("Simplified Chinese") != std::string::npos);
}

TEST_CASE("build_system_prompt ja contains language name",
          "[system-prompt]")
{
	auto prompt = build_system_prompt("ja");
	CHECK(prompt.find("Japanese") != std::string::npos);
}

TEST_CASE("build_system_prompt en contains language name",
          "[system-prompt]")
{
	auto prompt = build_system_prompt("en");
	CHECK(prompt.find("English") != std::string::npos);
}

TEST_CASE("build_system_prompt zh contains no-text response",
          "[system-prompt]")
{
	auto prompt = build_system_prompt("zh");
	CHECK(prompt.find("截图中未检测到剧情文本") != std::string::npos);
}

TEST_CASE("build_system_prompt contains output format rules",
          "[system-prompt]")
{
	auto prompt = build_system_prompt("zh");
	CHECK(prompt.find("plain text") != std::string::npos);
}
