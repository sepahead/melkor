// Format registry and conversion planner.

#ifndef MELKOR_FORMAT_REGISTRY_HPP
#define MELKOR_FORMAT_REGISTRY_HPP

#include "melkor/budget.hpp"
#include "melkor/color_space.hpp"
#include "melkor/format/format_id.hpp"
#include "melkor/format/loss.hpp"
#include "melkor/format/profile.hpp"
#include "melkor/provenance.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace melkor {

struct AssetReadOptions {
    std::optional<FormatId> format;
    std::optional<FormatProfileId> profile;
    std::optional<std::string> source_frame_id;
    std::optional<double> source_unit_to_meter;
    std::optional<ColorSpace> source_color_space;
};

struct AssetSourceMetadata {
    std::string encoding = "unknown";
    std::uint64_t source_bytes = 0;
    std::uint64_t declared_splats = 0;
};

struct AssetReadResult {
private:
    // Later members are destroyed first. Release the charge after the asset is destroyed.
    Budget::Charge retained_memory_;

public:
    AssetReadResult(SplatPrimitive primitive_value, LossReport loss_value, FormatId format_value,
                    FormatProfileId profile_value, AssetSourceMetadata source_value,
                    Budget::Charge retained_memory) noexcept
        : retained_memory_(std::move(retained_memory)), primitive(std::move(primitive_value)),
          losses(std::move(loss_value)), format(format_value), profile(profile_value),
          source(std::move(source_value)) {}

    AssetReadResult(const AssetReadResult&) = delete;
    AssetReadResult& operator=(const AssetReadResult&) = delete;
    AssetReadResult(AssetReadResult&&) noexcept = default;
    AssetReadResult& operator=(AssetReadResult&& other) noexcept {
        if (this == &other)
            return *this;
        primitive = std::move(other.primitive);
        losses = std::move(other.losses);
        format = other.format;
        profile = other.profile;
        source = std::move(other.source);
        retained_memory_ = std::move(other.retained_memory_);
        return *this;
    }

    SplatPrimitive primitive;
    LossReport losses;
    FormatId format = FormatId::unknown;
    FormatProfileId profile = FormatProfileId::unknown;
    AssetSourceMetadata source;
};

struct AssetWriteOptions {
    std::optional<FormatId> format;
    std::optional<FormatProfileId> profile;
    std::optional<std::string> target_frame_id;
    std::optional<bool> antialiased;
    int sh_degree = -1;
    bool ascii = false;
    bool overwrite = false;
    std::vector<std::string> approved_loss_codes;
};

struct ConversionRequest {
    std::filesystem::path input;
    std::filesystem::path output;
    AssetReadOptions read;
    AssetWriteOptions write;
};

struct ConversionResult {
    FormatId input_format = FormatId::unknown;
    FormatId output_format = FormatId::unknown;
    FormatProfileId input_profile = FormatProfileId::unknown;
    FormatProfileId output_profile = FormatProfileId::unknown;
    std::uint64_t splat_count = 0;
    std::uint64_t bytes_written = 0;
    LossReport losses;
};

// Run after output staging and loss validation, but before the atomic commit.
using ConversionCommitGate = std::function<Result<void>(const ConversionResult&)>;

// Probe and decode one local asset into the canonical primitive model.
Result<AssetReadResult> read_splat_asset(const std::filesystem::path& path,
                                         const AssetReadOptions& options,
                                         const OperationContext& context);

// Read, plan, encode, and atomically commit one conversion.
// A failed commit gate leaves the destination unchanged.
Result<ConversionResult> convert_file(const ConversionRequest& request,
                                      const OperationContext& context,
                                      const ConversionCommitGate& before_commit = {});

}  // namespace melkor

#endif  // MELKOR_FORMAT_REGISTRY_HPP
