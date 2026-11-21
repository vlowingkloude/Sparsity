module;

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

export module uploads;

import json;
import images;

export namespace sparsity::uploads {

    using json_type = json::json_type;

    struct Result {
        std::string kind;
        std::string name;
        json_type blocks = json_type::array();
        std::string preview;
    };

    Result handle(const std::string &name, const std::string &bytes,
                  const std::filesystem::path &directory, bool supports_image);

    bool remove(const std::filesystem::path &home, const std::string &project, const std::string &session);
    void cleanup(const std::filesystem::path &home);

}

namespace sparsity::uploads {

    std::string sanitize(const std::string &name) {
        std::string result;
        for (const char character: name) {
            if (std::isalnum(static_cast<unsigned char>(character)) || character == '.' ||
                character == '-' || character == '_') {
                result.push_back(character);
            } else {
                result.push_back('_');
            }
        }
        if (result.empty()) result = "upload";
        if (result.size() > 120) result.resize(120);
        return result;
    }

    std::string shell_quote(const std::string &value) {
        std::string result = "'";
        for (const char character: value) {
            if (character == '\'') result += "'\\''";
            else result.push_back(character);
        }
        result.push_back('\'');
        return result;
    }

    std::string capture(const std::string &command) {
        FILE *pipe = ::popen(command.c_str(), "r");
        if (pipe == nullptr) throw std::runtime_error("cannot run pdf tool");
        std::string output;
        char buffer[8192];
        while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) output += buffer;
        const auto status = ::pclose(pipe);
        if (status != 0) throw std::runtime_error("pdf tool failed");
        return output;
    }

    bool starts_with(const std::string &value, const std::string_view prefix) {
        return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
    }

    bool image_magic(const std::string &bytes) {
        const auto byte = [&bytes](const std::size_t index) -> unsigned char {
            return index < bytes.size() ? static_cast<unsigned char>(bytes[index]) : 0;
        };
        return (byte(0) == 0xff && byte(1) == 0xd8 && byte(2) == 0xff) ||
               (byte(0) == 0x89 && byte(1) == 0x50 && byte(2) == 0x4e && byte(3) == 0x47) ||
               (byte(0) == 0x47 && byte(1) == 0x49 && byte(2) == 0x46) ||
               (byte(0) == 0x42 && byte(1) == 0x4d) ||
               (byte(0) == 0x52 && byte(1) == 0x49 && byte(2) == 0x46 && byte(3) == 0x46);
    }

    bool image_extension(const std::string &name) {
        auto extension = std::filesystem::path(name).extension().string();
        std::ranges::transform(extension, extension.begin(), [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        static const std::vector<std::string> values{
            ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp"
        };
        return std::find(values.begin(), values.end(), extension) != values.end();
    }

    std::string preview(const std::string &text) {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return {};
        auto line = text.substr(first, text.find('\n', first) - first);
        if (line.size() > 160) line.resize(160);
        return line;
    }

    json_type text_block(const std::string &name, const std::string &path, const std::string &text) {
        auto body = text;
        if (body.size() > 20'000) body = body.substr(0, 20'000) + "\n… truncated";
        return {
            {"type", "text"},
            {"text", "<uploaded-file name=\"" + name + "\" path=\"" + path + "\">\n" + body +
                     "\n</uploaded-file>\n"},
        };
    }

    std::vector<std::filesystem::path> pages(const std::filesystem::path &prefix) {
        std::vector<std::filesystem::path> result;
        std::error_code error;
        const auto directory = prefix.parent_path();
        const auto stem = prefix.filename().string();
        for (const auto &entry: std::filesystem::directory_iterator(directory, error)) {
            const auto name = entry.path().filename().string();
            if (!name.starts_with(stem + "-") || entry.path().extension() != ".png") continue;
            result.push_back(entry.path());
        }
        std::ranges::sort(result);
        return result;
    }

}

export namespace sparsity::uploads {

    bool remove(const std::filesystem::path &home, const std::string &project, const std::string &session) {
        if (project.empty() || session.empty()) return false;
        std::error_code error;
        return std::filesystem::remove_all(home / "projects" / project / "uploads" / session, error) > 0;
    }

    void cleanup(const std::filesystem::path &home) {
        std::error_code error;
        const auto projects = home / "projects";
        if (!std::filesystem::is_directory(projects, error)) return;
        for (const auto &project: std::filesystem::directory_iterator(projects, error)) {
            if (!project.is_directory()) continue;
            const auto uploads = project.path() / "uploads";
            if (!std::filesystem::is_directory(uploads, error)) continue;
            for (const auto &session: std::filesystem::directory_iterator(uploads, error)) {
                if (!session.is_directory()) continue;
                const auto id = session.path().filename().string();
                if (!std::filesystem::is_regular_file(project.path() / "sessions" / (id + ".jsonl"), error)) {
                    std::filesystem::remove_all(session.path(), error);
                }
            }
        }
    }

    Result handle(const std::string &name, const std::string &bytes,
                  const std::filesystem::path &directory, const bool supports_image) {
        if (bytes.empty()) throw std::runtime_error("empty upload");
        if (bytes.size() > 20U * 1024U * 1024U) throw std::runtime_error("upload is too large");
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) throw std::runtime_error("cannot create upload directory");

        const auto stamp = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        const auto safe = sanitize(name);
        const auto source = directory / (stamp + "-" + safe);

        if (starts_with(bytes, "%PDF")) {
            const auto pdf_path = directory / (stamp + "-" + safe + ".pdf");
            std::ofstream(pdf_path, std::ios::binary | std::ios::trunc).write(
                bytes.data(), static_cast<std::streamsize>(bytes.size()));
            const auto text = capture("pdftotext -layout -enc UTF-8 " + shell_quote(pdf_path.string()) + " -");
            const auto text_path = directory / (stamp + "-" + safe + ".txt");
            std::ofstream(text_path) << text;
            Result result;
            result.kind = "pdf";
            result.name = name;
            result.preview = preview(text);
            result.blocks.push_back(text_block(name, text_path.string(), text));
            if (text.size() < 200) {
                if (!supports_image) throw std::runtime_error("scanned PDF needs an image-capable model");
                const auto prefix = directory / ("page-" + stamp);
                (void) std::system(("pdftoppm -png -r 120 -f 1 -l 10 " + shell_quote(pdf_path.string()) + " " +
                                    shell_quote(prefix.string())).c_str());
                for (const auto &page: pages(prefix)) {
                    const auto normalized = images::normalize(page, directory);
                    result.blocks.push_back({
                        {"type", "image_ref"},
                        {"path", normalized.path},
                        {"media_type", "image/jpeg"},
                        {"width", normalized.width},
                        {"height", normalized.height},
                        {"bytes", normalized.bytes},
                    });
                    std::filesystem::remove(page, error);
                }
            }
            return result;
        }

        if (image_magic(bytes) || image_extension(name)) {
            if (!supports_image) throw std::runtime_error("the current model does not accept images");
            std::ofstream(source, std::ios::binary | std::ios::trunc).write(
                bytes.data(), static_cast<std::streamsize>(bytes.size()));
            const auto normalized = images::normalize(source, directory);
            std::filesystem::remove(source, error);
            Result result;
            result.kind = "image";
            result.name = name;
            result.preview = safe;
            result.blocks.push_back({
                {"type", "image_ref"},
                {"path", normalized.path},
                {"media_type", "image/jpeg"},
                {"width", normalized.width},
                {"height", normalized.height},
                {"bytes", normalized.bytes},
            });
            return result;
        }

        if (bytes.find('\0') != std::string::npos) throw std::runtime_error("unsupported binary upload");
        const auto text_path = directory / (stamp + "-" + safe);
        std::ofstream(text_path, std::ios::binary | std::ios::trunc).write(
            bytes.data(), static_cast<std::streamsize>(bytes.size()));
        Result result;
        result.kind = "text";
        result.name = name;
        result.preview = preview(bytes);
        result.blocks.push_back(text_block(name, text_path.string(), bytes));
        return result;
    }

}
