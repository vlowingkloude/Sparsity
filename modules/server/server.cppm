module;

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <pwd.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <utility>
#include <vector>

export module server;

import lsp;
import json;
import terminal;
import watcher;
import files;
import projects;
import models;
import config;
import session;
import agent;
import uploads;
import fakeagent;
import boost_wrapper;

export namespace sparsity::server {

    struct Context {
        files::WorkspaceInfo playground;
        std::filesystem::path static_directory;
        projects::ProjectRegistry projects;
        config::Config config;
        std::string api_key;
        models::Catalog catalog;
        bool fake_agent{false};
        session::Store sessions;
        std::mutex catalog_mutex;

        Context(files::WorkspaceInfo playground_value, std::filesystem::path static_value,
                config::Config config_value, std::string key, models::Catalog catalog_value,
                const bool fake)
            : playground(std::move(playground_value)),
              static_directory(std::move(static_value)),
              projects(playground.root),
              config(std::move(config_value)),
              api_key(std::move(key)),
              catalog(std::move(catalog_value)),
              fake_agent(fake),
              sessions(config.home) {
        }
    };

    class Server {
    public:
        Server(files::WorkspaceInfo playground, std::filesystem::path static_directory,
               config::Config config, std::string api_key, models::Catalog catalog, bool fake_agent);
        void run(const std::string &host, std::uint16_t port, const std::atomic_bool &stopping);

    private:
        std::shared_ptr<Context> context_;
    };

}

namespace sparsity::server {

    class RequestError final : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    struct Target {
        std::string path;
        std::map<std::string, std::string> query;
    };

    namespace asio = boost_wrapper::asio;
    namespace beast = boost_wrapper::beast;
    namespace http = beast::http;
    namespace websocket = beast::websocket;

    using tcp = asio::ip::tcp;
    using Request = boost_wrapper::HttpRequest;
    using Response = boost_wrapper::HttpResponse;

}

namespace sparsity::server {

    std::string trim(std::string value) {
        const auto whitespace = [](const unsigned char character) {
            return std::isspace(character) != 0;
        };
        value.erase(value.begin(), std::ranges::find_if_not(value.begin(), value.end(), whitespace));
        value.erase(std::find_if_not(value.rbegin(), value.rend(), whitespace).base(), value.end());
        return value;
    }

    int hex_value(char character) {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        if (character >= 'A' && character <= 'F') return character - 'A' + 10;
        return -1;
    }

    std::string url_decode(std::string_view input) {
        std::string result;
        result.reserve(input.size());
        for (std::size_t index = 0; index < input.size(); ++index) {
            if (input[index] == '+') {
                result.push_back(' ');
            } else if (input[index] == '%' && index + 2 < input.size()) {
                const int high = hex_value(input[index + 1]);
                const int low = hex_value(input[index + 2]);
                if (high < 0 || low < 0) {
                    throw RequestError("invalid URL encoding");
                }
                result.push_back(static_cast<char>((high << 4) | low));
                index += 2;
            } else if (input[index] == '%') {
                throw RequestError("invalid URL encoding");
            } else {
                result.push_back(input[index]);
            }
        }
        return result;
    }

