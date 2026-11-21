module;

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

export module llm;

import json;
import http;

export namespace sparsity::llm {

    using json_type = json::json_type;

    struct Usage {
        int input_tokens{0};
        int output_tokens{0};
        int cache_read{0};
        int cache_creation{0};

        int total_input() const {
            return input_tokens + cache_read + cache_creation;
        }

        int total() const {
            return total_input() + output_tokens;
        }

        void update(const json_type &value);
        json_type to_json() const;
    };

    struct ToolCall {
        std::string id;
        std::string name;
        json_type input;
    };

    struct AssistantMessage {
        std::vector<json_type> content;
        std::optional<std::string> stop_reason;
        Usage usage;
        std::string model;
        bool interrupted{false};

        std::string text() const;
        std::string thinking() const;
        std::vector<ToolCall> tool_calls() const;
        json_type to_wire() const;
    };

    struct StreamEvent {
        std::string type;
        int index{-1};
        json_type block;
        std::string text;
        std::string signature;
        std::string partial_json;
        json_type usage;
        std::string stop_reason;
        std::string model;
    };

    struct Request {
        std::string model;
        std::string system;
        json_type messages{json_type::array()};
        int max_tokens{65536};
        std::string effort{"high"};
        json_type tools{json_type::array()};
        json_type tool_choice;

        json_type body() const;
    };

    class Error final : public std::runtime_error {
    public:
        Error(std::string message, std::string kind, const int status = 0,
              const bool retryable = false, const double retry_after = 0.0)
            : std::runtime_error(std::move(message)), kind(std::move(kind)), status(status),
              retryable(retryable), retry_after(retry_after) {
        }

        std::string kind;
        int status;
        bool retryable;
        double retry_after;
    };

    class MessageAccumulator {
    public:
        void handle(const StreamEvent &event);
        AssistantMessage message() const;
        bool finished() const;

    private:
        AssistantMessage progress_;
        std::map<int, std::size_t> positions_;
        std::map<int, std::string> partial_json_;
        bool finished_{false};
    };

    class Client {
    public:
        Client(std::string api_key, std::string base_url, int max_retries = 5);

        void stream(const Request &request, const std::function<void(const StreamEvent &)> &sink,
                    http::Cancel &cancel);

    private:
        http::Url url_;
        std::string api_key_;
        int max_retries_;
    };

}

namespace sparsity::llm {

    std::string trim_left(std::string value) {
        value.erase(0, value.find_first_not_of(" \t\r\n"));
        return value;
    }

    json_type object_field(const json_type &value, const char *name) {
        if (!value.is_object()) return json_type::object();
        const auto found = value.find(name);
        return found != value.end() && found->is_object() ? *found : json_type::object();
    }

    std::string string_field(const json_type &value, const char *name, std::string fallback = {}) {
        if (!value.is_object()) return fallback;
        const auto found = value.find(name);
        return found != value.end() && found->is_string() ? found->get<std::string>() : std::move(fallback);
    }

    double retry_after(const http::Response &response) {
        const auto found = response.headers.find("retry-after");
        if (found == response.headers.end()) return 0.0;
        try {
            return std::max(0.0, std::stod(found->second));
        } catch (...) {
            return 0.0;
        }
    }

    Error error_for(const http::Response &response) {
        std::string message = response.body;
        try {
            const auto payload = json_type::parse(response.body);
            if (payload.is_object()) {
                if (const auto error = payload.find("error"); error != payload.end() && error->is_object()) {
                    message = string_field(*error, "message", message);
                } else if (payload.contains("message") && payload["message"].is_string()) {
                    message = payload["message"].get<std::string>();
                }
            }
        } catch (...) {
        }
        if (message.empty()) message = "request failed";

        const int status = response.status;
        if (status == 401 || status == 403) return Error(message, "auth", status, false, 0.0);
        if (status == 402) return Error(message, "quota", status, false, 0.0);
        if (status == 429) return Error(message, "rate_limit", status, true, retry_after(response));
        if (status >= 500) return Error(message, "server", status, true, retry_after(response));
        std::string lower = message;
        for (auto &character: lower) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        if (status == 400 && (lower.find("context") != std::string::npos ||
                              lower.find("length") != std::string::npos ||
                              lower.find("too long") != std::string::npos)) {
            return Error(message, "context_length", status, false, 0.0);
        }
        return Error(message, "invalid_request", status, false, 0.0);
    }

    std::string endpoint_for(std::string base_url) {
        while (!base_url.empty() && base_url.back() == '/') base_url.pop_back();
        return base_url.ends_with("/v1") ? base_url + "/messages" : base_url + "/v1/messages";
    }

