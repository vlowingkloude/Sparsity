module;

#include <chrono>
#include <cstddef>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module models;

import json;
import http;

export namespace sparsity::models {

    struct ModelInfo {
        std::string id;
        std::string name;
        std::string owner{"deepseek"};
        std::size_t context_window{1'048'576};
        std::size_t max_output_tokens{65'536};
        bool supports_image{false};
        std::vector<std::string> effort_levels{"low", "high", "max"};
        std::string default_effort{"high"};
        std::string system_prompt_update{"leading-only"};
    };

    struct Catalog {
        std::vector<ModelInfo> models;
        std::string source{"fallback"};
        std::string fetched_at;
    };

    std::vector<ModelInfo> default_catalog();
    Catalog fallback_catalog();
    Catalog load_cache(const std::filesystem::path &path);
    Catalog fetch(const std::string &api_root, const std::string &api_key, http::Cancel *cancel = nullptr);
    void save_cache(const std::filesystem::path &path, const Catalog &catalog);
    const ModelInfo *find(const Catalog &catalog, const std::string &id);

    json::json_type model_json(const ModelInfo &model);
    json::json_type catalog_json(const std::vector<ModelInfo> &models, const std::string_view source);
    json::json_type catalog_json(const Catalog &catalog);
    json::json_type default_catalog_json();

}

namespace sparsity::models {

    json::json_type model_json(const ModelInfo &model) {
        json::json_type modalities = json::json_type::array({"text"});
        if (model.supports_image) {
            modalities.push_back("image");
        }

        return {
            {"id", model.id},
            {"object", "model"},
            {"owned_by", model.owner},
            {"name", model.name},
            {"context_window", model.context_window},
            {"max_output_tokens", model.max_output_tokens},
            {"input_modalities", std::move(modalities)},
            {"output_modalities", json::json_type::array({"text"})},
            {
                "effort", {
                    {"supported_levels", model.effort_levels},
                    {"default_level", model.default_effort},
                }
            },
            {
                "api_capabilities", {
                    {
                        "anthropic_messages", {
                            {"system_prompt_update", model.system_prompt_update}
                        }
                    }
                }
            },
        };
    }

}

export namespace sparsity::models {

    ModelInfo model_from_json(const json::json_type &value) {
        ModelInfo model;
        if (!value.is_object()) return model;
        if (value.contains("id") && value["id"].is_string()) model.id = value["id"].get<std::string>();
        if (value.contains("name") && value["name"].is_string()) model.name = value["name"].get<std::string>();
        else model.name = model.id;
        if (value.contains("owned_by") && value["owned_by"].is_string()) model.owner = value["owned_by"].get<std::string>();
        if (value.contains("context_window") && value["context_window"].is_number()) model.context_window = value["context_window"].get<std::size_t>();
        if (value.contains("max_output_tokens") && value["max_output_tokens"].is_number()) model.max_output_tokens = value["max_output_tokens"].get<std::size_t>();
        if (value.contains("input_modalities") && value["input_modalities"].is_array()) {
            for (const auto &item: value["input_modalities"]) {
                if (item.is_string() && item.get<std::string>() == "image") model.supports_image = true;
            }
        }
        if (value.contains("effort") && value["effort"].is_object()) {
            const auto &effort = value["effort"];
            if (effort.contains("supported_levels") && effort["supported_levels"].is_array()) {
                model.effort_levels.clear();
                for (const auto &item: effort["supported_levels"]) {
                    if (item.is_string()) model.effort_levels.push_back(item.get<std::string>());
                }
                if (model.effort_levels.empty()) model.effort_levels = {"low", "high", "max"};
            }
            if (effort.contains("default_level") && effort["default_level"].is_string()) {
                model.default_effort = effort["default_level"].get<std::string>();
            }
        }
        if (value.contains("api_capabilities") && value["api_capabilities"].is_object()) {
            const auto &capabilities = value["api_capabilities"];
            if (capabilities.contains("anthropic_messages") && capabilities["anthropic_messages"].is_object()) {
                const auto &messages = capabilities["anthropic_messages"];
                if (messages.contains("system_prompt_update") && messages["system_prompt_update"].is_string()) {
                    model.system_prompt_update = messages["system_prompt_update"].get<std::string>();
                }
            }
        }
        return model;
    }

