#include <catch2/catch_test_macros.hpp>
#include "llm-providers-internal.h"

class ProviderTestAccess {
public:
	static nlohmann::json call_build_request_body(
		LlmProvider &p, const std::string &b64,
		const std::string &media_type,
		const std::string &system_prompt,
		const char *user_message)
	{
		return p.build_request_body(b64, media_type, system_prompt,
					    user_message);
	}

	static std::string call_extract_response_text(LlmProvider &p,
						      const nlohmann::json &resp)
	{
		return p.extract_response_text(resp);
	}
};

static ClaudeProvider make_claude()
{
	return ClaudeProvider("test-key");
}

static GlmProvider make_glm()
{
	return GlmProvider("test-key");
}

TEST_CASE("ClaudeProvider build_request_body structure",
          "[provider][claude]")
{
	auto p = make_claude();
	auto body = ProviderTestAccess::call_build_request_body(
		p, "b64data", "image/jpeg", "sys prompt", "translate this");

	CHECK(body["model"] == "claude-sonnet-4-6");
	CHECK(body["max_tokens"] == 2048);
	CHECK(body["system"] == "sys prompt");

	auto messages = body["messages"];
	REQUIRE(messages.is_array());
	REQUIRE(messages.size() == 1);
	CHECK(messages[0]["role"] == "user");

	auto content = messages[0]["content"];
	REQUIRE(content.is_array());
	REQUIRE(content.size() == 2);
	CHECK(content[0]["type"] == "image");
	CHECK(content[0]["source"]["type"] == "base64");
	CHECK(content[0]["source"]["data"] == "b64data");
	CHECK(content[1]["type"] == "text");
	CHECK(content[1]["text"] == "translate this");
}

TEST_CASE("ClaudeProvider extract_response_text parses content",
          "[provider][claude]")
{
	auto p = make_claude();
	nlohmann::json resp = {
		{"content", {{{"type", "text"}, {"text", "翻译结果"}}}}};
	auto text = ProviderTestAccess::call_extract_response_text(p, resp);
	CHECK(text == "翻译结果");
}

TEST_CASE("ClaudeProvider extract_response_text returns empty on bad json",
          "[provider][claude]")
{
	auto p = make_claude();
	nlohmann::json resp = {{"error", "something"}};
	auto text = ProviderTestAccess::call_extract_response_text(p, resp);
	CHECK(text.empty());

	nlohmann::json empty_resp = {};
	text = ProviderTestAccess::call_extract_response_text(p, empty_resp);
	CHECK(text.empty());
}

TEST_CASE("GlmProvider build_request_body structure",
          "[provider][glm]")
{
	auto p = make_glm();
	auto body = ProviderTestAccess::call_build_request_body(
		p, "b64data", "image/jpeg", "sys prompt", "translate this");

	CHECK(body["model"] == "glm-4.6v");
	CHECK(body["max_tokens"] == 2048);

	auto messages = body["messages"];
	REQUIRE(messages.is_array());
	REQUIRE(messages.size() == 2);
	CHECK(messages[0]["role"] == "system");
	CHECK(messages[0]["content"] == "sys prompt");
	CHECK(messages[1]["role"] == "user");

	auto content = messages[1]["content"];
	REQUIRE(content.is_array());
	REQUIRE(content.size() == 2);
	CHECK(content[0]["type"] == "image_url");
	CHECK(content[0]["image_url"]["url"] ==
	      "data:image/jpeg;base64,b64data");
	CHECK(content[1]["type"] == "text");
	CHECK(content[1]["text"] == "translate this");
}

TEST_CASE("GlmProvider extract_response_text parses choices",
          "[provider][glm]")
{
	auto p = make_glm();
	nlohmann::json resp = {
		{"choices",
		 {{{"message", {{"content", "翻訳結果"}}}}}}};
	auto text = ProviderTestAccess::call_extract_response_text(p, resp);
	CHECK(text == "翻訳結果");
}

TEST_CASE("GlmProvider extract_response_text returns empty on bad json",
          "[provider][glm]")
{
	auto p = make_glm();
	nlohmann::json resp = {{"error", "something"}};
	auto text = ProviderTestAccess::call_extract_response_text(p, resp);
	CHECK(text.empty());

	nlohmann::json empty_resp = {};
	text = ProviderTestAccess::call_extract_response_text(p, empty_resp);
	CHECK(text.empty());
}
