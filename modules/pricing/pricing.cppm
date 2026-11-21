module;

#include <chrono>
#include <ctime>
#include <map>
#include <optional>
#include <string>
#include <utility>

export module pricing;

import llm;

export namespace sparsity::pricing {

    using Prices = std::map<std::string, double>;

    struct ModelPrices {
        Prices hit;
        Prices miss;
        Prices output;
    };

    bool is_peak();
    double estimate_cost(const llm::Usage &usage, const std::string &model);
    std::optional<ModelPrices> prices_for(const std::string &model);

}

namespace sparsity::pricing {

    const std::map<std::string, ModelPrices> &table() {
        static const std::map<std::string, ModelPrices> value = {
            {"deepseek-flash", {
                {{"off", 0.003}, {"peak", 0.006}},
                {{"off", 0.15}, {"peak", 0.30}},
                {{"off", 0.60}, {"peak", 1.20}},
            }},
            {"deepseek-v4-pro", {
                {{"off", 0.022}, {"peak", 0.044}},
                {{"off", 0.66}, {"peak", 1.32}},
                {{"off", 1.98}, {"peak", 3.96}},
            }},
        };
        return value;
    }

}

export namespace sparsity::pricing {

    bool is_peak() {
        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);
        std::tm utc{};
        gmtime_r(&time, &utc);
        if (utc.tm_wday == 0 || utc.tm_wday == 6) return false;
        return (utc.tm_hour >= 1 && utc.tm_hour < 4) || (utc.tm_hour >= 6 && utc.tm_hour < 10);
    }

    std::optional<ModelPrices> prices_for(const std::string &model) {
        const auto found = table().find(model);
        if (found == table().end()) return std::nullopt;
        return found->second;
    }

    double estimate_cost(const llm::Usage &usage, const std::string &model) {
        const auto found = table().find(model);
        if (found == table().end()) return 0.0;
        const auto bucket = is_peak() ? "peak" : "off";
        const auto &prices = found->second;
        return (
            static_cast<double>(usage.cache_read) * prices.hit.at(bucket) +
            static_cast<double>(usage.cache_creation) * prices.miss.at(bucket) +
            static_cast<double>(usage.input_tokens) * prices.miss.at(bucket) +
            static_cast<double>(usage.output_tokens) * prices.output.at(bucket)
        ) / 1'000'000.0;
    }

}
