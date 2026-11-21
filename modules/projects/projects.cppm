module;

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module projects;

import files;
import json;

export namespace sparsity::projects {

    struct ProjectInfo {
        std::string id;
        std::string path; // relative to the playground, "." for the playground itself
        files::WorkspaceInfo workspace;
    };

}

namespace sparsity::projects {

    std::string trim(std::string value) {
        const auto whitespace = [](const unsigned char character) {
            return std::isspace(character) != 0;
        };
        value.erase(value.begin(), std::ranges::find_if_not(value.begin(), value.end(), whitespace));
        value.erase(std::find_if_not(value.rbegin(), value.rend(), whitespace).base(), value.end());
        return value;
    }

    std::string stable_id(const std::string_view value) {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const unsigned char character : value) {
            hash ^= character;
            hash *= 1099511628211ULL;
        }
        std::ostringstream stream;
        stream << std::hex << std::setw(16) << std::setfill('0') << hash;
        return stream.str();
    }

}

export namespace sparsity::projects {

    void to_json(json::json_type &value, const ProjectInfo &project) {
        value = {
            {"id", project.id},
            {"path", project.path},
            {"name", project.workspace.name},
            {"kind", project.workspace.kind},
            {"root", project.workspace.root.string()},
            {
                "initial_file", project.workspace.initial_file.has_value()
                    ? json::json_type(project.workspace.initial_file.value())
                    : json::json_type(nullptr)
            },
        };
    }

    class ProjectRegistry {
    public:
        explicit ProjectRegistry(std::filesystem::path playground_root)
            : playground_root_(std::filesystem::canonical(std::move(playground_root))) {
        }

        ProjectRegistry(const ProjectRegistry &) = delete;
        ProjectRegistry &operator=(const ProjectRegistry &) = delete;

        ProjectInfo open(const std::string &raw_path) {
            auto relative = normalize(raw_path);
            const auto id = stable_id(relative);

            std::lock_guard lock(mutex_);
            if (const auto found = projects_.find(id); found != projects_.end()) {
                return found->second;
            }

            const auto target = relative == "."
                                    ? playground_root_
                                    : playground_root_ / std::filesystem::path(relative);
            auto workspace = files::open_workspace(target);
            ProjectInfo project{id, relative, std::move(workspace)};
            projects_.emplace(id, project);
            return project;
        }

        std::optional<ProjectInfo> get(const std::string &id) const {
            std::lock_guard lock(mutex_);
            const auto found = projects_.find(id);
            if (found == projects_.end()) {
                return std::nullopt;
            }
            return found->second;
        }

        std::vector<ProjectInfo> list() const {
            std::lock_guard lock(mutex_);
            std::vector<ProjectInfo> result;
            result.reserve(projects_.size());
            for (const auto &[id, project]: projects_) {
                result.push_back(project);
            }
            std::ranges::sort(result, [](const ProjectInfo &left, const ProjectInfo &right) {
                return left.path < right.path;
            });
            return result;
        }

        bool close(const std::string &id) {
            std::lock_guard lock(mutex_);
            return projects_.erase(id) > 0;
        }

        const std::filesystem::path &playground() const {
            return playground_root_;
        }

    private:
        std::string normalize(const std::string &raw_path) const {
            auto value = trim(raw_path);
            if (value.empty() || value == "." || value == "./") {
                return ".";
            }

            if (!files::validate_relative_path(value)) {
                throw std::runtime_error("invalid project path");
            }

            std::error_code error;
            const auto resolved = std::filesystem::canonical(playground_root_ / value, error);
            if (error) {
                throw std::runtime_error("project path does not exist");
            }
            if (!files::within_root(playground_root_, resolved)) {
                throw std::runtime_error("project path is outside the playground");
            }

            return std::filesystem::path(value).lexically_normal().generic_string();
        }

        std::filesystem::path playground_root_;
        std::map<std::string, ProjectInfo> projects_;
        mutable std::mutex mutex_;
    };

}
