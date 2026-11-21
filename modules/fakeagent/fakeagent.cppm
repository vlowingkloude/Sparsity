module;

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

export module fakeagent;

import json;
import boost_wrapper;

namespace sparsity::fakeagent {

    using WebSocket = boost_wrapper::WebSocket;
    using json_type = json::json_type;

    struct FakeState {
        std::mutex mutex;
        std::condition_variable cv;
        std::optional<std::string> decision;
        std::atomic_bool interrupted{false};
        std::atomic_bool running{false};
        std::string model{"deepseek-flash"};
        std::string effort{"high"};
        std::string permissions{"ask"};
    };

    void send_json(WebSocket &socket, std::mutex &write_mutex, const json_type &value) {
        std::lock_guard lock(write_mutex);
        try {
            boost_wrapper::write_websocket_text(socket, value.dump());
        } catch (...) {
            // The client went away; the turn loop will notice on its next write.
        }
    }

    void send_state(WebSocket &socket, std::mutex &write_mutex, FakeState &state, const bool busy) {
        std::string model;
        std::string effort;
        std::string permissions;
        {
            std::lock_guard lock(state.mutex);
            model = state.model;
            effort = state.effort;
            permissions = state.permissions;
        }
        send_json(socket, write_mutex, {
                      {"type", "state"},
                      {"model", model},
                      {"effort", effort},
                      {"permissions", permissions},
                      {"busy", busy},
                      {"ctx_tokens", 1200},
                      {"cost", 0.0007},
                  });
    }

    std::size_t utf8_next(const std::string_view text, const std::size_t index) {
        if (index >= text.size()) {
            return text.size();
        }
        const auto first = static_cast<unsigned char>(text[index]);
        std::size_t length = 1;
        if ((first & 0x80U) == 0) {
            length = 1;
        } else if ((first & 0xe0U) == 0xc0U) {
            length = 2;
        } else if ((first & 0xf0U) == 0xe0U) {
            length = 3;
        } else if ((first & 0xf8U) == 0xf0U) {
            length = 4;
        }
        return std::min(index + length, text.size());
    }

    bool stream_text(WebSocket &socket, std::mutex &write_mutex, FakeState &state,
                     const std::string_view kind, const int index, const std::string_view text) {
        std::size_t start = 0;
        while (start < text.size()) {
            if (state.interrupted.load(std::memory_order_relaxed)) {
                return false;
            }
            std::size_t end = start;
            for (int count = 0; count < 3 && end < text.size(); ++count) {
                end = utf8_next(text, end);
            }
            send_json(socket, write_mutex, {
                          {"type", kind},
                          {"index", index},
                          {"text", text.substr(start, end - start)},
                      });
            start = end;
            std::this_thread::sleep_for(std::chrono::milliseconds(35));
        }
        return true;
    }

    void finish_turn(WebSocket &socket, std::mutex &write_mutex, FakeState &state,
                     const std::string_view stop_reason) {
        send_json(socket, write_mutex, {
                      {"type", "turn_end"},
                      {"stop_reason", stop_reason},
                      {"usage", {
                          {"input_tokens", 1200},
                          {"output_tokens", 120},
                          {"cache_read_input_tokens", 0},
                          {"cache_creation_input_tokens", 0},
                      }},
                      {"cost", 0.0007},
                  });
        state.running.store(false, std::memory_order_relaxed);
        send_state(socket, write_mutex, state, false);
    }

