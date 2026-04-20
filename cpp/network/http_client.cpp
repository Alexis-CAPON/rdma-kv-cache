#include "cpp/network/http_client.h"
#include "cpp/common/logger.h"
#include <sstream>

HTTPClient::HTTPClient() {
    curl_ = curl_easy_init();
    if (!curl_) {
        Logger::error("Failed to initialize CURL");
    }
}

HTTPClient::~HTTPClient() {
    if (curl_) {
        curl_easy_cleanup(curl_);
    }
}

size_t HTTPClient::write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

HTTPResponse HTTPClient::post(const std::string& url, const std::string& json_body) {
    HTTPResponse response;
    response.success = false;
    response.status_code = 0;

    if (!curl_) {
        response.error = "CURL not initialized";
        return response;
    }

    std::string response_body;

    // Set URL
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());

    // Set POST request
    curl_easy_setopt(curl_, CURLOPT_POST, 1L);

    // Set JSON body
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, json_body.c_str());

    // Set headers
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);

    // Set callback for response
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response_body);

    // Set timeout (30 seconds)
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 30L);

    // Perform request
    CURLcode res = curl_easy_perform(curl_);

    if (res != CURLE_OK) {
        response.error = std::string("CURL error: ") + curl_easy_strerror(res);
        curl_slist_free_all(headers);
        return response;
    }

    // Get status code
    long status_code;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &status_code);
    response.status_code = static_cast<int>(status_code);

    // Clean up
    curl_slist_free_all(headers);

    // Check status code
    if (status_code >= 200 && status_code < 300) {
        response.success = true;
        response.body = response_body;
    } else {
        response.error = "HTTP error " + std::to_string(status_code);
        response.body = response_body;
    }

    return response;
}