    Target parse_target(const std::string_view raw_target) {
        const std::string target(raw_target);
        const auto question = target.find('?');
        Target result{url_decode(target.substr(0, question)), {}};
        if (question == std::string::npos) {
            return result;
        }
        std::size_t start = question + 1;
        while (start <= target.size()) {
            const auto end = target.find('&', start);
            const auto item = std::string_view(target).substr(
                start, end == std::string::npos ? std::string::npos : end - start);
            const auto equals = item.find('=');
            const auto name = url_decode(item.substr(0, equals));
            const auto value = equals == std::string_view::npos
                                   ? std::string{}
                                   : url_decode(item.substr(equals + 1));
            result.query[name] = value;
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
        return result;
    }

    files::WorkspaceInfo project_workspace(const Context &context, const std::string &id) {
        if (id.empty()) {
            throw RequestError("project id is required");
        }
        const auto project = context.projects.get(id);
        if (!project.has_value()) {
            throw RequestError("project not found");
        }
        return project->workspace;
    }

    files::WorkspaceInfo workspace_from_query(const Context &context, const Target &target) {
        const auto found = target.query.find("project");
        if (found == target.query.end() || found->second.empty()) {
            return context.playground;
        }
        return project_workspace(context, found->second);
    }

    std::optional<std::string> optional_string(const json::json_type &value, const char *name) {
        const auto found = value.find(name);
        if (found == value.end() || found->is_null()) {
            return std::nullopt;
        }
        if (!found->is_string()) {
            throw RequestError(std::string("invalid JSON request: ") + name + " must be a string or null");
        }
        return found->get<std::string>();
    }

    files::WorkspaceInfo workspace_from_json(const Context &context, const json::json_type &value) {
        const auto project = optional_string(value, "project");
        if (project.has_value() && !project->empty()) {
            return project_workspace(context, *project);
        }
        return context.playground;
    }

    json::json_type decode_json(const Request &request, std::initializer_list<std::string_view> allowed) {
        json::json_type value;
        try {
            value = json::json_type::parse(request.body());
        } catch (const std::exception &caught) {
            throw RequestError(std::string("invalid JSON request: ") + caught.what());
        }
        if (!value.is_object()) {
            throw RequestError("invalid JSON request: expected an object");
        }
        const std::unordered_set<std::string_view> fields(allowed);
        return value;
    }

    std::string required_string(const json::json_type &value, const char *name) {
        const auto found = value.find(name);
        if (found == value.end() || !found->is_string()) {
            throw RequestError(std::string("invalid JSON request: ") + name + " must be a string");
        }
        return found->get<std::string>();
    }

    void decorate(Response &response) {
        response.set(http::field::server, "Sparsity");
        response.set(http::field::access_control_allow_origin, "*");
        response.set(http::field::access_control_allow_headers, "Content-Type");
        response.set(http::field::access_control_allow_methods, "GET, POST, DELETE, OPTIONS");
    }

    Response text_response(const Request &request, http::status status, std::string body,
                           std::string_view content_type) {
        Response response{status, request.version()};
        response.keep_alive(request.keep_alive());
        decorate(response);
        response.set(http::field::content_type, content_type);
        response.body() = std::move(body);
        response.prepare_payload();
        return response;
    }

    Response json_response(const Request &request, http::status status, const json::json_type &value) {
        return text_response(request, status, value.dump(-1, ' ', false, json::json_type::error_handler_t::replace) + "\n", "application/json; charset=utf-8");
    }

    Response empty_response(const Request &request, http::status status = http::status::no_content) {
        Response response{status, request.version()};
        response.keep_alive(request.keep_alive());
        decorate(response);
        response.prepare_payload();
        return response;
    }

    Response error_response(const Request &request, const std::exception &error) {
        auto status = http::status::internal_server_error;
        if (dynamic_cast<const RequestError *>(&error) != nullptr) {
            status = http::status::bad_request;
        }
        if (const auto *system = dynamic_cast<const std::system_error *>(&error)) {
            if (system->code() == std::errc::no_such_file_or_directory) {
                status = http::status::not_found;
            }
        }
        json::json_type err = {{"error", error.what()}};
        return json_response(request, status, err);
    }

    Response require_method(const Request &request, http::verb expected) {
        json::json_type err = {{"error", "method not allowed"}};
        auto response = json_response(request, http::status::method_not_allowed, err);
        response.set(http::field::allow, boost_wrapper::http_verb_string(expected));
        return response;
    }

    std::string mime_type(const std::filesystem::path &path) {
        auto extension = path.extension().string();
        std::ranges::transform(extension, extension.begin(), [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        if (extension == ".htm" || extension == ".html") return "text/html; charset=utf-8";
        if (extension == ".css") return "text/css; charset=utf-8";
        if (extension == ".js" || extension == ".mjs") return "text/javascript; charset=utf-8";
        if (extension == ".json" || extension == ".map") return "application/json; charset=utf-8";
        if (extension == ".svg") return "image/svg+xml";
        if (extension == ".png") return "image/png";
        if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
        if (extension == ".gif") return "image/gif";
        if (extension == ".webp") return "image/webp";
        if (extension == ".bmp") return "image/bmp";
        if (extension == ".ico") return "image/x-icon";
        if (extension == ".avif") return "image/avif";
        if (extension == ".woff") return "font/woff";
        if (extension == ".woff2") return "font/woff2";
        if (extension == ".wasm") return "application/wasm";
        return "application/octet-stream";
    }

    std::string read_file(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("failed to open file: " + path.string());
        }
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    Response static_response(const Request &request, const Target &target,
                             const std::filesystem::path &static_directory) {
        auto relative = std::filesystem::path(target.path.empty() ? "/" : target.path)
                            .lexically_normal()
                            .relative_path();
        auto path = static_directory / relative;
        std::error_code error;
        if (std::filesystem::is_directory(path, error)) {
            path /= "index.html";
        }
        if (!error && std::filesystem::is_regular_file(path, error)) {
            return text_response(request, http::status::ok, read_file(path), mime_type(path));
        }
        const auto index = static_directory / "index.html";
        if (!std::filesystem::is_regular_file(index, error)) {
            throw std::runtime_error("frontend build not found in " + static_directory.string());
        }
        return text_response(request, http::status::ok, read_file(index), mime_type(index));
    }

    std::filesystem::path home_directory() {
        if (const char *home = std::getenv("HOME"); home != nullptr && *home != '\0') {
            std::filesystem::path path(home);
            std::error_code error;
            if (std::filesystem::is_directory(path, error)) {
                return path;
            }
        }
        if (const passwd *entry = ::getpwuid(::getuid()); entry != nullptr && entry->pw_dir != nullptr) {
            return std::filesystem::path(entry->pw_dir);
        }
        return std::filesystem::current_path();
    }

    std::filesystem::path terminal_cwd(const Target &target) {
        const auto found = target.query.find("cwd");
        if (found == target.query.end() || found->second.empty()) {
            return home_directory();
        }
        std::filesystem::path path(found->second);
        if (!path.is_absolute()) {
            path = home_directory() / path;
        }
        std::error_code error;
        const auto resolved = std::filesystem::canonical(path, error);
        if (error || !std::filesystem::is_directory(resolved)) {
            throw RequestError("invalid terminal cwd");
        }
        return resolved;
    }

    Response handle_api(const Request &request, const Target &target, Context &context) {
        if (request.method() == http::verb::options) {
            return empty_response(request);
        }

        if (target.path == "/api/health") {
            if (request.method() != http::verb::get) {
                return require_method(request, http::verb::get);
            }
            json::json_type value = {{"ok", true}};
            return json_response(request, http::status::ok, value);
        }

        if (target.path == "/api/playground") {
            if (request.method() != http::verb::get) {
                return require_method(request, http::verb::get);
            }
            json::json_type value = {
                {"name", context.playground.name},
                {"root", context.playground.root.string()},
                {"kind", context.playground.kind},
            };
            return json_response(request, http::status::ok, value);
        }

        if (target.path == "/api/tree") {
            if (request.method() != http::verb::get) {
                return require_method(request, http::verb::get);
            }
            const auto directory = target.query.find("path");
            json::json_type value = files::list_tree(
                context.playground,
                directory == target.query.end() ? "" : directory->second);
            return json_response(request, http::status::ok, value);
        }

        if (target.path == "/api/files") {
            if (request.method() != http::verb::get) {
                return require_method(request, http::verb::get);
            }
            json::json_type value = files::list_workspace_files(workspace_from_query(context, target));
            return json_response(request, http::status::ok, value);
        }

        if (target.path == "/api/file/read" || target.path == "/api/file/write" ||
            target.path == "/api/file/create" || target.path == "/api/file/rename" ||
            target.path == "/api/file/delete") {
            if (request.method() != http::verb::post) {
                return require_method(request, http::verb::post);
            }
            json::json_type body;
            if (target.path == "/api/file/write") {
                body = decode_json(request, {"project", "path", "content"});
            } else if (target.path == "/api/file/create") {
                body = decode_json(request, {"project", "path", "kind"});
            } else if (target.path == "/api/file/rename") {
                body = decode_json(request, {"project", "path", "destination"});
            } else {
                body = decode_json(request, {"project", "path"});
            }
            const auto workspace = workspace_from_json(context, body);
            const auto path = required_string(body, "path");
            if (target.path == "/api/file/read") {
                json::json_type value = files::read_text_file(workspace, path);
                return json_response(request, http::status::ok, value);
            }
            if (target.path == "/api/file/write") {
                json::json_type value = files::write_text_file(
                    workspace, path, required_string(body, "content"));
                return json_response(request, http::status::ok, value);
            }
            if (target.path == "/api/file/create") {
                files::create_entry(workspace, path, required_string(body, "kind"));
            } else if (target.path == "/api/file/rename") {
                files::rename_entry(workspace, path, required_string(body, "destination"));
            } else {
                files::delete_entry(workspace, path);
            }
            return empty_response(request);
        }

        if (target.path == "/api/search") {
            if (request.method() != http::verb::post) {
                return require_method(request, http::verb::post);
            }
            const auto body = decode_json(request, {"project", "query", "case_sensitive"});
            const auto workspace = workspace_from_json(context, body);
            const auto query = trim(required_string(body, "query"));
            bool case_sensitive = false;
            if (const auto found = body.find("case_sensitive"); found != body.end()) {
                if (!found->is_boolean()) {
                    throw RequestError("invalid JSON request: case_sensitive must be a boolean");
                }
                case_sensitive = found->get<bool>();
            }
            json::json_type value = files::search_workspace(workspace, query, case_sensitive);
            return json_response(request, http::status::ok, value);
        }

        if (target.path == "/api/upload") {
            if (request.method() != http::verb::post) {
                return require_method(request, http::verb::post);
            }
            const auto project = target.query.find("project");
            const auto session_id = target.query.find("session");
            if (project == target.query.end() || project->second.empty()) throw RequestError("project is required");
            if (session_id == target.query.end() || session_id->second.empty()) throw RequestError("session is required");
            const auto info = context.projects.get(project->second);
            if (!info.has_value()) throw RequestError("project not found");
            const auto session = context.sessions.load(project->second, session_id->second);
            if (!session) throw RequestError("session not found");
            bool supports_image = false;
            {
                std::lock_guard lock(context.catalog_mutex);
                if (const auto *model = models::find(context.catalog, session->model()); model != nullptr) {
                    supports_image = model->supports_image;
                }
            }
            const auto name = target.query.find("name") == target.query.end() ? std::string{"upload"} : target.query.at("name");
            try {
                const auto result = uploads::handle(name, request.body(),
                    context.config.home / "projects" / project->second / "uploads" / session_id->second,
                    supports_image);
                json::json_type value = {
                    {"kind", result.kind},
                    {"name", result.name},
                    {"preview", result.preview},
                    {"blocks", result.blocks},
                };
                return json_response(request, http::status::ok, value);
            } catch (const std::exception &caught) {
                throw RequestError(caught.what());
            }
        }

        if (target.path == "/api/file/raw") {
            if (request.method() != http::verb::get) {
                return require_method(request, http::verb::get);
            }
            const auto path_value = target.query.find("path");
            const auto path = files::raw_file_path(
                workspace_from_query(context, target),
                path_value == target.query.end() ? "" : path_value->second);
            const auto content_type = mime_type(path);
            if (!content_type.starts_with("image/")) {
                throw RequestError("raw preview is limited to image files");
            }
            return text_response(request, http::status::ok, read_file(path), content_type);
        }

        if (target.path == "/api/projects") {
            if (request.method() != http::verb::get) {
                return require_method(request, http::verb::get);
            }
            json::json_type value = json::json_type::array();
            for (const auto &project: context.projects.list()) {
                value.push_back(project);
            }
            return json_response(request, http::status::ok, value);
        }

        if (target.path == "/api/projects/open") {
            if (request.method() != http::verb::post) {
                return require_method(request, http::verb::post);
            }
            const auto body = decode_json(request, {"path"});
            const auto path = trim(required_string(body, "path"));
            try {
                json::json_type value = context.projects.open(path);
                return json_response(request, http::status::ok, value);
            } catch (const std::exception &caught) {
                throw RequestError(caught.what());
            }
        }

        constexpr std::string_view project_prefix = "/api/projects/";
        if (target.path.starts_with(project_prefix)) {
            const auto rest = target.path.substr(project_prefix.size());
            const auto slash = rest.find('/');
            const auto id = slash == std::string::npos ? rest : rest.substr(0, slash);
            if (id.empty()) {
                json::json_type value = {{"error", "API route not found"}};
                return json_response(request, http::status::not_found, value);
            }
            const auto project = context.projects.get(id);
            if (!project.has_value()) {
                json::json_type value = {{"error", "project not found"}};
                return json_response(request, http::status::not_found, value);
            }
            if (slash == std::string::npos) {
                if (request.method() == http::verb::get) {
                    json::json_type value = *project;
                    return json_response(request, http::status::ok, value);
                }
                if (request.method() == http::verb::delete_) {
                    context.projects.close(id);
                    return empty_response(request);
                }
                return require_method(request, http::verb::get);
            }

            const auto tail = rest.substr(slash + 1);
            if (tail == "sessions") {
                if (request.method() == http::verb::get) {
                    json::json_type value = json::json_type::array();
                    for (const auto &meta: context.sessions.list(id)) {
                        value.push_back({
                            {"id", meta.id},
                            {"title", meta.title},
                            {"messages", meta.messages},
                            {"updated", meta.updated},
                        });
                    }
                    return json_response(request, http::status::ok, value);
                }
                if (request.method() == http::verb::post) {
                    std::string model = context.config.model;
                    std::string effort = context.config.effort;
                    if (!request.body().empty()) {
                        const auto body = decode_json(request, {"model", "effort"});
                        if (const auto value = optional_string(body, "model"); value.has_value() && !value->empty()) {
                            model = *value;
                        }
                        if (const auto value = optional_string(body, "effort"); value.has_value() && !value->empty()) {
                            effort = *value;
                        }
                    }
                    const auto created = context.sessions.create(id, project->workspace.root.string(),
                                                                 model, effort);
                    json::json_type value = {
                        {"id", created->id()},
                        {"title", created->title()},
                        {"messages", 0},
                        {"model", created->model()},
                        {"effort", created->effort()},
                    };
                    return json_response(request, http::status::ok, value);
                }
                return require_method(request, http::verb::get);
            }

            constexpr std::string_view sessions_prefix = "sessions/";
            if (tail.starts_with(sessions_prefix)) {
                const auto session_id = tail.substr(sessions_prefix.size());
                if (session_id.empty() || session_id.find('/') != std::string::npos) {
                    json::json_type value = {{"error", "API route not found"}};
                    return json_response(request, http::status::not_found, value);
                }
                if (request.method() == http::verb::delete_) {
                    if (!context.sessions.remove(id, session_id)) {
                        json::json_type value = {{"error", "session not found"}};
                        return json_response(request, http::status::not_found, value);
                    }
                    uploads::remove(context.config.home, id, session_id);
                    return json_response(request, http::status::ok, {{"ok", true}});
                }
                const auto session = context.sessions.load(id, session_id);
                if (!session) {
                    json::json_type value = {{"error", "session not found"}};
                    return json_response(request, http::status::not_found, value);
                }
                if (request.method() == http::verb::patch || request.method() == http::verb::post) {
                    const auto body = decode_json(request, {"title"});
                    session->add_title(trim(required_string(body, "title")));
                    return json_response(request, http::status::ok, session->meta_json());
                }
                if (request.method() == http::verb::get) {
                    return json_response(request, http::status::ok, session->meta_json());
                }
                return require_method(request, http::verb::patch);
            }

            json::json_type value = {{"error", "API route not found"}};
            return json_response(request, http::status::not_found, value);
        }

        if (target.path == "/api/models") {
            if (request.method() != http::verb::get) {
                return require_method(request, http::verb::get);
            }
            std::lock_guard lock(context.catalog_mutex);
            return json_response(request, http::status::ok, models::catalog_json(context.catalog));
        }

        if (target.path == "/api/models/refresh") {
            if (request.method() != http::verb::post) {
                return require_method(request, http::verb::post);
            }
            if (context.api_key.empty()) throw RequestError("no API key configured");
            try {
                auto catalog = models::fetch(context.config.api_root, context.api_key);
                models::save_cache(context.config.home / "models.json", catalog);
                {
                    std::lock_guard lock(context.catalog_mutex);
                    context.catalog = catalog;
                }
                return json_response(request, http::status::ok, models::catalog_json(catalog));
            } catch (const std::exception &caught) {
                throw RequestError(caught.what());
            }
        }

        if (target.path.starts_with("/api/")) {
            json::json_type value = {{"error", "API route not found"}};
            return json_response(request, http::status::not_found, value);
        }

        if (request.method() != http::verb::get) {
            return require_method(request, http::verb::get);
        }
        return static_response(request, target, context.static_directory);
    }

    bool is_websocket_path(const std::string &path) {
        return path == "/api/watch" || path == "/api/terminal" ||
               path == "/api/lsp" || path == "/api/agent";
    }

    std::optional<lsp::LanguageServer> language_server_for_target(const Target &target) {
        const auto language = target.query.find("language");
        const auto name = language == target.query.end() ? std::string{} : language->second;
        return lsp::language_server_for(name);
    }

    void validate_websocket_target(const Target &target, Context &context) {
        if (target.path == "/api/terminal") {
            (void) terminal_cwd(target);
            return;
        }
        if (target.path == "/api/agent") {
            const auto project = target.query.find("project");
            if (project == target.query.end() || project->second.empty()) {
                throw RequestError("project is required");
            }
            (void) project_workspace(context, project->second);
            return;
        }
        if (target.path == "/api/lsp") {
            const auto project = target.query.find("project");
            if (project == target.query.end() || project->second.empty()) {
                throw RequestError("project is required");
            }
            (void) project_workspace(context, project->second);
            if (!language_server_for_target(target).has_value()) {
                throw RequestError("unsupported language server");
            }
        }
    }

    void run_websocket(beast::tcp_stream stream, const Request &request, const Target &target,
                       std::shared_ptr<Context> context) {
        auto socket = boost_wrapper::make_websocket(std::move(stream));
        boost_wrapper::configure_websocket(socket, 16U << 20U);
        boost_wrapper::accept_websocket(socket, request);

        if (target.path == "/api/watch") {
            watcher::run(socket, context->playground);
        } else if (target.path == "/api/terminal") {
            terminal::run(socket, terminal_cwd(target));
        } else if (target.path == "/api/lsp") {
            const auto project = target.query.find("project");
            const auto workspace = project_workspace(*context, project->second);
            const auto server = language_server_for_target(target);
            lsp::run(socket, workspace.root, server.value());
        } else if (target.path == "/api/agent") {
            const auto project = target.query.find("project");
            const auto workspace = context->projects.get(project->second);
            if (!workspace.has_value()) {
                throw RequestError("project not found");
            }
            if (context->fake_agent) {
                fakeagent::run(socket, workspace->id);
                return;
            }
            const auto session = target.query.find("session");
            models::Catalog catalog;
            {
                std::lock_guard lock(context->catalog_mutex);
                catalog = context->catalog;
            }
            agent::AgentContext agent_context{
                workspace->workspace,
                workspace->id,
                context->playground,
                context->config,
                context->api_key,
                std::move(catalog),
                context->config.home,
            };
            agent::run(socket, std::move(agent_context), session == target.query.end() ? "" : session->second);
        } else {
            throw RequestError("API route not found");
        }
    }

    void handle_session(tcp::socket socket, std::shared_ptr<Context> context) {
        try {
            beast::tcp_stream stream(std::move(socket));
            beast::flat_buffer buffer;
            while (true) {
                stream.expires_after(std::chrono::seconds(10));
                boost_wrapper::HttpRequestParser parser;
                parser.body_limit(32U << 20U);
                boost_wrapper::error_code error;
                boost_wrapper::read_http(stream, buffer, parser, error);
                if (error == http::error::end_of_stream) {
                    break;
                }
                if (error) {
                    throw boost_wrapper::system_error(error);
                }
                auto request = parser.release();
                const auto request_target = boost_wrapper::http_target_string(request);
                const auto target = parse_target(request_target);
                std::clog << boost_wrapper::http_verb_string(request.method()) << ' ' << request_target << '\n';

                if (boost_wrapper::is_websocket_upgrade(request) && is_websocket_path(target.path)) {
                    try {
                        validate_websocket_target(target, *context);
                    } catch (const std::exception &caught) {
                        const auto response = error_response(request, caught);
                        boost_wrapper::write_http(stream, response);
                        return;
                    }
                    run_websocket(std::move(stream), request, target, std::move(context));
                    return;
                }

                Response response;
                try {
                    response = handle_api(request, target, *context);
                } catch (const std::exception &caught) {
                    response = error_response(request, caught);
                }
                const bool keep_alive = response.keep_alive();
                boost_wrapper::write_http(stream, response);
                if (!keep_alive) {
                    break;
                }
            }
            boost_wrapper::error_code ignored;
            ignored = stream.socket().shutdown(tcp::socket::shutdown_send, ignored);
        } catch (const boost_wrapper::system_error &caught) {
            if (caught.code() != websocket::error::closed && caught.code() != asio::error::eof &&
                caught.code() != asio::error::connection_reset && caught.code() != asio::error::not_connected) {
                std::clog << "connection failed: " << caught.what() << '\n';
            }
        } catch (const std::exception &caught) {
            std::clog << "connection failed: " << caught.what() << '\n';
        }
    }

}

export namespace sparsity::server {

    Server::Server(files::WorkspaceInfo playground, std::filesystem::path static_directory,
                   config::Config config, std::string api_key, models::Catalog catalog,
                   const bool fake_agent)
        : context_(std::make_shared<Context>(std::move(playground), std::move(static_directory),
                                             std::move(config), std::move(api_key),
                                             std::move(catalog), fake_agent)) {
        uploads::cleanup(context_->config.home);
    }

    void Server::run(const std::string &host, const std::uint16_t port,
                     const std::atomic_bool &stopping) {
        asio::io_context context(1);
        tcp::resolver resolver(context);
        const auto endpoints = resolver.resolve(host, std::to_string(port), tcp::resolver::passive);
        if (endpoints.empty()) {
            throw std::runtime_error("failed to resolve listen address: " + host);
        }
        tcp::acceptor acceptor(context);
        acceptor.open(endpoints.begin()->endpoint().protocol());
        acceptor.set_option(asio::socket_base::reuse_address(true));
        acceptor.bind(endpoints.begin()->endpoint());
        acceptor.listen(asio::socket_base::max_listen_connections);
        acceptor.non_blocking(true);
        while (!stopping.load(std::memory_order_relaxed)) {
            boost_wrapper::error_code error;
            tcp::socket socket(context);
            error = acceptor.accept(socket, error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            if (error) {
                throw boost_wrapper::system_error(error);
            }
            std::thread(handle_session, std::move(socket), context_).detach();
        }
    }

}