    void run_turn(WebSocket &socket, std::mutex &write_mutex, FakeState &state,
                  const std::string &project_id, const json_type &prompt) {
        (void) project_id;
        (void) prompt;

        send_json(socket, write_mutex, {{"type", "turn_start"}});

        if (!stream_text(socket, write_mutex, state, "thinking", 0,
                         "先看一下当前 project 的结构和活动文件，再做一个最小改动。")) {
            finish_turn(socket, write_mutex, state, "aborted");
            return;
        }
        if (!stream_text(socket, write_mutex, state, "text", 1,
                         "我找到了入口文件。先读一下，然后改一行再跑测试。")) {
            finish_turn(socket, write_mutex, state, "aborted");
            return;
        }

        send_json(socket, write_mutex, {
                      {"type", "tool_call"},
                      {"id", "fake-read"},
                      {"name", "read"},
                      {"summary", "src/app/main.cpp"},
                      {"detail", ""},
                  });

        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        if (state.interrupted.load(std::memory_order_relaxed)) {
            finish_turn(socket, write_mutex, state, "aborted");
            return;
        }

        send_json(socket, write_mutex, {
                      {"type", "tool_result"},
                      {"id", "fake-read"},
                      {"text", "1| int main() {\n2|     return 0;\n3| }\n"},
                      {"is_error", false},
                      {"display", "3 lines"},
                  });

        const json_type diff = {
            {"path", "src/app/main.cpp"},
            {"before", "int main() {\n    return 0;\n}\n"},
            {"after", "int main() {\n    return 1;\n}\n"},
            {"added", 1},
            {"removed", 1},
            {"truncated", false},
        };

        send_json(socket, write_mutex, {
                      {"type", "tool_call"},
                      {"id", "fake-edit"},
                      {"name", "edit"},
                      {"summary", "src/app/main.cpp"},
                      {"detail", "return 0 -> return 1"},
                      {"diff", diff},
                  });

        send_json(socket, write_mutex, {
                      {"type", "approval"},
                      {"id", "fake-edit"},
                      {"card", {
                          {"tool", "edit"},
                          {"summary", "src/app/main.cpp"},
                          {"detail", "return 0 -> return 1"},
                          {"editable", nullptr},
                          {"danger", nullptr},
                          {"outside_project", false},
                          {"diff", diff},
                      }},
                  });

        std::string action;
        bool interrupted = false;
        {
            std::unique_lock lock(state.mutex);
            state.cv.wait(lock, [&state] {
                return state.decision.has_value() || state.interrupted.load(std::memory_order_relaxed);
            });
            interrupted = state.interrupted.load(std::memory_order_relaxed);
            if (!interrupted) {
                action = state.decision.value_or("deny");
                state.decision.reset();
            }
        }
        if (interrupted) {
            finish_turn(socket, write_mutex, state, "aborted");
            return;
        }

        if (action == "deny") {
            send_json(socket, write_mutex, {
                          {"type", "tool_result"},
                          {"id", "fake-edit"},
                          {"text", "用户拒绝了这次修改。"},
                          {"is_error", true},
                          {"display", "已拒绝"},
                      });
            stream_text(socket, write_mutex, state, "text", 2, "好的，那我不动它。需要我换个方案吗？");
            finish_turn(socket, write_mutex, state, "end_turn");
            return;
        }

        send_json(socket, write_mutex, {
                      {"type", "tool_result"},
                      {"id", "fake-edit"},
                      {"text", "已修改 src/app/main.cpp：1 行新增，1 行删除。"},
                      {"is_error", false},
                      {"display", "1 file changed"},
                  });
        stream_text(socket, write_mutex, state, "text", 2,
                    "改好了。这个 fake agent 只用于 P0 验证 UI，真正的 C++ harness 会在 P1 接上。");
        finish_turn(socket, write_mutex, state, "end_turn");
    }

}

export namespace sparsity::fakeagent {

    void run(WebSocket &socket, std::string project_id) {
        std::mutex write_mutex;
        FakeState state;
        std::thread worker;

        send_json(socket, write_mutex, {
                      {"type", "ready"},
                      {"project", project_id},
                      {"session", "fake-session"},
                      {"model", "deepseek-flash"},
                      {"effort", "high"},
                      {"permissions", "ask"},
                      {"status", "idle"},
                  });
        send_json(socket, write_mutex, {{"type", "history"}, {"events", json::json_type::array()}});
        send_state(socket, write_mutex, state, false);

        try {
            while (true) {
                const auto message = boost_wrapper::read_websocket(socket);
                if (message.payload.empty()) {
                    continue;
                }

                json_type value;
                try {
                    value = json_type::parse(message.payload);
                } catch (...) {
                    continue;
                }

                const auto type = value.value("type", std::string{});
                if (type == "prompt") {
                    if (state.running.exchange(true, std::memory_order_relaxed)) {
                        send_json(socket, write_mutex, {
                                      {"type", "error"},
                                      {"kind", "busy"},
                                      {"message", "这一轮还没结束。"},
                                  });
                        continue;
                    }
                    state.interrupted.store(false, std::memory_order_relaxed);
                    {
                        std::lock_guard lock(state.mutex);
                        state.decision.reset();
                    }
                    if (worker.joinable()) {
                        worker.join();
                    }
                    worker = std::thread(run_turn, std::ref(socket), std::ref(write_mutex),
                                         std::ref(state), project_id, value);
                } else if (type == "decision") {
                    {
                        std::lock_guard lock(state.mutex);
                        state.decision = value.value("action", std::string{"deny"});
                    }
                    state.cv.notify_all();
                } else if (type == "interrupt") {
                    state.interrupted.store(true, std::memory_order_relaxed);
                    state.cv.notify_all();
                } else if (type == "settings") {
                    {
                        std::lock_guard lock(state.mutex);
                        if (value.contains("model") && value["model"].is_string()) {
                            state.model = value["model"].get<std::string>();
                        }
                        if (value.contains("effort") && value["effort"].is_string()) {
                            state.effort = value["effort"].get<std::string>();
                        }
                        if (value.contains("permissions") && value["permissions"].is_string()) {
                            state.permissions = value["permissions"].get<std::string>();
                        }
                    }
                    send_state(socket, write_mutex, state, state.running.load(std::memory_order_relaxed));
                }
            }
        } catch (...) {
            // Connection closed or read failed; fall through to cleanup.
        }

        state.interrupted.store(true, std::memory_order_relaxed);
        state.cv.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
    }

}
