module;

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

export module session;

import json;
import llm;
import pricing;

export namespace sparsity::session {

    using json_type = json::json_type;

    struct SessionMeta {
        std::string id;
        std::string project;
        std::string title;
        int messages{0};
        double updated{0.0};
    };

    struct Projection {
        json_type messages;
        std::vector<std::pair<int, int>> sources;
    };

    json_type now_iso();
    std::string new_session_id();

    class Session {
    public:
        Session(std::filesystem::path path, std::string id, std::string project, std::string cwd,
                std::string model, std::string effort);

        const std::string &id() const { return id_; }
        const std::string &project() const { return project_; }
        const std::string &cwd() const { return cwd_; }
        const std::string &model() const { return model_; }
        const std::string &effort() const { return effort_; }
        const std::filesystem::path &path() const { return path_; }
        const std::vector<json_type> &events() const { return events_; }

        std::string title() const;
        int message_count() const;
        double cost() const;
        llm::Usage last_usage() const;

        void add_user(const std::string &text, const json_type &attachments = json_type::array());
        void add_assistant(const llm::AssistantMessage &message);
        void add_tool_result(const std::string &tool_use_id, const json_type &content, bool is_error);
        void add_switch(const std::string &model, const std::string &effort);
        void add_cwd(const std::string &path);
        void add_todos(const json_type &items);
        void add_title(const std::string &title);
        void add_abort(const std::string &reason);
        void add_compaction(const std::string &summary, int keep_from_seq, int through_seq,
                            int before, int after, const json_type &usage, const std::string &model);
        void add_prune(int target_seq, const json_type &content);

        json_type messages() const;
        Projection project_with_sources() const;
        json_type meta_json() const;
        void restore(std::vector<json_type> events);

    private:
        void append(const std::string &kind, json_type fields);

        std::filesystem::path path_;
        std::string id_;
        std::string project_;
        std::string cwd_;
        std::string model_;
        std::string effort_;
        std::vector<json_type> events_;
    };

    class Store {
    public:
        explicit Store(std::filesystem::path home);

        std::shared_ptr<Session> create(const std::string &project, const std::string &cwd,
                                        const std::string &model, const std::string &effort);
        std::shared_ptr<Session> load(const std::string &project, const std::string &id) const;
        std::shared_ptr<Session> latest(const std::string &project) const;
        std::vector<SessionMeta> list(const std::string &project) const;
        bool remove(const std::string &project, const std::string &id) const;
        std::filesystem::path directory(const std::string &project) const;

    private:
        std::filesystem::path home_;
    };

}

namespace sparsity::session {

