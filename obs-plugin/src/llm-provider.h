#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

struct curl_slist;

class LlmProvider {
public:
    virtual ~LlmProvider() = default;

    std::string analyze_image(const std::vector<uint8_t> &image_data,
                              const std::string &media_type,
                              const std::string &target_language);

    static std::unique_ptr<LlmProvider> create(const std::string &provider,
                                                const std::string &api_key);

    explicit LlmProvider(std::string api_key);

protected:
    virtual std::string endpoint_url() const = 0;
    virtual struct curl_slist *build_headers() const = 0;
    virtual nlohmann::json build_request_body(const std::string &b64,
                                              const std::string &media_type,
                                              const std::string &system_prompt,
                                              const char *user_message) const = 0;
    // Returns the translated text on success, empty string if not found.
    virtual std::string extract_response_text(const nlohmann::json &resp) const = 0;

    std::string api_key_;
};

// Backward-compatible free function — translate-source.cpp continues unchanged.
std::string analyze_image_data(const std::vector<uint8_t> &image_data,
                                const std::string &media_type,
                                const std::string &api_key,
                                const std::string &provider,
                                const std::string &target_language);
