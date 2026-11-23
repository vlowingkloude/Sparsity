module;

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <pwd.h>
#include <stdexcept>
#include <string>
#include <termios.h>
#include <toml++/toml.hpp>
#include <unistd.h>
#include <vector>

export module config;

export namespace sparsity::config {

    struct Permissions {
        std::string mode{"workspace"};
    };

    struct Tools {
        int bash_timeout_s{120};
        int tool_output_limit{32000};
    };

    struct Web {
        std::string api_base;
        int search_count{8};
        int fetch_max_chars{30000};
    };

    struct Compaction {
        int context_window{1'048'576};
        double threshold_ratio{0.80};
        double retain_ratio{0.16};
        int headroom_tokens{65536};
        int prune_tool_chars{8192};
        int max_summary_tokens{8192};
        bool auto_compact{true};
    };

    struct Config {
        std::string model{"deepseek-flash"};
        std::string effort{"high"};
        int max_output_tokens{65536};
        int max_steps{0};
        double max_turn_cost{1.0};
        std::string base_url{"https://api.deepseek.com/anthropic"};
        std::string api_root{"https://api.deepseek.com"};
        Permissions permissions;
        Tools tools;
        Web web;
        Compaction compaction;
        std::filesystem::path home;
    };

    std::filesystem::path home_directory();
    std::filesystem::path credentials_path(const std::filesystem::path &home);
    Config load(const std::filesystem::path &playground, const std::filesystem::path &home);
    void ensure_home(const std::filesystem::path &home);
    std::string resolve_api_key(const std::filesystem::path &home);
    void save_api_key(const std::filesystem::path &home, const std::string &key);
    bool remove_api_key(const std::filesystem::path &home);
    std::string read_secret(const std::string &prompt);
    std::string derive_api_root(const std::string &base_url);

}

namespace sparsity::config {

    std::optional<std::string> string_value(const toml::table &global, const toml::table &local,
                                            const std::string &key) {
        if (const auto value = local[key].value<std::string>()) return value;
        if (const auto value = global[key].value<std::string>()) return value;
        return std::nullopt;
    }

    std::optional<int> int_value(const toml::table &global, const toml::table &local,
                                 const std::string &key) {
        if (const auto value = local[key].value<int>()) return value;
        if (const auto value = global[key].value<int>()) return value;
        return std::nullopt;
    }

    std::optional<double> double_value(const toml::table &global, const toml::table &local,
                                       const std::string &key) {
        if (const auto value = local[key].value<double>()) return value;
        if (const auto value = local[key].value<int>()) return static_cast<double>(*value);
        if (const auto value = global[key].value<double>()) return value;
        if (const auto value = global[key].value<int>()) return static_cast<double>(*value);
        return std::nullopt;
    }

    std::string trim(std::string value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        const auto last = value.find_last_not_of(" \t\r\n");
        return first == std::string::npos ? std::string{} : value.substr(first, last - first + 1);
    }

}

export namespace sparsity::config {

    std::filesystem::path home_directory() {
        if (const char *home = std::getenv("HOME"); home != nullptr && *home != '\0') {
            return std::filesystem::path(home);
        }
        if (const passwd *entry = ::getpwuid(::getuid()); entry != nullptr && entry->pw_dir != nullptr) {
            return std::filesystem::path(entry->pw_dir);
        }
        return std::filesystem::current_path();
    }

    std::filesystem::path credentials_path(const std::filesystem::path &home) {
        return home / "credentials.toml";
    }

    std::string derive_api_root(const std::string &base_url) {
        auto value = base_url;
        while (!value.empty() && value.back() == '/') value.pop_back();
        for (const std::string suffix: {"/anthropic", "/v1"}) {
            if (value.ends_with(suffix)) {
                value.resize(value.size() - suffix.size());
                break;
            }
        }
        return value.empty() ? "https://api.deepseek.com" : value;
    }