    std::string now_iso() {
        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);
        std::tm utc{};
        gmtime_r(&time, &utc);
        char buffer[32];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
        return buffer;
    }

    Catalog fallback_catalog() {
        Catalog catalog;
        catalog.models = default_catalog();
        catalog.source = "fallback";
        return catalog;
    }

    Catalog load_cache(const std::filesystem::path &path) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) return fallback_catalog();
        try {
            std::ifstream input(path);
            if (!input) return fallback_catalog();
            const auto document = json::json_type::parse(input);
            Catalog catalog;
            catalog.source = "cache";
            if (document.contains("fetched_at") && document["fetched_at"].is_string()) {
                catalog.fetched_at = document["fetched_at"].get<std::string>();
            }
            if (document.contains("models") && document["models"].is_array()) {
                for (const auto &item: document["models"]) catalog.models.push_back(model_from_json(item));
            }
            if (catalog.models.empty()) return fallback_catalog();
            return catalog;
        } catch (...) {
            return fallback_catalog();
        }
    }

    Catalog fetch(const std::string &api_root, const std::string &api_key, http::Cancel *cancel) {
        auto root = api_root;
        while (!root.empty() && root.back() == '/') root.pop_back();
        const auto response = http::get(http::parse_url(root + "/models"), {
            {"authorization", "Bearer " + api_key},
        }, cancel);
        if (response.status >= 400) {
            throw std::runtime_error("models request failed: " + std::to_string(response.status));
        }
        const auto document = json::json_type::parse(response.body);
        Catalog catalog;
        catalog.source = "live";
        catalog.fetched_at = now_iso();
        if (document.contains("data") && document["data"].is_array()) {
            for (const auto &item: document["data"]) {
                auto model = model_from_json(item);
                if (!model.id.empty()) catalog.models.push_back(std::move(model));
            }
        }
        if (catalog.models.empty()) throw std::runtime_error("models response was empty");
        return catalog;
    }

    void save_cache(const std::filesystem::path &path, const Catalog &catalog) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        std::ofstream output(path, std::ios::trunc);
        if (!output) return;
        output << catalog_json(catalog).dump(2) << '\n';
    }

    const ModelInfo *find(const Catalog &catalog, const std::string &id) {
        for (const auto &model: catalog.models) {
            if (model.id == id) return &model;
        }
        return nullptr;
    }

    json::json_type catalog_json(const Catalog &catalog) {
        auto value = catalog_json(catalog.models, catalog.source);
        value["fetched_at"] = catalog.fetched_at;
        return value;
    }

}

export namespace sparsity::models {

    std::vector<ModelInfo> default_catalog() {
        auto flash = ModelInfo{};
        flash.id = "deepseek-flash";
        flash.name = "DeepSeek-V4.1-Flash";
        flash.supports_image = true;
        flash.effort_levels = {"low", "high", "max"};
        flash.default_effort = "high";
        flash.system_prompt_update = "in-history";

        auto pro = ModelInfo{};
        pro.id = "deepseek-v4-pro";
        pro.name = "DeepSeek-V4-Pro";
        pro.supports_image = false;
        pro.effort_levels = {"low", "high", "max"};
        pro.default_effort = "high";
        pro.system_prompt_update = "leading-only";

        return {flash, pro};
    }

    json::json_type catalog_json(const std::vector<ModelInfo> &models, const std::string_view source) {
        json::json_type list = json::json_type::array();
        for (const auto &model: models) {
            list.push_back(model_json(model));
        }
        return {
            {"models", std::move(list)},
            {"source", source},
            {"fetched_at", ""},
        };
    }

    json::json_type default_catalog_json() {
        return catalog_json(default_catalog(), "fallback");
    }

}
