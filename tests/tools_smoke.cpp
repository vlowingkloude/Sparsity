#include <filesystem>
#include <fstream>
#include <iostream>
#include <utility>

import json;
import llm;
import config;
import tools;

using namespace sparsity;

int main() {
    const auto root = std::filesystem::temp_directory_path() / "sparsity-tools-smoke";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    std::ofstream(root / "a.txt") << "alpha\nbeta\ngamma\n";

    config::Config cfg;
    cfg.permissions.mode = "auto";
    tools::BashSession shell(root);
    tools::ToolContext context{root, root, root, cfg, &shell};
    tools::ToolExecutor executor(tools::make_tools(), cfg, context,
        [](const tools::ApprovalCard &) { return tools::Approval{}; });

    auto call = [](std::string name, json::json_type input) {
        llm::ToolCall value;
        value.id = "call";
        value.name = std::move(name);
        value.input = std::move(input);
        return value;
    };

    const auto read = executor.execute(call("read", {{"path", "a.txt"}}));
    const auto write = executor.execute(call("write", {{"path", "b.txt"}, {"content", "one\ntwo\n"}}));
    const auto edit = executor.execute(call("edit", {{"path", "b.txt"}, {"old_string", "two"}, {"new_string", "three"}}));
    const auto bash = executor.execute(call("bash", {{"command", "echo hello && pwd"}}));
    const auto glob = executor.execute(call("glob", {{"pattern", "*.txt"}}));
    const auto grep = executor.execute(call("grep", {{"pattern", "three"}, {"case_sensitive", true}}));

    const bool ok = read.text.find("alpha") != std::string::npos &&
                    write.text.find("Wrote") != std::string::npos &&
                    edit.text.find("Edited") != std::string::npos &&
                    bash.text.find("hello") != std::string::npos &&
                    glob.text.find("b.txt") != std::string::npos &&
                    grep.text.find("three") != std::string::npos;
    std::cout << (ok ? "tools ok\n" : "tools failed\n");
    return ok ? 0 : 1;
}