    Config load(const std::filesystem::path &playground, const std::filesystem::path &home) {
        toml::table global;
        toml::table local;
        try {
            if (std::filesystem::is_regular_file(home / "config.toml")) {
                global = toml::parse_file((home / "config.toml").string());
            }
            const auto project_config = playground / ".sparsity" / "config.toml";
            if (std::filesystem::is_regular_file(project_config)) {
                local = toml::parse_file(project_config.string());
            }
        } catch (const toml::parse_error &error) {
            throw std::runtime_error(std::string("invalid config: ") + error.description().data());
        }

        Config result;
        result.home = home;
        if (const auto value = string_value(global, local, "model")) result.model = trim(*value);
        if (const auto value = string_value(global, local, "effort")) result.effort = trim(*value);
        if (const auto value = int_value(global, local, "max_output_tokens")) result.max_output_tokens = *value;
        if (const auto value = int_value(global, local, "max_steps")) result.max_steps = *value;
        if (const auto value = double_value(global, local, "max_turn_cost")) result.max_turn_cost = *value;
        if (const auto value = string_value(global, local, "base_url")) result.base_url = trim(*value);
        if (const auto value = string_value(global, local, "api_root")) result.api_root = trim(*value);
        else result.api_root = derive_api_root(result.base_url);

        const auto *global_permissions = global["permissions"].as_table();
        const auto *local_permissions = local["permissions"].as_table();
        if (local_permissions != nullptr) {
            if (const auto value = (*local_permissions)["mode"].value<std::string>()) result.permissions.mode = *value;
        } else if (global_permissions != nullptr) {
            if (const auto value = (*global_permissions)["mode"].value<std::string>()) result.permissions.mode = *value;
        }

        const auto *global_tools = global["tools"].as_table();
        const auto *local_tools = local["tools"].as_table();
        if (local_tools != nullptr) {
            if (const auto value = (*local_tools)["bash_timeout_s"].value<int>()) result.tools.bash_timeout_s = *value;
            if (const auto value = (*local_tools)["tool_output_limit"].value<int>()) result.tools.tool_output_limit = *value;
        } else if (global_tools != nullptr) {
            if (const auto value = (*global_tools)["bash_timeout_s"].value<int>()) result.tools.bash_timeout_s = *value;
            if (const auto value = (*global_tools)["tool_output_limit"].value<int>()) result.tools.tool_output_limit = *value;
        }

        const auto *global_web = global["web"].as_table();
        const auto *local_web = local["web"].as_table();
        const auto apply_web = [&result](const toml::table &data) {
            if (const auto value = data["api_base"].value<std::string>()) result.web.api_base = *value;
            if (const auto value = data["search_count"].value<int>()) result.web.search_count = *value;
            if (const auto value = data["fetch_max_chars"].value<int>()) result.web.fetch_max_chars = *value;
        };
        if (global_web != nullptr) apply_web(*global_web);
        if (local_web != nullptr) apply_web(*local_web);

        const auto *global_compaction = global["compaction"].as_table();
        const auto *local_compaction = local["compaction"].as_table();
        const auto apply = [&result](const toml::table &data) {
            if (const auto value = data["context_window"].value<int>()) result.compaction.context_window = *value;
            if (const auto value = data["threshold_ratio"].value<double>()) result.compaction.threshold_ratio = *value;
            if (const auto value = data["retain_ratio"].value<double>()) result.compaction.retain_ratio = *value;
            if (const auto value = data["headroom_tokens"].value<int>()) result.compaction.headroom_tokens = *value;
            if (const auto value = data["prune_tool_chars"].value<int>()) result.compaction.prune_tool_chars = *value;
            if (const auto value = data["max_summary_tokens"].value<int>()) result.compaction.max_summary_tokens = *value;
            if (const auto value = data["auto"].value<bool>()) result.compaction.auto_compact = *value;
        };
        if (global_compaction != nullptr) apply(*global_compaction);
        if (local_compaction != nullptr) apply(*local_compaction);

        if (result.effort != "off" && result.effort != "low" && result.effort != "high" && result.effort != "max") {
            throw std::runtime_error("effort must be off, low, high or max");
        }
        if (result.permissions.mode != "ask" && result.permissions.mode != "auto" &&
            result.permissions.mode != "readonly" && result.permissions.mode != "workspace") {
            throw std::runtime_error("permissions.mode must be ask, workspace, auto or readonly");
        }
        if (result.max_output_tokens < 256) throw std::runtime_error("max_output_tokens is too small");
        if (result.max_steps < 0) throw std::runtime_error("max_steps must be zero or positive");
        if (result.max_turn_cost < 0.0) throw std::runtime_error("max_turn_cost must be zero or positive");
        if (result.tools.bash_timeout_s < 1) throw std::runtime_error("tools.bash_timeout_s must be positive");
        if (result.tools.tool_output_limit < 1024) throw std::runtime_error("tools.tool_output_limit is too small");
        if (result.compaction.threshold_ratio < 0.5 || result.compaction.threshold_ratio > 0.95) {
            throw std::runtime_error("compaction.threshold_ratio must be between 0.5 and 0.95");
        }
        if (result.compaction.retain_ratio < 0.01 || result.compaction.retain_ratio >= result.compaction.threshold_ratio) {
            throw std::runtime_error("compaction.retain_ratio must be below threshold_ratio");
        }
        if (result.compaction.context_window < 4096) throw std::runtime_error("compaction.context_window is too small");
        if (result.compaction.headroom_tokens < 0) throw std::runtime_error("compaction.headroom_tokens must not be negative");
        if (result.compaction.prune_tool_chars < 512) throw std::runtime_error("compaction.prune_tool_chars is too small");
        if (result.compaction.max_summary_tokens < 512) throw std::runtime_error("compaction.max_summary_tokens is too small");
        if (result.web.search_count < 1 || result.web.search_count > 50) throw std::runtime_error("web.search_count must be 1..50");
        if (result.web.fetch_max_chars < 1024) throw std::runtime_error("web.fetch_max_chars is too small");
        return result;
    }

