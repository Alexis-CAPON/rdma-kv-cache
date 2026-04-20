#pragma once

#include <string>
#include <curl/curl.h>

struct HTTPResponse {
    bool success;
    int status_code;
    std::string body;
    std::string error;
};

class HTTPClient {
public:
    HTTPClient();
    ~HTTPClient();

    /**
     * Perform HTTP POST request
     * @param url Target URL
     * @param json_body JSON string to send as body
     * @return HTTPResponse with result
     */
    HTTPResponse post(const std::string& url, const std::string& json_body);

private:
    CURL* curl_;

    static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp);
};