    std::string timestamp(const std::string_view format) {
        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);
        std::tm utc{};
        gmtime_r(&time, &utc);
        char buffer[32];
        std::strftime(buffer, sizeof(buffer), std::string(format).c_str(), &utc);
        return buffer;
    }

    std::string text_of(const json_type &event) {
        if (!event.contains("content") || !event["content"].is_array()) return {};
        std::string result;
        for (const auto &block: event["content"]) {
            if (block.is_object() && block.value("type", std::string{}) == "text") {
                result += block.value("text", std::string{});
            }
        }
        return result;
    }

    bool metadata(const std::string &type) {
        return type == "cwd" || type == "abort" || type == "switch" || type == "todos" ||
               type == "title" || type == "prune";
    }

    int seq_of(const json_type &event) {
        if (!event.contains("seq") || !event["seq"].is_number_integer()) return 0;
        return event["seq"].get<int>();
    }

    json_type summary_message(const std::string &summary) {
        return {
            {"role", "user"},
            {"content", json_type::array({
                {{"type", "text"}, {"text", "<conversation_summary>\n" + summary + "\n</conversation_summary>"}}
            })},
        };
    }

    std::string base64(const std::vector<unsigned char> &data) {
        static constexpr char alphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string result;
        result.reserve((data.size() + 2) / 3 * 4);
        for (std::size_t index = 0; index < data.size(); index += 3) {
            const auto a = data[index];
            const auto b = index + 1 < data.size() ? data[index + 1] : 0;
            const auto c = index + 2 < data.size() ? data[index + 2] : 0;
            const auto value = (static_cast<std::uint32_t>(a) << 16) |
                               (static_cast<std::uint32_t>(b) << 8) | c;
            result.push_back(alphabet[(value >> 18) & 63]);
            result.push_back(alphabet[(value >> 12) & 63]);
            result.push_back(index + 1 < data.size() ? alphabet[(value >> 6) & 63] : '=');
            result.push_back(index + 2 < data.size() ? alphabet[value & 63] : '=');
        }
        return result;
    }

    json_type expand_blocks(const json_type &blocks) {
        if (!blocks.is_array()) return blocks;
        json_type result = json_type::array();
        for (const auto &block: blocks) {
            if (!block.is_object()) {
                result.push_back(block);
                continue;
            }
            const auto type = block.value("type", std::string{});
            if (type == "image_ref") {
                const auto path = block.value("path", std::string{});
                std::ifstream input(path, std::ios::binary);
                if (!input) {
                    result.push_back({{"type", "text"}, {"text", "[image unavailable: " + path + "]"}});
                    continue;
                }
                std::vector<unsigned char> data{std::istreambuf_iterator<char>(input),
                                                std::istreambuf_iterator<char>()};
                result.push_back({
                    {"type", "image"},
                    {"source", {
                        {"type", "base64"},
                        {"media_type", block.value("media_type", std::string{"image/jpeg"})},
                        {"data", base64(data)},
                    }},
                });
                continue;
            }
            if (type == "tool_result") {
                auto copy = block;
                copy["content"] = expand_blocks(block.value("content", json_type::array()));
                result.push_back(std::move(copy));
                continue;
            }
            result.push_back(block);
        }
        return result;
    }

    bool plain_user(const json_type &message) {
        if (!message.contains("content") || !message["content"].is_array()) return true;
        for (const auto &block: message["content"]) {
            if (block.is_object() && block.value("type", std::string{}) == "tool_result") return false;
        }
        return true;
    }

    json_type ensure_results(const json_type &content, json_type results) {
        std::vector<std::string> seen;
        for (const auto &item: results) {
            if (item.is_object() && item.contains("tool_use_id") && item["tool_use_id"].is_string()) {
                seen.push_back(item["tool_use_id"].get<std::string>());
            }
        }
        for (const auto &block: content) {
            if (!block.is_object() || block.value("type", std::string{}) != "tool_use") continue;
            const auto id = block.value("id", std::string{});
            if (id.empty() || std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
            results.push_back({
                {"type", "tool_result"},
                {"tool_use_id", id},
                {"content", json_type::array({
                    {{"type", "text"}, {"text", "this tool call did not complete"}}
                })},
                {"is_error", true},
            });
        }
        return results;
    }

}

export namespace sparsity::session {

    json_type now_iso() {
        return timestamp("%Y-%m-%dT%H:%M:%SZ");
    }

    std::string new_session_id() {
        static std::random_device device;
        static std::mt19937 engine(device());
        std::uniform_int_distribution<int> distribution(0, 0xffffff);
        char suffix[8];
        std::snprintf(suffix, sizeof(suffix), "%06x", distribution(engine));
        return timestamp("%Y%m%d-%H%M%S") + "-" + suffix;
    }

    Session::Session(std::filesystem::path path, std::string id, std::string project, std::string cwd,
                     std::string model, std::string effort)
        : path_(std::move(path)), id_(std::move(id)), project_(std::move(project)),
          cwd_(std::move(cwd)), model_(std::move(model)), effort_(std::move(effort)) {
        events_.push_back({
            {"v", 1},
            {"seq", 1},
            {"ts", now_iso()},
            {"type", "session"},
            {"id", id_},
            {"project", project_},
            {"cwd", cwd_},
            {"model", model_},
            {"effort", effort_},
        });
    }

