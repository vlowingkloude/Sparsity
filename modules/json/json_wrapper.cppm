module;

#include <nlohmann/json.hpp>

export module json;

export namespace sparsity::json {
    using json_type = nlohmann::json;
}