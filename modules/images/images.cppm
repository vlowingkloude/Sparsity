module;

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_resize2.h>
#include <stb_image_write.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

export module images;

export namespace sparsity::images {

    struct Normalized {
        std::string path;
        int width{0};
        int height{0};
        std::size_t bytes{0};
    };

    Normalized normalize(const std::filesystem::path &source, const std::filesystem::path &dest_dir);

}

namespace sparsity::images {

    std::string hash_name(const std::vector<unsigned char> &data) {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const unsigned char byte: data) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        }
        char buffer[24];
        std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
        return buffer;
    }

    struct Buffer {
        std::vector<unsigned char> bytes;
    };

    void write_callback(void *context, void *data, const int size) {
        auto &buffer = *static_cast<Buffer *>(context);
        const auto *bytes = static_cast<unsigned char *>(data);
        buffer.bytes.insert(buffer.bytes.end(), bytes, bytes + size);
    }

}

export namespace sparsity::images {

    Normalized normalize(const std::filesystem::path &source, const std::filesystem::path &dest_dir) {
        int width = 0;
        int height = 0;
        int channels = 0;
        auto *pixels = stbi_load(source.c_str(), &width, &height, &channels, 4);
        if (pixels == nullptr) throw std::runtime_error("unsupported or corrupt image");
        if (width <= 0 || height <= 0) {
            stbi_image_free(pixels);
            throw std::runtime_error("empty image");
        }

        std::vector<unsigned char> rgb(static_cast<std::size_t>(width) * height * 3);
        for (int index = 0; index < width * height; ++index) {
            const auto alpha = pixels[index * 4 + 3];
            for (int channel = 0; channel < 3; ++channel) {
                const auto value = pixels[index * 4 + channel];
                rgb[index * 3 + channel] = static_cast<unsigned char>((value * alpha + 255 * (255 - alpha)) / 255);
            }
        }
        stbi_image_free(pixels);

        constexpr int max_side = 1568;
        int target_width = width;
        int target_height = height;
        if (std::max(width, height) > max_side) {
            const auto scale = static_cast<double>(max_side) / std::max(width, height);
            target_width = std::max(1, static_cast<int>(width * scale));
            target_height = std::max(1, static_cast<int>(height * scale));
        }

        std::vector<unsigned char> resized(static_cast<std::size_t>(target_width) * target_height * 3);
        if (target_width != width || target_height != height) {
            if (stbir_resize_uint8_linear(rgb.data(), width, height, 0, resized.data(),
                                          target_width, target_height, 0, STBIR_RGB) == nullptr) {
                throw std::runtime_error("image resize failed");
            }
        } else {
            resized = std::move(rgb);
        }

        Buffer buffer;
        for (const int quality: {85, 75, 60}) {
            buffer.bytes.clear();
            stbi_write_jpg_to_func(write_callback, &buffer, target_width, target_height, 3,
                                   resized.data(), quality);
            if (buffer.bytes.size() <= 2U * 1024U * 1024U) break;
        }
        if (buffer.bytes.empty()) throw std::runtime_error("image encode failed");

        std::error_code error;
        std::filesystem::create_directories(dest_dir, error);
        const auto destination = dest_dir / (hash_name(buffer.bytes) + ".jpg");
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("cannot write normalized image");
        output.write(reinterpret_cast<const char *>(buffer.bytes.data()),
                     static_cast<std::streamsize>(buffer.bytes.size()));
        return {destination.string(), target_width, target_height, buffer.bytes.size()};
    }

}