    class SseParser {
    public:
        template<typename Handler>
        void feed(const std::string_view chunk, Handler &&handler) {
            buffer_.append(chunk);
            std::size_t start = 0;
            while (true) {
                const auto newline = buffer_.find('\n', start);
                if (newline == std::string::npos) break;
                std::string line = buffer_.substr(start, newline - start);
                start = newline + 1;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) {
                    if (!data_.empty()) {
                        handler(event_, data_);
                        event_.clear();
                        data_.clear();
                    }
                } else if (line.starts_with("event:")) {
                    event_ = line.substr(6);
                } else if (line.starts_with("data:")) {
                    auto value = line.substr(5);
                    if (!value.empty() && value.front() == ' ') value.erase(0, 1);
                    if (!data_.empty()) data_.push_back('\n');
                    data_ += value;
                }
            }
            buffer_.erase(0, start);
        }

    private:
        std::string buffer_;
        std::string event_;
        std::string data_;
    };

    void dispatch(const std::string_view event, const std::string_view data,
                  const std::function<void(const StreamEvent &)> &sink, bool &finished) {
        json_type payload;
        try {
            payload = json_type::parse(data);
        } catch (const std::exception &error) {
            throw Error(std::string("malformed stream: ") + error.what(), "malformed", 0, false);
        }
        const auto kind = string_field(payload, "type", std::string(event));
        if (kind == "ping") return;

        if (kind == "message_start") {
            const auto message = object_field(payload, "message");
            StreamEvent value;
            value.type = "message_start";
            value.model = string_field(message, "model");
            value.usage = object_field(message, "usage");
            sink(value);
            return;
        }
        if (kind == "content_block_start") {
            StreamEvent value;
            value.type = "block_start";
            value.index = payload.value("index", 0);
            value.block = object_field(payload, "content_block");
            sink(value);
            return;
        }
        if (kind == "content_block_delta") {
            const auto delta = object_field(payload, "delta");
            const auto delta_type = string_field(delta, "type");
            StreamEvent value;
            value.index = payload.value("index", 0);
            if (delta_type == "text_delta") {
                value.type = "text_delta";
                value.text = string_field(delta, "text");
            } else if (delta_type == "thinking_delta") {
                value.type = "thinking_delta";
                value.text = string_field(delta, "thinking");
            } else if (delta_type == "signature_delta") {
                value.type = "signature_delta";
                value.signature = string_field(delta, "signature");
            } else if (delta_type == "input_json_delta") {
                value.type = "input_json_delta";
                value.partial_json = string_field(delta, "partial_json");
            } else {
                return;
            }
            sink(value);
            return;
        }
        if (kind == "content_block_stop") {
            StreamEvent value;
            value.type = "block_stop";
            value.index = payload.value("index", 0);
            sink(value);
            return;
        }
        if (kind == "message_delta") {
            const auto delta = object_field(payload, "delta");
            StreamEvent value;
            value.type = "usage";
            value.stop_reason = string_field(delta, "stop_reason");
            if (const auto usage = payload.find("usage"); usage != payload.end() && usage->is_object()) {
                value.usage = *usage;
            }
            sink(value);
            return;
        }
        if (kind == "message_stop") {
            finished = true;
            StreamEvent value;
            value.type = "message_stop";
            value.stop_reason = string_field(payload, "stop_reason");
            sink(value);
            return;
        }
        if (kind == "error") {
            const auto error = object_field(payload, "error");
            throw Error(string_field(error, "message", "provider reported an error"), "server", 0, false);
        }
    }

}

export namespace sparsity::llm {

    void Usage::update(const json_type &value) {
        if (!value.is_object()) return;
        if (const auto found = value.find("input_tokens"); found != value.end() && found->is_number()) {
            input_tokens = found->get<int>();
        }
        if (const auto found = value.find("output_tokens"); found != value.end() && found->is_number()) {
            output_tokens = found->get<int>();
        }
        if (const auto found = value.find("cache_read_input_tokens"); found != value.end() && found->is_number()) {
            cache_read = found->get<int>();
        }
        if (const auto found = value.find("cache_creation_input_tokens"); found != value.end() && found->is_number()) {
            cache_creation = found->get<int>();
        }
    }

    json_type Usage::to_json() const {
        return {
            {"input_tokens", input_tokens},
            {"output_tokens", output_tokens},
            {"cache_read_input_tokens", cache_read},
            {"cache_creation_input_tokens", cache_creation},
        };
    }

    std::string AssistantMessage::text() const {
        std::string result;
        for (const auto &block: content) {
            if (block.is_object() && block.value("type", std::string{}) == "text") {
                result += block.value("text", std::string{});
            }
        }
        return result;
    }

    std::string AssistantMessage::thinking() const {
        std::string result;
        for (const auto &block: content) {
            if (block.is_object() && block.value("type", std::string{}) == "thinking") {
                result += block.value("thinking", std::string{});
            }
        }
        return result;
    }

    std::vector<ToolCall> AssistantMessage::tool_calls() const {
        std::vector<ToolCall> result;
        for (const auto &block: content) {
            if (!block.is_object() || block.value("type", std::string{}) != "tool_use") continue;
            ToolCall call;
            call.id = block.value("id", std::string{});
            call.name = block.value("name", std::string{});
            if (const auto found = block.find("input"); found != block.end() && found->is_object()) {
                call.input = *found;
            }
            result.push_back(std::move(call));
        }
        return result;
    }

