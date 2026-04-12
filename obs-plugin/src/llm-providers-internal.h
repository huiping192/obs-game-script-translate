#pragma once
#include "llm-provider.h"
#include <curl/curl.h>

class ClaudeProvider final : public LlmProvider {
    friend class ProviderTestAccess;
public:
    using LlmProvider::LlmProvider;
protected:
    std::string endpoint_url() const override;
    struct curl_slist *build_headers() const override;
    nlohmann::json build_request_body(const std::string &b64,
                                      const std::string &media_type,
                                      const std::string &system_prompt,
                                      const char *user_message) const override;
    std::string extract_response_text(const nlohmann::json &resp) const override;
};

class GlmProvider final : public LlmProvider {
    friend class ProviderTestAccess;
public:
    using LlmProvider::LlmProvider;
protected:
    std::string endpoint_url() const override;
    struct curl_slist *build_headers() const override;
    nlohmann::json build_request_body(const std::string &b64,
                                      const std::string &media_type,
                                      const std::string &system_prompt,
                                      const char *user_message) const override;
    std::string extract_response_text(const nlohmann::json &resp) const override;
};
