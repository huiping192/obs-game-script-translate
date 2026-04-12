#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct curl_slist;

struct LangConfig {
    const char *language_name;
    const char *no_text_response;
    const char *user_message;
};

const LangConfig &get_lang_config(const std::string &lang);
std::string build_system_prompt(const std::string &target_language);
std::string base64_encode(const std::vector<uint8_t> &data);
std::string do_post(const char *url, struct curl_slist *headers,
                    const std::string &body_str);
