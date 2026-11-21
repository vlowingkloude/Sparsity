module;

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module compaction;

import json;
import llm;
import session;
import config;
import http;

export namespace sparsity::compaction {

    using json_type = json::json_type;

    struct Outcome {
        std::string summary;
        int keep_from_seq{0};
        int through_seq{0};
        int before_tokens{0};
        int after_tokens{0};
        llm::Usage usage;
        std::string model;
    };

    class TokenMeter {
    public:
        void observe(int actual, double estimated);
        double estimate_text(const std::string &text) const;
        double estimate_blocks(const json_type &blocks) const;
        int estimate_messages(const json_type &messages) const;

    private:
        double factor_{1.0};
    };

    class Compactor {
    public:
        Compactor(config::Config config, session::Session &session, int window);

        TokenMeter &meter() { return meter_; }
        bool should_compact(const json_type &messages) const;
        std::optional<Outcome> compact(llm::Client &client, http::Cancel &cancel,
                                       const std::string &system, const json_type &tools, bool force);

    private:
        int limit() const;
        int retain_budget() const;
        bool prune();

        config::Config config_;
        session::Session &session_;
        int window_;
        TokenMeter meter_;
    };

}

namespace sparsity::compaction {

    bool cjk(const unsigned int code) {
        return (code >= 0x3400 && code <= 0x4dbf) || (code >= 0x4e00 && code <= 0x9fff) ||
               (code >= 0x3040 && code <= 0x30ff) || (code >= 0xac00 && code <= 0xd7af) ||
               (code >= 0xf900 && code <= 0xfaff);
    }

    json_type summary_message(const std::string &summary) {
        return {
            {"role", "user"},
            {"content", json_type::array({
                {{"type", "text"}, {"text", "<conversation_summary>\n" + summary + "\n</conversation_summary>"}}
            })},
        };
    }

    bool safe_cut(const json_type &messages, const int boundary) {
        if (boundary <= 0 || boundary >= static_cast<int>(messages.size())) return false;
        const auto &message = messages[boundary];
        if (message.value("role", std::string{}) != "user") return false;
        if (message.contains("content") && message["content"].is_array()) {
            for (const auto &block: message["content"]) {
                if (block.is_object() && block.value("type", std::string{}) == "tool_result") return false;
            }
        }
        const auto &previous = messages[boundary - 1];
        if (previous.value("role", std::string{}) != "assistant") return false;
        if (previous.contains("content") && previous["content"].is_array()) {
            for (const auto &block: previous["content"]) {
                if (block.is_object() && block.value("type", std::string{}) == "tool_use") return false;
            }
        }
        return true;
    }

    int choose_boundary(const json_type &messages, const int retain_budget, const TokenMeter &meter) {
        if (messages.size() < 4) return -1;
        double accumulated = 0.0;
        int target = static_cast<int>(messages.size());
        for (int index = static_cast<int>(messages.size()) - 1; index >= 0; --index) {
            accumulated += 4.0 + meter.estimate_blocks(messages[index].value("content", json_type::array()));
            target = index;
            if (accumulated >= retain_budget) break;
        }
        for (int offset = 0; offset < static_cast<int>(messages.size()); ++offset) {
            for (const int candidate: {target + offset, target - offset}) {
                if (candidate >= 2 && candidate < static_cast<int>(messages.size()) &&
                    safe_cut(messages, candidate)) {
                    return candidate;
                }
            }
        }
        return -1;
    }

    constexpr char summary_instruction[] =
        "Summarize the conversation above for your own future reference. Output only the summary, "
        "in the user's language, with these sections:\n\n"
        "## 目标\n## 已完成\n## 关键决定（含理由）\n## 涉及的文件与命令\n## 待办 / 未决\n"
        "## 重要事实与偏好\n\n"
        "Keep paths, commands, numbers, and names exact. Do not address the user.";

}

export namespace sparsity::compaction {

    void TokenMeter::observe(const int actual, const double estimated) {
        if (actual <= 0 || estimated <= 0.0) return;
        const auto ratio = static_cast<double>(actual) / estimated;
        factor_ = std::min(2.0, std::max(0.5, 0.5 * factor_ + 0.5 * ratio));
    }

    double TokenMeter::estimate_text(const std::string &text) const {
        double ascii = 0.0;
        double cjk_chars = 0.0;
        double other = 0.0;
        for (std::size_t index = 0; index < text.size();) {
            const auto first = static_cast<unsigned char>(text[index]);
            std::size_t length = 1;
            unsigned int code = first;
            if (first >= 0xf0) {
                length = 4;
                code = first & 0x07;
            } else if (first >= 0xe0) {
                length = 3;
                code = first & 0x0f;
            } else if (first >= 0xc0) {
                length = 2;
                code = first & 0x1f;
            }
            for (std::size_t offset = 1; offset < length && index + offset < text.size(); ++offset) {
                code = (code << 6) | (static_cast<unsigned char>(text[index + offset]) & 0x3f);
            }
            if (length == 1 && first < 0x80) ++ascii;
            else if (cjk(code)) ++cjk_chars;
            else ++other;
            index += length;
        }
        return (ascii / 3.6 + cjk_chars * 0.6 + other / 2.0) * factor_;
    }

