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
        double cost{0.0};
        bool hard_trimmed{false};
        bool fits{true};
        int dropped_messages{0};
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
        int limit() const;
        int hard_limit() const;
        int system_tools_tokens(const std::string &system, const json_type &tools) const;
        bool should_compact(const json_type &messages) const;
        bool should_compact(const json_type &messages, const std::string &system,
                            const json_type &tools) const;
        std::optional<Outcome> compact(llm::Client &client, http::Cancel &cancel,
                                       const std::string &system, const json_type &tools, bool force);

    private:
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

    bool user_has_tool_result(const json_type &message) {
        if (!message.contains("content") || !message["content"].is_array()) return false;
        for (const auto &block: message["content"]) {
            if (block.is_object() && block.value("type", std::string{}) == "tool_result") return true;
        }
        return false;
    }

    // Drop one self-contained conversation group from the front of an array.
    // An assistant message carries its following tool-result user messages.
    int drop_front_group(json_type &messages, const int start) {
        if (!messages.is_array() || start < 0 || start >= static_cast<int>(messages.size())) return 0;
        int end = start + 1;
        if (messages[start].value("role", std::string{}) == "assistant") {
            while (end < static_cast<int>(messages.size()) &&
                   messages[end].value("role", std::string{}) == "user" &&
                   user_has_tool_result(messages[end])) {
                ++end;
            }
        }
        messages.erase(messages.begin() + start, messages.begin() + end);
        return end - start;
    }

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
        const auto by_headroom = hard_limit();
        return std::min(by_ratio, by_headroom);
    }

    int Compactor::hard_limit() const {
        return window_ - config_.max_output_tokens - config_.compaction.headroom_tokens;
    }

    int Compactor::system_tools_tokens(const std::string &system, const json_type &tools) const {
        return static_cast<int>(meter_.estimate_text(system) + meter_.estimate_text(tools.dump()));
    }

    int Compactor::retain_budget() const {
        return static_cast<int>(static_cast<double>(window_) * config_.compaction.retain_ratio);
    }

    bool Compactor::should_compact(const json_type &messages) const {
        if (!config_.compaction.auto_compact) return false;
        if (messages.size() < 4) return false;
        return meter_.estimate_messages(messages) >= limit();
    }

    bool Compactor::should_compact(const json_type &messages, const std::string &system,
                                   const json_type &tools) const {
        if (!config_.compaction.auto_compact) return false;
        if (messages.size() < 4) return false;
        return meter_.estimate_messages(messages) + system_tools_tokens(system, tools) >= limit();
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
        const auto system_tools = system_tools_tokens(system, tools);
        if (!force && before + system_tools < limit()) return std::nullopt;

        // Do not prune if there is no safe cut point: that would invalidate
        // the provider prefix without producing a usable summary.
        if (choose_boundary(projection.messages, retain_budget(), meter_) < 0) return std::nullopt;

        // Pruning rewrites earlier tool results and invalidates the provider
        // prefix from that point. Only do it as part of a full compaction so
        // the conversation pays for one cache break, not two.
        if (prune()) projection = session_.project_with_sources();

        const auto boundary = choose_boundary(projection.messages, retain_budget(), meter_);
        if (boundary < 0) return std::nullopt;

        // The summary request itself must fit. Drop the oldest complete groups
        // if a very large early history would otherwise overflow the window.
        json_type head = json_type::array();
        for (int index = 0; index < boundary; ++index) head.push_back(projection.messages[index]);
        int head_dropped = 0;
        const int head_budget = std::max(0, window_ - config_.compaction.max_summary_tokens -
                                               system_tools - 1024);
        while (meter_.estimate_messages(head) > head_budget && !head.empty()) {
            const auto dropped = drop_front_group(head, 0);
            if (dropped == 0) break;
            head_dropped += dropped;
        }
        if (meter_.estimate_messages(head) > head_budget) return std::nullopt;

        std::string instruction = summary_instruction;
        if (head_dropped > 0) {
            instruction = "Some of the oldest messages were omitted to fit the context window.\n\n" + instruction;
        }
        head.push_back({
            {"role", "user"},
            {"content", json_type::array({{{"type", "text"}, {"text", instruction}}})},
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

        const int through_seq = projection.sources[boundary - 1].second;
        json_type tail = json_type::array();
        tail.push_back(summary_message(summary));
        for (int index = boundary; index < static_cast<int>(projection.messages.size()); ++index) {
            tail.push_back(projection.messages[index]);
        }

        // Last-resort deterministic trim: the summary already captures the old
        // head, so drop the oldest retained groups until the next request fits.
        const int target = limit() - system_tools;
        int tail_dropped = 0;
        while (meter_.estimate_messages(tail) > target && tail.size() > 2) {
            const auto dropped = drop_front_group(tail, 1);
            if (dropped == 0) break;
            tail_dropped += dropped;
        }

        const auto after = meter_.estimate_messages(tail);
        const bool fits = after + system_tools <= limit();
        const int body_size = static_cast<int>(projection.messages.size()) - boundary;
        const int keep_from_seq = tail_dropped >= body_size
                                      ? through_seq + 1
                                      : projection.sources[boundary + tail_dropped].first;
        const auto model = message.model.empty() ? session_.model() : message.model;
        const auto hard_trimmed = head_dropped > 0 || tail_dropped > 0;
        const auto cost = session_.add_compaction(summary, keep_from_seq, through_seq, before, after,
                                                  message.usage.to_json(), model, hard_trimmed,
                                                  head_dropped + tail_dropped);
        return Outcome{summary, keep_from_seq, through_seq, before, after, message.usage, model, cost,
                       hard_trimmed, fits, head_dropped + tail_dropped};
    }
}