    void Session::append(const std::string &kind, json_type fields) {
        json_type event = {
            {"v", 1},
            {"seq", static_cast<int>(events_.size()) + 1},
            {"ts", now_iso()},
            {"type", kind},
        };
        for (auto it = fields.begin(); it != fields.end(); ++it) event[it.key()] = it.value();
        const auto line = event.dump(-1, ' ', false, json_type::error_handler_t::replace) + "\n";
        const int descriptor = ::open(path_.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0600);
        if (descriptor < 0) throw std::runtime_error("cannot append session: " + path_.string());
        const auto written = ::write(descriptor, line.data(), line.size());
        ::fsync(descriptor);
        ::close(descriptor);
        if (written != static_cast<ssize_t>(line.size())) throw std::runtime_error("short session write");
        events_.push_back(std::move(event));
    }

    std::string Session::title() const {
        for (auto it = events_.rbegin(); it != events_.rend(); ++it) {
            if (it->value("type", std::string{}) == "title") return it->value("title", std::string{});
        }
        for (const auto &event: events_) {
            if (event.value("type", std::string{}) != "user") continue;
            const auto text = text_of(event);
            if (text.empty()) continue;
            const auto newline = text.find('\n');
            auto title = text.substr(0, newline);
            if (title.size() > 60) title = title.substr(0, 60) + "…";
            return title;
        }
        return "(untitled)";
    }

    int Session::message_count() const {
        int count = 0;
        for (const auto &event: events_) {
            const auto type = event.value("type", std::string{});
            if (type == "user" || type == "assistant") ++count;
        }
        return count;
    }

    double Session::cost() const {
        double total = 0.0;
        for (const auto &event: events_) {
            const auto type = event.value("type", std::string{});
            if (type != "assistant" && type != "compaction") continue;
            llm::Usage usage;
            if (event.contains("usage")) usage.update(event["usage"]);
            total += pricing::estimate_cost(usage, event.value("model", model_));
        }
        return total;
    }

    void Session::add_user(const std::string &text, const json_type &attachments) {
        json_type content = json_type::array({{{"type", "text"}, {"text", text}}});
        if (attachments.is_array()) {
            for (const auto &attachment: attachments) content.push_back(attachment);
        }
        append("user", {{"content", std::move(content)}});
    }

    void Session::add_assistant(const llm::AssistantMessage &message) {
        append("assistant", {
            {"content", message.content},
            {"stop_reason", message.stop_reason.value_or("")},
            {"usage", message.usage.to_json()},
            {"model", message.model.empty() ? model_ : message.model},
            {"interrupted", message.interrupted},
        });
    }

    void Session::add_tool_result(const std::string &tool_use_id, const json_type &content, const bool is_error) {
        append("tool_result", {
            {"tool_use_id", tool_use_id},
            {"content", content},
            {"is_error", is_error},
        });
    }

    void Session::add_switch(const std::string &model, const std::string &effort) {
        if (!model.empty()) model_ = model;
        if (!effort.empty()) effort_ = effort;
        append("switch", {{"model", model_}, {"effort", effort_}});
    }

    void Session::add_cwd(const std::string &path) {
        cwd_ = path;
        append("cwd", {{"path", path}});
    }

    void Session::add_todos(const json_type &items) {
        append("todos", {{"items", items}});
    }

    void Session::add_title(const std::string &title) {
        append("title", {{"title", title}});
    }

    void Session::add_abort(const std::string &reason) {
        append("abort", {{"reason", reason}});
    }

    void Session::add_compaction(const std::string &summary, const int keep_from_seq, const int through_seq,
                                 const int before, const int after, const json_type &usage,
                                 const std::string &model) {
        append("compaction", {
            {"summary", summary},
            {"keep_from_seq", keep_from_seq},
            {"through_seq", through_seq},
            {"before", before},
            {"after", after},
            {"usage", usage},
            {"model", model},
        });
    }

    void Session::add_prune(const int target_seq, const json_type &content) {
        append("prune", {{"target_seq", target_seq}, {"content", content}});
    }

    json_type Session::messages() const {
        return project_with_sources().messages;
    }

