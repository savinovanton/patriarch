#include "pat/sim/save.hpp"
#include "pat/stress/dataset.hpp"

#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr const char* kUsage = R"(Usage: stressgen --output FILE [options]
  --profile year1|year60|year60x3  Synthetic snapshot (default: year60)
  --seed N                       Reproducible unsigned 64-bit seed (default: 1)
  --provinces N --characters N --dynasties N
  --armies N --schemes N --contracts N --events N
  --help                         Show this help

Counts override the profile regardless of argument order. Both --name N and --name=N work.
Writes an E1 PSV1 save and FILE.psl with the synthetic workload, then a JSON summary to stdout.
)";

int fail(const std::string& message) {
    std::fprintf(stderr, "stressgen: %s\n", message.c_str());
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace pat;
    std::string_view profile = "year60";
    std::filesystem::path output;
    std::vector<std::pair<std::string_view, std::string_view>> flags;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            std::fputs(kUsage, stdout);
            return 0;
        }
        const auto equals = arg.find('=');
        const auto name = arg.substr(0, equals);
        if (name != "--profile" && name != "--output" && name != "--seed" && name != "--provinces" &&
            name != "--characters" && name != "--dynasties" && name != "--armies" && name != "--schemes" &&
            name != "--contracts" && name != "--events") {
            return fail("unknown option: " + std::string(name));
        }
        std::string_view value;
        if (equals != std::string_view::npos) {
            value = arg.substr(equals + 1);
        } else if (i + 1 < argc) {
            value = argv[++i];
        }
        if (value.empty()) {
            return fail("missing value for " + std::string(name));
        }
        if (name == "--profile") {
            profile = value;
        } else if (name == "--output") {
            output = value;
        } else {
            flags.emplace_back(name, value);
        }
    }
    auto preset = stress::profile_config(profile);
    if (!preset) {
        return fail(preset.error().message);
    }
    auto config = preset.value();
    for (const auto& [name, text] : flags) {
        std::uint64_t value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size()) {
            return fail(std::string(name) + " requires an unsigned integer");
        }
        if (name == "--seed") {
            config.world.seed = value;
            continue;
        }
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            return fail(std::string(name) + " is too large");
        }
        auto count = static_cast<std::uint32_t>(value);
        if (name == "--provinces") {
            config.world.provinces = count;
        } else if (name == "--characters") {
            config.world.characters = count;
        } else if (name == "--dynasties") {
            config.world.dynasties = count;
        } else if (name == "--armies") {
            config.armies = count;
        } else if (name == "--schemes") {
            config.schemes = count;
        } else if (name == "--contracts") {
            config.contracts = count;
        } else if (name == "--events") {
            config.events = count;
        }
    }
    if (output.empty()) {
        return fail("--output FILE is required (see --help)");
    }
    const auto start = std::chrono::steady_clock::now();
    auto generated = stress::generate(config);
    const auto stop = std::chrono::steady_clock::now();
    if (!generated) {
        return fail(generated.error().message);
    }
    const auto& dataset = generated.value();
    const auto save = sim::serialize_save(dataset.world);
    const auto workload = stress::serialize_workload(dataset.world, dataset.workload);
    if (const auto result = sim::write_file_atomic(output, save); !result) {
        return fail(result.error().message);
    }
    auto companion = output;
    companion += ".psl";
    if (const auto result = sim::write_file_atomic(companion, workload); !result) {
        return fail(result.error().message);
    }
    const auto milliseconds = std::chrono::duration<double, std::milli>(stop - start).count();
    const auto world_hash = dataset.world.compute_hash().total;
    const auto hash = stress::compute_hash(dataset);
    std::printf("{\"profile\":\"%.*s\",\"seed\":%llu,\"date\":\"%s\","
                "\"provinces\":%u,\"characters\":%u,\"dynasties\":%u,"
                "\"armies\":%u,\"schemes\":%u,\"contracts\":%u,\"events\":%u,"
                "\"world_hash\":\"%016llx\",\"dataset_hash\":\"%016llx\","
                "\"generation_ms\":%.3f,\"save_bytes\":%zu,\"workload_bytes\":%zu}\n",
                static_cast<int>(profile.size()),
                profile.data(),
                static_cast<unsigned long long>(config.world.seed),
                sim::to_string(dataset.world.today()).c_str(),
                config.world.provinces,
                config.world.characters,
                config.world.dynasties,
                config.armies,
                config.schemes,
                config.contracts,
                config.events,
                static_cast<unsigned long long>(world_hash),
                static_cast<unsigned long long>(hash),
                milliseconds,
                save.size(),
                workload.size());
    return 0;
}
