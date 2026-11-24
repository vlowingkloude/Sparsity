module;

#include <algorithm>
#include <chrono>
#include <ctime>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
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

    struct Quote {
        double usd{0.0};
        bool known{false};
        bool peak{false};
        std::string model;
        std::string bucket;
        std::string version;
    };

    std::string prices_version();
    bool is_peak();
    std::string current_bucket();
    std::string canonical_model(const std::string &model);
    std::optional<ModelPrices> prices_for(const std::string &model);

    Quote quote(const llm::Usage &usage, const std::string &model);
    Quote quote(const llm::Usage &usage, const std::string &model, const std::string &bucket);

    // Worst-case estimate for budget checks: peak prices, cache miss input.
    double budget_cost(int input_tokens, int output_tokens, const std::string &model);

    // DeepSeek Search / Fetch APIs bill per successful request.
    double web_call_cost(int count);

}

namespace sparsity::pricing {

    // Update this together with the bundled table. DeepSeek has no pricing API.
    constexpr std::string_view kPricesVersion = "2026-11-24";

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

    const std::set<std::string> &holidays() {
        // Chinese public holidays for 2026. Peak pricing excludes these days.
        static const std::set<std::string> value = {
            "2026-01-01", "2026-01-02", "2026-01-03",
            "2026-02-15", "2026-02-16", "2026-02-17", "2026-02-18", "2026-02-19",
            "2026-02-20", "2026-02-21", "2026-02-22", "2026-02-23",
            "2026-04-04", "2026-04-05", "2026-04-06",
            "2026-05-01", "2026-05-02", "2026-05-03", "2026-05-04", "2026-05-05",
            "2026-06-19", "2026-06-20", "2026-06-21",
            "2026-09-25", "2026-09-26", "2026-09-27",
            "2026-10-01", "2026-10-02", "2026-10-03", "2026-10-04",
            "2026-10-05", "2026-10-06", "2026-10-07",
        };
        return value;
    }

}

export namespace sparsity::pricing {

    std::string prices_version() {
        return std::string(kPricesVersion);
    }

    bool is_peak() {
        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);
        std::tm utc{};
        gmtime_r(&time, &utc);
        if (utc.tm_wday == 0 || utc.tm_wday == 6) return false;
        char day[16];
        std::strftime(day, sizeof(day), "%Y-%m-%d", &utc);
        if (holidays().contains(day)) return false;
        return (utc.tm_hour >= 1 && utc.tm_hour < 4) || (utc.tm_hour >= 6 && utc.tm_hour < 10);
    }

    std::string current_bucket() {
        return is_peak() ? "peak" : "off";
    }

    std::string canonical_model(const std::string &model) {
        // Legacy aliases still accepted by the API and billed at Flash price.
        if (model == "deepseek-v4-flash" || model == "deepseek-v4-flash-vision-exp") {
            return "deepseek-flash";
        }
        return model;
    }

    std::optional<ModelPrices> prices_for(const std::string &model) {
        const auto canonical = canonical_model(model);
        const auto found = table().find(canonical);
        if (found == table().end()) return std::nullopt;
        return found->second;
    }

}

namespace sparsity::pricing {

    ModelPrices conservative_prices(const std::string &bucket) {
        ModelPrices result;
        result.hit[bucket] = 0.0;
        result.miss[bucket] = 0.0;
        result.output[bucket] = 0.0;
        for (const auto &[name, prices]: table()) {
            (void) name;
            result.hit[bucket] = std::max(result.hit[bucket], prices.hit.at(bucket));
            result.miss[bucket] = std::max(result.miss[bucket], prices.miss.at(bucket));
            result.output[bucket] = std::max(result.output[bucket], prices.output.at(bucket));
        }
        return result;
    }

    Quote quote_with(const llm::Usage &usage, const std::string &model, const std::string &bucket) {
        const auto canonical = canonical_model(model);
        const auto found = table().find(canonical);
        const auto peak = bucket == "peak";
        const auto effective = peak ? "peak" : "off";
        const auto prices = found == table().end() ? conservative_prices(effective) : found->second;
        const auto usd = (
            static_cast<double>(usage.cache_read) * prices.hit.at(effective) +
            static_cast<double>(usage.cache_creation) * prices.miss.at(effective) +
            static_cast<double>(usage.input_tokens) * prices.miss.at(effective) +
            static_cast<double>(usage.output_tokens) * prices.output.at(effective)
        ) / 1'000'000.0;
        return {
            .usd = usd,
            .known = found != table().end(),
            .peak = peak,
            .model = found == table().end() ? model : canonical,
            .bucket = effective,
            .version = prices_version(),
        };
    }

}

export namespace sparsity::pricing {

    Quote quote(const llm::Usage &usage, const std::string &model) {
        return quote_with(usage, model, current_bucket());
    }

    Quote quote(const llm::Usage &usage, const std::string &model, const std::string &bucket) {
        return quote_with(usage, model, bucket);
    }

    double budget_cost(const int input_tokens, const int output_tokens, const std::string &model) {
        const auto canonical = canonical_model(model);
        const auto found = table().find(canonical);
        const auto prices = found == table().end() ? conservative_prices("peak") : found->second;
        return (
            static_cast<double>(std::max(0, input_tokens)) * prices.miss.at("peak") +
            static_cast<double>(std::max(0, output_tokens)) * prices.output.at("peak")
        ) / 1'000'000.0;
    }

    double web_call_cost(const int count) {
        // $0.75 per 1,000 successful Search/Fetch requests.
        return static_cast<double>(std::max(0, count)) * 0.75 / 1'000.0;
    }

}
