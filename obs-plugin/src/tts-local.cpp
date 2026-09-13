#include "tts-local.h"
#include <curl/curl.h>
#include <obs-module.h>

static size_t write_bytes_cb(void *contents, size_t size, size_t nmemb, void *userp)
{
    auto *buf = static_cast<std::vector<uint8_t> *>(userp);
    auto *p   = static_cast<uint8_t *>(contents);
    buf->insert(buf->end(), p, p + size * nmemb);
    return size * nmemb;
}

TtsStatus synthesize_from_frame(const std::string &base_url,
                                 const std::vector<uint8_t> &jpeg,
                                 std::vector<uint8_t> &out_wav)
{
    if (base_url.empty() || jpeg.empty()) return TtsStatus::Error;

    std::string url = base_url + "/f8";
    CURL *curl = curl_easy_init();
    if (!curl) return TtsStatus::Error;

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: image/jpeg");

    out_wav.clear();
    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_POST,           1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     reinterpret_cast<const char *>(jpeg.data()));
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,  static_cast<long>(jpeg.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_bytes_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &out_wav);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);

    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        blog(LOG_ERROR, "[game-translator] tts-local: %s (%s)",
             curl_easy_strerror(res), url.c_str());
        return TtsStatus::Error;
    }
    if (code == 204) return TtsStatus::NoText;
    if (code != 200 || out_wav.empty()) {
        blog(LOG_ERROR, "[game-translator] tts-local: HTTP %ld", code);
        return TtsStatus::Error;
    }
    return TtsStatus::Ok;
}
