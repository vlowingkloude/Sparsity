#include <algorithm>
#include <atomic>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>

import server;
import files;
import config;
import models;

#ifndef SPARSITY_DEFAULT_STATIC_DIRECTORY
#define SPARSITY_DEFAULT_STATIC_DIRECTORY "frontend/dist"
#endif

namespace {
    struct Args {
        std::filesystem::path path;
        std::string host{"127.0.0.1"};
        std::uint16_t port{1377};
        std::filesystem::path static_directory{SPARSITY_DEFAULT_STATIC_DIRECTORY};
        std::string model;
        std::string effort;
        bool fake_agent{false};
        bool reset_api_key{false};
    };

    std::atomic_bool stopping{false};

    void stop(int) {
        stopping.store(true, std::memory_order_relaxed);
    }

    void print_usage() {
        std::cerr << "Usage: sparsity [PLAYGROUND] [--host HOST] [--port PORT] [--static-dir DIR]\n"
                     "                [--model ID] [--effort LEVEL] [--fake-agent] [--reset-api-key]\n";
    }

    Args parse_arguments(int argc, char **argv) {
        Args result;
        for (int index = 1; index < argc; ++index) {
            std::string argument(argv[index]);
            const auto equals = argument.find('=');
            const auto name = argument.substr(0, equals);
            const bool inline_value = equals != std::string::npos;
            auto value = inline_value ? argument.substr(equals + 1) : std::string{};
            if (name == "-h" || name == "--help") {
                print_usage();
                std::exit(0);
            }
            if (name == "--fake-agent") {
                result.fake_agent = true;
                continue;
            }
            if (name == "--reset-api-key") {
                result.reset_api_key = true;
                continue;
            }
            if (name == "--host" || name == "--port" || name == "--static-dir" ||
                name == "--model" || name == "--effort") {
                if (!inline_value) {
                    if (++index >= argc) throw std::runtime_error(name + " requires a value");
                    value = argv[index];
                }
                if (name == "--host") {
                    result.host = value;
                } else if (name == "--static-dir") {
                    result.static_directory = value;
                } else if (name == "--model") {
                    result.model = value;
                } else if (name == "--effort") {
                    result.effort = value;
                } else {
                    unsigned int parsed = 0;
                    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
                    if (error != std::errc{} || end != value.data() + value.size() ||
                        parsed == 0 || parsed > 65535) {
                        throw std::runtime_error("invalid port: " + value);
                    }
                    result.port = static_cast<std::uint16_t>(parsed);
                }
                continue;
            }
            if (!argument.empty() && argument.front() == '-') {
                throw std::runtime_error("unknown option: " + argument);
            }
            if (!result.path.empty()) throw std::runtime_error("only one playground can be opened");
            result.path = argument;
        }
        return result;
    }
}

int main(int argc, char **argv) {
    try {
        const auto args = parse_arguments(argc, argv);
        const auto playground_path = args.path.empty() ? std::filesystem::current_path() : args.path;
        auto playground = sparsity::files::open_workspace(playground_path);
        if (playground.kind != "folder") {
            throw std::runtime_error("playground must be a directory: " + playground_path.string());
        }

        const auto home = sparsity::config::home_directory() / ".sparsity";
        sparsity::config::ensure_home(home);
        auto cfg = sparsity::config::load(playground.root, home);
        if (!args.model.empty()) cfg.model = args.model;
        if (!args.effort.empty()) cfg.effort = args.effort;

        auto api_key = sparsity::config::resolve_api_key(home);
        if (args.reset_api_key) {
            api_key = sparsity::config::read_secret("DeepSeek API key: ");
            if (api_key.empty()) throw std::runtime_error("empty API key");
            sparsity::config::save_api_key(home, api_key);
        }
        if (!args.fake_agent && api_key.empty()) {
            if (::isatty(STDIN_FILENO)) {
                api_key = sparsity::config::read_secret("DeepSeek API key: ");
                if (api_key.empty()) throw std::runtime_error("empty API key");
                sparsity::config::save_api_key(home, api_key);
            } else {
                throw std::runtime_error("no DeepSeek API key; set DEEPSEEK_API_KEY or run with a TTY");
            }
        }

        sparsity::models::Catalog catalog = sparsity::models::load_cache(home / "models.json");
        if (!api_key.empty() && !args.fake_agent) {
            try {
                catalog = sparsity::models::fetch(cfg.api_root, api_key);
                sparsity::models::save_cache(home / "models.json", catalog);
            } catch (const std::exception &error) {
                std::cerr << "model catalog: " << error.what() << '\n';
            }
        }
        if (!catalog.models.empty() &&
            sparsity::models::find(catalog, cfg.model) == nullptr) {
            cfg.model = catalog.models.front().id;
        }
        if (!catalog.models.empty()) {
            const auto *info = sparsity::models::find(catalog, cfg.model);
            if (info != nullptr && cfg.effort != "off" &&
                std::find(info->effort_levels.begin(), info->effort_levels.end(), cfg.effort) ==
                    info->effort_levels.end()) {
                cfg.effort = info->default_effort;
            }
        }

        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        std::signal(SIGPIPE, SIG_IGN);
        std::clog << "Sparsity is available at http://" << args.host << ':' << args.port
                  << " (playground: " << playground.root << ")\n";

        sparsity::server::Server server(std::move(playground), args.static_directory, std::move(cfg),
                                        std::move(api_key), std::move(catalog), args.fake_agent);
        server.run(args.host, args.port, stopping);
        return 0;
    } catch (const std::exception &caught) {
        std::cerr << caught.what() << '\n';
        print_usage();
        return 2;
    }
}