    json_type AssistantMessage::to_wire() const {
        return {{"role", "assistant"}, {"content", content}};
    }

    json_type Request::body() const {
        json_type result = {
            {"model", model},
            {"max_tokens", max_tokens},
            {"system", system},
            {"messages", messages},
            {"stream", true},
        };
        if (!tools.empty()) result["tools"] = tools;
        if (!tool_choice.is_null()) result["tool_choice"] = tool_choice;
        if (effort == "off") {
            result["thinking"] = {{"type", "disabled"}};
        } else {
            result["thinking"] = {{"type", "enabled"}};
            result["output_config"] = {{"effort", effort}};
        }
        return result;
    }

    void MessageAccumulator::handle(const StreamEvent &event) {
        if (event.type == "message_start") {
            progress_.model = event.model;
            progress_.usage.update(event.usage);
            return;
        }
        if (event.type == "block_start") {
            positions_[event.index] = progress_.content.size();
            progress_.content.push_back(event.block);
            return;
        }
        if (event.type == "text_delta" || event.type == "thinking_delta" ||
            event.type == "signature_delta") {
            const auto found = positions_.find(event.index);
            if (found == positions_.end()) return;
            auto &block = progress_.content[found->second];
            if (event.type == "text_delta") {
                block["text"] = block.value("text", std::string{}) + event.text;
            } else if (event.type == "thinking_delta") {
                block["thinking"] = block.value("thinking", std::string{}) + event.text;
            } else {
                block["signature"] = block.value("signature", std::string{}) + event.signature;
            }
            return;
        }
        if (event.type == "input_json_delta") {
            partial_json_[event.index] += event.partial_json;
            return;
        }
        if (event.type == "block_stop") {
            const auto found = positions_.find(event.index);
            if (found == positions_.end()) return;
            auto &block = progress_.content[found->second];
            const auto partial = partial_json_.find(event.index);
            if (block.value("type", std::string{}) == "tool_use" && partial != partial_json_.end()) {
                try {
                    const auto parsed = json_type::parse(partial->second);
                    if (parsed.is_object()) block["input"] = parsed;
                } catch (...) {
                    block["input"] = json_type::object();
                }
            }
            partial_json_.erase(event.index);
            return;
        }
        if (event.type == "usage") {
            progress_.usage.update(event.usage);
            if (!event.stop_reason.empty()) progress_.stop_reason = event.stop_reason;
            return;
        }
        if (event.type == "message_stop") {
            if (!event.stop_reason.empty()) progress_.stop_reason = event.stop_reason;
            else if (!progress_.stop_reason.has_value()) progress_.stop_reason = "end_turn";
            finished_ = true;
        }
    }

    AssistantMessage MessageAccumulator::message() const {
        return progress_;
    }

    bool MessageAccumulator::finished() const {
        return finished_;
    }

    Client::Client(std::string api_key, std::string base_url, const int max_retries)
        : url_(http::parse_url(endpoint_for(std::move(base_url)))),
          api_key_(std::move(api_key)), max_retries_(max_retries) {
    }

    void Client::stream(const Request &request, const std::function<void(const StreamEvent &)> &sink,
                        http::Cancel &cancel) {
        const auto body = request.body().dump(-1, ' ', false, json_type::error_handler_t::replace);
        const http::Headers headers = {
            {"x-api-key", api_key_},
            {"anthropic-version", "2023-06-01"},
            {"content-type", "application/json"},
            {"accept", "text/event-stream"},
        };

        int attempt = 0;
        while (true) {
            bool saw_any = false;
            bool finished = false;
            SseParser parser;
            try {
                const auto response = http::perform("POST", url_, headers, body,
                    [&parser, &sink, &saw_any, &finished](const std::string_view chunk) {
                        parser.feed(chunk, [&](const std::string_view event, const std::string_view data) {
                            saw_any = true;
                            dispatch(event, data, sink, finished);
                        });
                        return !finished;
                    }, cancel);
                if (response.status >= 400) throw error_for(response);
                if (!finished) throw Error("stream ended before message_stop", "stream_closed", 0, true);
                return;
            } catch (const http::Canceled &) {
                throw Error("canceled", "canceled", 0, false);
            } catch (const Error &error) {
                if (!error.retryable || saw_any || attempt >= max_retries_) throw;
                const auto delay = error.retry_after > 0.0
                                       ? error.retry_after
                                       : std::min(20.0, 0.8 * static_cast<double>(1 << attempt));
                ++attempt;
                for (int tick = 0; tick < static_cast<int>(delay * 10.0); ++tick) {
                    if (cancel.canceled()) throw Error("canceled", "canceled", 0, false);
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            } catch (const std::exception &error) {
                if (cancel.canceled()) throw Error("canceled", "canceled", 0, false);
                if (saw_any || attempt >= max_retries_) throw Error(error.what(), "transport", 0, true);
                ++attempt;
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    static_cast<int>(std::min(20.0, 0.8 * static_cast<double>(1 << attempt)) * 1000.0)));
            }
        }
    }

}