    void ensure_home(const std::filesystem::path &home) {
        for (const auto &path: {home, home / "artifacts", home / "projects"}) {
            std::error_code error;
            std::filesystem::create_directories(path, error);
            if (error) throw std::runtime_error("cannot create " + path.string());
        }
        std::error_code error;
        std::filesystem::permissions(home, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, error);
    }

    std::string resolve_api_key(const std::filesystem::path &home) {
        const auto path = credentials_path(home);
        std::error_code error;
        if (std::filesystem::is_regular_file(path, error)) {
            try {
                const auto document = toml::parse_file(path.string());
                if (const auto value = document["api_key"].value<std::string>()) {
                    if (!value->empty()) return *value;
                }
                if (const auto *keys = document["keys"].as_table()) {
                    if (const auto value = (*keys)["DEEPSEEK_API_KEY"].value<std::string>()) {
                        if (!value->empty()) return *value;
                    }
                }
            } catch (const toml::parse_error &) {
                // Fall back to the environment.
            }
        }
        for (const char *name: {"DEEPSEEK_API_KEY", "SPARSITY_API_KEY"}) {
            if (const char *value = std::getenv(name); value != nullptr && *value != '\0') {
                return value;
            }
        }
        return {};
    }

    void save_api_key(const std::filesystem::path &home, const std::string &key) {
        ensure_home(home);
        const auto path = credentials_path(home);
        std::ofstream output(path, std::ios::trunc);
        if (!output) throw std::runtime_error("cannot write " + path.string());
        output << "api_key = \"" << key << "\"\n";
        output.close();
        std::error_code error;
        std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                          std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, error);
    }

    bool remove_api_key(const std::filesystem::path &home) {
        std::error_code error;
        return std::filesystem::remove(credentials_path(home), error);
    }

    std::string read_secret(const std::string &prompt) {
        std::cout << prompt << std::flush;
        termios original{};
        const bool interactive = ::isatty(STDIN_FILENO) != 0 && ::tcgetattr(STDIN_FILENO, &original) == 0;
        if (interactive) {
            auto hidden = original;
            hidden.c_lflag &= ~static_cast<tcflag_t>(ECHO);
            ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden);
        }
        std::string value;
        std::getline(std::cin, value);
        if (interactive) {
            ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
            std::cout << '\n';
        }
        return trim(value);
    }

}
