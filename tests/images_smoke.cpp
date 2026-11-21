#include <filesystem>
#include <fstream>
#include <iostream>

import images;

int main() {
    const auto root = std::filesystem::temp_directory_path() / "sparsity-images-smoke";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto source = root / "test.ppm";
    std::ofstream output(source, std::ios::binary);
    output << "P6\n4 4\n255\n";
    for (int index = 0; index < 16; ++index) {
        output.put(static_cast<char>(index * 12));
        output.put(static_cast<char>(255 - index * 10));
        output.put(static_cast<char>(128));
    }
    output.close();
    try {
        const auto normalized = sparsity::images::normalize(source, root / "out");
        std::cout << "image " << normalized.width << 'x' << normalized.height
                  << " bytes=" << normalized.bytes << '\n';
        return std::filesystem::is_regular_file(normalized.path) ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