    Projection Session::project_with_sources() const {
        std::map<int, json_type> prune;
        for (const auto &event: events_) {
            if (event.value("type", std::string{}) != "prune") continue;
            if (event.contains("target_seq") && event["target_seq"].is_number_integer()) {
                prune[event["target_seq"].get<int>()] = event.value("content", json_type::array());
            }
        }

        const json_type *compaction = nullptr;
        for (auto it = events_.rbegin(); it != events_.rend(); ++it) {
            if (it->value("type", std::string{}) == "compaction") {
                compaction = &*it;
                break;
            }
        }

        json_type messages = json_type::array();
        std::vector<std::pair<int, int>> sources;
        std::vector<const json_type *> body;
        if (compaction != nullptr) {
            const auto start_seq = compaction->value("keep_from_seq", 0);
            messages.push_back(summary_message(compaction->value("summary", std::string{})));
            sources.emplace_back(start_seq, start_seq);
            for (const auto &event: events_) {
                if (seq_of(event) >= start_seq && event.value("type", std::string{}) != "compaction") {
                    body.push_back(&event);
                }
            }
        } else {
            for (const auto &event: events_) body.push_back(&event);
        }

        std::size_t index = 0;
        while (index < body.size()) {
            const auto &event = *body[index];
            const auto type = event.value("type", std::string{});
            if (type == "user") {
                const auto content = event.value("content", json_type::array());
                if (!content.empty()) {
                    const auto seq = seq_of(event);
                    if (!messages.empty() && messages.back().value("role", std::string{}) == "user" &&
                        plain_user(messages.back())) {
                        for (const auto &block: content) messages.back()["content"].push_back(block);
                        sources.back().second = seq;
                    } else {
                        messages.push_back({{"role", "user"}, {"content", expand_blocks(content)}});
                        sources.emplace_back(seq, seq);
                    }
                }
                ++index;
                continue;
            }
            if (type == "assistant") {
                const auto content = event.value("content", json_type::array());
                if (content.empty()) {
                    ++index;
                    continue;
                }
                const auto seq = seq_of(event);
                messages.push_back({{"role", "assistant"}, {"content", content}});
                sources.emplace_back(seq, seq);

                json_type results = json_type::array();
                std::size_t cursor = index + 1;
                int first_seq = 0;
                int last_seq = 0;
                while (cursor < body.size()) {
                    const auto &next = *body[cursor];
                    const auto next_type = next.value("type", std::string{});
                    if (next_type == "tool_result") {
                        const auto current_seq = seq_of(next);
                        json_type result_content = next.value("content", json_type::array());
                        if (const auto found = prune.find(current_seq); found != prune.end()) {
                            result_content = found->second;
                        }
                        results.push_back({
                            {"type", "tool_result"},
                            {"tool_use_id", next.value("tool_use_id", std::string{})},
                            {"content", expand_blocks(result_content)},
                            {"is_error", next.value("is_error", false)},
                        });
                        if (first_seq == 0) first_seq = current_seq;
                        last_seq = current_seq;
                        ++cursor;
                        continue;
                    }
                    if (metadata(next_type)) {
                        ++cursor;
                        continue;
                    }
                    break;
                }
                results = ensure_results(content, std::move(results));
                if (!results.empty()) {
                    messages.push_back({{"role", "user"}, {"content", results}});
                    sources.emplace_back(first_seq == 0 ? seq : first_seq, last_seq == 0 ? seq : last_seq);
                }
                index = cursor;
                continue;
            }
            ++index;
        }
        return {std::move(messages), std::move(sources)};
    }

    void Session::restore(std::vector<json_type> events) {
        events_ = std::move(events);
        for (const auto &event: events_) {
            const auto type = event.value("type", std::string{});
            if (type == "switch") {
                if (event.contains("model") && event["model"].is_string()) model_ = event["model"].get<std::string>();
                if (event.contains("effort") && event["effort"].is_string()) effort_ = event["effort"].get<std::string>();
            } else if (type == "cwd" && event.contains("path") && event["path"].is_string()) {
                cwd_ = event["path"].get<std::string>();
            }
        }
    }

    llm::Usage Session::last_usage() const {
        for (auto event = events_.rbegin(); event != events_.rend(); ++event) {
            const auto type = event->value("type", std::string{});
            if (type != "assistant" && type != "compaction") continue;
            llm::Usage usage;
            if (event->contains("usage")) usage.update((*event)["usage"]);
            return usage;
        }
        return {};
    }