    double TokenMeter::estimate_blocks(const json_type &blocks) const {
        if (!blocks.is_array()) return 0.0;
        double total = 0.0;
        for (const auto &block: blocks) {
            if (!block.is_object()) continue;
            const auto type = block.value("type", std::string{});
            if (type == "text") total += estimate_text(block.value("text", std::string{}));
            else if (type == "thinking") total += estimate_text(block.value("thinking", std::string{}));
            else if (type == "tool_use") {
                total += 20.0 + estimate_text(block.value("input", json_type::object()).dump());
            } else if (type == "tool_result") {
                total += 8.0 + estimate_blocks(block.value("content", json_type::array()));
            } else if (type == "image" || type == "image_ref") {
                total += 1024.0;
            } else {
                total += 8.0;
            }
        }
        return total;
    }

    int TokenMeter::estimate_messages(const json_type &messages) const {
        if (!messages.is_array()) return 0;
        double total = 0.0;
        for (const auto &message: messages) {
            total += 4.0 + estimate_blocks(message.value("content", json_type::array()));
        }
        return static_cast<int>(total);
    }

    Compactor::Compactor(config::Config config, session::Session &session, const int window)
        : config_(std::move(config)), session_(session), window_(window) {
    }

    int Compactor::limit() const {
        const auto by_ratio = static_cast<int>(static_cast<double>(window_) * config_.compaction.threshold_ratio);
        const auto by_headroom = window_ - config_.max_output_tokens - config_.compaction.headroom_tokens;
        return std::min(by_ratio, by_headroom);
    }

    int Compactor::retain_budget() const {
        return static_cast<int>(static_cast<double>(window_) * config_.compaction.retain_ratio);
    }

    bool Compactor::should_compact(const json_type &messages) const {
        if (!config_.compaction.auto_compact) return false;
        if (messages.size() < 4) return false;
        return meter_.estimate_messages(messages) >= limit();
    }

    bool Compactor::prune() {
        std::set<int> already;
        std::vector<std::pair<int, json_type>> pending;
        for (const auto &event: session_.events()) {
            if (event.value("type", std::string{}) == "prune" &&
                event.contains("target_seq") && event["target_seq"].is_number_integer()) {
                already.insert(event["target_seq"].get<int>());
            }
        }
        for (const auto &event: session_.events()) {
            if (event.value("type", std::string{}) != "tool_result") continue;
            const int seq = event.value("seq", 0);
            if (seq == 0 || already.contains(seq)) continue;
            if (!event.contains("content") || !event["content"].is_array()) continue;
            json_type replacement = json_type::array();
            bool touched = false;
            for (const auto &block: event["content"]) {
                if (!block.is_object() || block.value("type", std::string{}) != "text") {
                    replacement.push_back(block);
                    continue;
                }
                auto text = block.value("text", std::string{});
                const auto limit = static_cast<std::size_t>(config_.compaction.prune_tool_chars);
                if (text.size() > limit) {
                    const auto head = limit * 3 / 4;
                    const auto tail = limit - head;
                    text = text.substr(0, head) + "\n… pruned " +
                           std::to_string(text.size() - head - tail) + " chars …\n" +
                           text.substr(text.size() - tail);
                    touched = true;
                }
                replacement.push_back({{"type", "text"}, {"text", text}});
            }
            if (touched) pending.emplace_back(seq, std::move(replacement));
        }
        for (const auto &[seq, content]: pending) session_.add_prune(seq, content);
        return !pending.empty();
    }

    std::optional<Outcome> Compactor::compact(llm::Client &client, http::Cancel &cancel,
                                              const std::string &system, const json_type &tools,
                                              const bool force) {
        auto projection = session_.project_with_sources();
        const auto before = meter_.estimate_messages(projection.messages);
        if (!force && before < limit()) return std::nullopt;

        const auto pruned = prune();
        int after_prune = before;
        if (pruned) {
            projection = session_.project_with_sources();
            after_prune = meter_.estimate_messages(projection.messages);
            if (!force && after_prune < limit()) {
                return Outcome{"", 0, 0, before, after_prune, {}, session_.model()};
            }
        }

        const auto boundary = choose_boundary(projection.messages, retain_budget(), meter_);
        if (boundary < 0) return std::nullopt;

        json_type head = json_type::array();
        for (int index = 0; index < boundary; ++index) head.push_back(projection.messages[index]);
        head.push_back({
            {"role", "user"},
            {"content", json_type::array({{{"type", "text"}, {"text", summary_instruction}}})},
        });

        llm::Request request;
        request.model = session_.model();
        request.system = system;
        request.messages = std::move(head);
        request.max_tokens = config_.compaction.max_summary_tokens;
        request.effort = "off";
        request.tools = tools;
        request.tool_choice = {{"type", "none"}};

        llm::MessageAccumulator accumulator;
        client.stream(request, [&accumulator](const llm::StreamEvent &event) {
            accumulator.handle(event);
        }, cancel);
        const auto message = accumulator.message();
        auto summary = message.text();
        while (!summary.empty() && std::isspace(static_cast<unsigned char>(summary.front()))) summary.erase(0, 1);
        while (!summary.empty() && std::isspace(static_cast<unsigned char>(summary.back()))) summary.pop_back();
        if (summary.empty()) return std::nullopt;

        const int keep_from_seq = projection.sources[boundary].first;
        const int through_seq = projection.sources[boundary - 1].second;
        json_type tail = json_type::array();
        tail.push_back(summary_message(summary));
        for (int index = boundary; index < static_cast<int>(projection.messages.size()); ++index) {
            tail.push_back(projection.messages[index]);
        }
        const auto after = meter_.estimate_messages(tail);
        const auto model = message.model.empty() ? session_.model() : message.model;
        session_.add_compaction(summary, keep_from_seq, through_seq, before, after,
                                message.usage.to_json(), model);
        return Outcome{summary, keep_from_seq, through_seq, before, after, message.usage, model};
    }

}
