module;

#include <string>
#include <string_view>

export module http;

import boost_wrapper;

export namespace sparsity::http {

    using Url = boost_wrapper::HttpUrl;
    using Headers = boost_wrapper::HttpHeaders;
    using DataSink = boost_wrapper::HttpSink;
    using Response = boost_wrapper::HttpResult;
    using Cancel = boost_wrapper::HttpCancel;
    using Canceled = boost_wrapper::HttpCanceled;

    inline Url parse_url(const std::string_view text) {
        return boost_wrapper::http_parse_url(text);
    }

    inline Response perform(const std::string &method, const Url &url, const Headers &headers,
                            const std::string &body, const DataSink &sink, Cancel &cancel) {
        return boost_wrapper::http_perform(method, url, headers, body, sink, cancel);
    }

    inline Response request(const std::string &method, const Url &url, const Headers &headers,
                            const std::string &body, Cancel *cancel = nullptr) {
        return boost_wrapper::http_request(method, url, headers, body, cancel);
    }

    inline Response get(const Url &url, const Headers &headers = {}, Cancel *cancel = nullptr) {
        return boost_wrapper::http_get(url, headers, cancel);
    }

    inline Response post(const Url &url, const Headers &headers, const std::string &body,
                         Cancel *cancel = nullptr) {
        return boost_wrapper::http_post(url, headers, body, cancel);
    }

}