    json_type Session::meta_json() const {
        return {
            {"id", id_},
            {"project", project_},
            {"title", title()},
            {"messages", message_count()},
            {"model", model_},
            {"effort", effort_},
        };
    }

    Store::Store(std::filesystem::path home) : home_(std::move(home)) {
    }

    std::filesystem::path Store::directory(const std::string &project) const {
        return home_ / "projects" / project / "sessions";
    }

    std::shared_ptr<Session> Store::create(const std::string &project, const std::string &cwd,
                                           const std::string &model, const std::string &effort) {
        const auto directory = this->directory(project);
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) throw std::runtime_error("cannot create session directory");
        const auto id = new_session_id();
        auto value = std::make_shared<Session>(directory / (id + ".jsonl"), id, project, cwd, model, effort);
        const auto header = value->events().front().dump(-1, ' ', false, json_type::error_handler_t::replace) + "\n";
        const int descriptor = ::open(value->path().c_str(), O_WRONLY | O_APPEND | O_CREAT, 0600);
        if (descriptor < 0) throw std::runtime_error("cannot create session file");
        ::write(descriptor, header.data(), header.size());
        ::fsync(descriptor);
        ::close(descriptor);
        return value;
    }

    std::shared_ptr<Session> Store::load(const std::string &project, const std::string &id) const {
        const auto path = directory(project) / (id + ".jsonl");
        std::ifstream input(path);
        if (!input) return nullptr;
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty()) lines.push_back(line);
        }
        if (lines.empty()) return nullptr;
        json_type header;
        try {
            header = json_type::parse(lines.front());
        } catch (...) {
            return nullptr;
        }
        if (header.value("type", std::string{}) != "session") return nullptr;

        auto value = std::make_shared<Session>(path, header.value("id", id), project,
                                               header.value("cwd", std::string{}),
                                               header.value("model", std::string{"deepseek-flash"}),
                                               header.value("effort", std::string{"high"}));
        std::vector<json_type> events;
        for (const auto &raw: lines) {
            try {
                events.push_back(json_type::parse(raw));
            } catch (...) {
                continue;
            }
        }
        value->restore(std::move(events));
        return value;
    }

    std::shared_ptr<Session> Store::latest(const std::string &project) const {
        std::error_code error;
        const auto directory = this->directory(project);
        if (!std::filesystem::is_directory(directory, error)) return nullptr;
        std::filesystem::path newest;
        std::filesystem::file_time_type newest_time{};
        for (const auto &entry: std::filesystem::directory_iterator(directory, error)) {
            if (!entry.is_regular_file()) continue;
            const auto time = entry.last_write_time();
            if (newest.empty() || time > newest_time) {
                newest = entry.path();
                newest_time = time;
            }
        }
        if (newest.empty()) return nullptr;
        return load(project, newest.stem().string());
    }

    bool Store::remove(const std::string &project, const std::string &id) const {
        if (id.empty()) return false;
        for (const char character: id) {
            if (!std::isalnum(static_cast<unsigned char>(character)) && character != '-' && character != '_') {
                return false;
            }
        }
        std::error_code error;
        return std::filesystem::remove(directory(project) / (id + ".jsonl"), error);
    }

    std::vector<SessionMeta> Store::list(const std::string &project) const {
        std::vector<SessionMeta> result;
        std::error_code error;
        const auto directory = this->directory(project);
        if (!std::filesystem::is_directory(directory, error)) return result;
        for (const auto &entry: std::filesystem::directory_iterator(directory, error)) {
            if (!entry.is_regular_file()) continue;
            auto value = load(project, entry.path().stem().string());
            if (!value) continue;
            const auto time = std::chrono::duration_cast<std::chrono::seconds>(
                entry.last_write_time().time_since_epoch()).count();
            result.push_back({
                value->id(),
                project,
                value->title(),
                value->message_count(),
                static_cast<double>(time),
            });
        }
        std::ranges::sort(result, [](const SessionMeta &left, const SessionMeta &right) {
            return left.updated > right.updated;
        });
        return result;
    }

}
