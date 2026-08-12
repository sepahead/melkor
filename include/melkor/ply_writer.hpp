#ifndef MELKOR_PLY_WRITER_HPP
#define MELKOR_PLY_WRITER_HPP

#include "melkor/budget.hpp"
#include "melkor/color_space.hpp"
#include "melkor/format/loss.hpp"
#include "melkor/format/profile.hpp"
#include "melkor/limits.hpp"
#include "melkor/scene.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace melkor {

// PLY encoding options.
enum class PlyFormat {
    Binary,  // Binary little-endian.
    Ascii    // ASCII text.
};

// Configuration for PLY writing.
struct PlyWriteConfig {
    PlyFormat format = PlyFormat::Binary;

    // The canonical profile stores canonical values and writes semantic header markers.
    FormatProfileId profile = FormatProfileId::ply_melkor_canonical_v1;

    // These values describe the canonical input. Training-layout output stores them as markers.
    std::optional<ColorSpace> color_space;
    std::optional<bool> antialiased;

    // Training-layout output needs an explicit target frame. Canonical PLY uses gltf-luf.
    std::optional<std::string> target_frame_id;

    // Apply these limits before allocation and during atomic output.
    Limits limits = Limits::for_profile(LimitsProfile::desktop);

    // Replace an existing output file. The default preserves the existing file.
    bool overwrite = false;

    // Preserve spherical harmonics beyond degree 0. A false value needs loss approval.
    bool include_sh_rest = true;

    // Use -1 to keep the highest degree that the selected profile supports.
    int sh_degree = -1;

    // A write emits no bytes when it has an unapproved severe loss.
    std::vector<std::string> approved_loss_codes;

    // Optional comment for the header. The writer replaces control bytes with spaces.
    std::string comment;
};

// Result of PLY writing.
struct PlyWriteResult {
private:
    // Keep the controlled output allocation charged until this result releases it.
    Budget::Charge retained_memory_;

public:
    PlyWriteResult() = default;
    PlyWriteResult(const PlyWriteResult&) = delete;
    PlyWriteResult& operator=(const PlyWriteResult&) = delete;
    PlyWriteResult(PlyWriteResult&&) noexcept = default;
    PlyWriteResult& operator=(PlyWriteResult&&) noexcept = default;

    bool success = false;
    std::string error_message;
    std::uint64_t bytes_written = 0;
    // A successful write can include a reported representation change.
    std::vector<Diagnostic> diagnostics;
    ErrorCode failure_code = ErrorCode::invalid_data;
    FormatProfileId profile = FormatProfileId::unknown;
    LossReport losses;

    // Keep this result alive while the controlled output buffer uses its allocation.
    std::uint64_t retained_memory_bytes() const noexcept { return retained_memory_.amount(); }
    Budget::Charge take_retained_memory() noexcept { return std::move(retained_memory_); }
    void set_retained_memory(Budget::Charge charge) noexcept {
        retained_memory_ = std::move(charge);
    }

    ErrorCode error_code() const noexcept { return success ? ErrorCode::ok : failure_code; }
};

// Configuration for semantic PLY decoding.
//
// PLY does not define a coordinate frame or a color space. Training-layout PLY needs explicit
// values unless a Melkor marker supplies them. The reader never guesses these values.
struct PlyReadConfig {
    Limits limits = Limits::for_profile(LimitsProfile::desktop);
    std::optional<FormatProfileId> profile;
    std::optional<std::string> source_frame_id;
    std::optional<double> source_unit_to_meter;
    std::optional<ColorSpace> source_color_space;
};

// PLY writer for Gaussian splat data.
class PlyWriter {
public:
    PlyWriter();
    ~PlyWriter() = default;

    // Write to a file.
    PlyWriteResult writeToFile(const std::string& filepath, const SplatData& data,
                               const PlyWriteConfig& config = {});
    PlyWriteResult writeToFile(const std::string& filepath, const SplatData& data,
                               const PlyWriteConfig& config, const OperationContext& context);

    // Write to a stream.
    PlyWriteResult writeToStream(std::ostream& stream, const SplatData& data,
                                 const PlyWriteConfig& config = {});
    PlyWriteResult writeToStream(std::ostream& stream, const SplatData& data,
                                 const PlyWriteConfig& config, const OperationContext& context);

    // Write to a memory buffer.
    PlyWriteResult writeToBuffer(std::vector<uint8_t>& buffer, const SplatData& data,
                                 const PlyWriteConfig& config = {});
    PlyWriteResult writeToBuffer(std::vector<uint8_t>& buffer, const SplatData& data,
                                 const PlyWriteConfig& config, const OperationContext& context);
};

// PLY reader for Gaussian splat data.
class PlyReader {
public:
    PlyReader();
    ~PlyReader() = default;

    struct Metadata {
        enum class Encoding {
            Unknown,
            Ascii,
            BinaryLittleEndian,
            BinaryBigEndian,
        };

        Encoding encoding = Encoding::Unknown;
        std::uint64_t source_bytes = 0;
        size_t declared_vertices = 0;
        bool has_position = false;
        bool has_sh_dc = false;
        bool has_opacity = false;
        bool has_scale = false;
        bool has_rotation = false;
        bool has_sh_rest = false;
        std::uint32_t sh_degree = 0;
        FormatProfileId profile = FormatProfileId::unknown;
        std::optional<std::string> source_frame_id;
        std::optional<double> source_unit_to_meter;
        std::optional<ColorSpace> color_space;
        bool has_profile_marker = false;
        bool has_coordinate_marker = false;
        bool has_length_unit_marker = false;
        bool has_color_space_marker = false;
        std::optional<bool> antialiased;
        size_t unknown_property_count = 0;
    };

    struct ReadResult {
    private:
        // Later members are destroyed first. Release this charge after `data` is destroyed.
        Budget::Charge retained_memory_;

    public:
        ReadResult() = default;
        ReadResult(bool read_success, std::string message, std::optional<SplatData> read_data,
                   Metadata read_metadata, ErrorCode code = ErrorCode::invalid_data,
                   LossReport read_losses = {}, std::vector<Diagnostic> read_diagnostics = {})
            : success(read_success), error_message(std::move(message)), data(std::move(read_data)),
              metadata(std::move(read_metadata)), failure_code(code),
              losses(std::move(read_losses)), diagnostics(std::move(read_diagnostics)) {
            if (!success && diagnostics.empty()) {
                diagnostics.emplace_back("MK1215_PLY_READ_FAILED", Severity::error,
                                         error_message.empty() ? "the PLY reader failed"
                                                               : error_message);
            }
        }

        ReadResult(const ReadResult&) = delete;
        ReadResult& operator=(const ReadResult&) = delete;
        ReadResult(ReadResult&&) noexcept = default;
        ReadResult& operator=(ReadResult&& other) noexcept {
            if (this == &other)
                return *this;
            success = other.success;
            error_message = std::move(other.error_message);
            data = std::move(other.data);
            metadata = std::move(other.metadata);
            failure_code = other.failure_code;
            losses = std::move(other.losses);
            diagnostics = std::move(other.diagnostics);
            retained_memory_ = std::move(other.retained_memory_);
            return *this;
        }

        std::uint64_t retained_memory_bytes() const noexcept { return retained_memory_.amount(); }
        Budget::Charge take_retained_memory() noexcept { return std::move(retained_memory_); }
        void set_retained_memory(Budget::Charge charge) noexcept {
            retained_memory_ = std::move(charge);
        }

        bool success = false;
        std::string error_message;
        // Engaged on every successful read, including an empty PLY. Optional keeps the invalid
        // default state out of SplatData while preserving the reader's structured metadata.
        std::optional<SplatData> data;
        Metadata metadata;
        ErrorCode failure_code = ErrorCode::invalid_data;
        LossReport losses;
        std::vector<Diagnostic> diagnostics;

        ErrorCode error_code() const noexcept { return success ? ErrorCode::ok : failure_code; }
    };

    // Read from file. `limits` bounds resource use: the file bytes, the declared vertex count, and
    // the reconstructed canonical arrays are charged against a Budget before allocation, so a
    // well-formed header declaring an enormous count is refused by policy, not only by bad_alloc.
    ReadResult readFromFile(const std::filesystem::path& filepath,
                            const Limits& limits = Limits::for_profile(LimitsProfile::desktop));
    ReadResult readFromFile(const std::filesystem::path& filepath, const OperationContext& context);
    ReadResult readFromFile(const std::filesystem::path& filepath, const PlyReadConfig& config);
    ReadResult readFromFile(const std::filesystem::path& filepath, const PlyReadConfig& config,
                            const OperationContext& context);

    // Read from memory buffer (see readFromFile for the meaning of `limits`).
    ReadResult readFromBuffer(const uint8_t* data, size_t size,
                              const Limits& limits = Limits::for_profile(LimitsProfile::desktop));
    ReadResult readFromBuffer(const uint8_t* data, size_t size, const OperationContext& context);
    ReadResult readFromBuffer(const uint8_t* data, size_t size, const PlyReadConfig& config);
    ReadResult readFromBuffer(const uint8_t* data, size_t size, const PlyReadConfig& config,
                              const OperationContext& context);

private:
    ReadResult readFromBufferImpl(const uint8_t* data, size_t size, const OperationContext& context,
                                  const PlyReadConfig& config, bool input_is_charged);
};

}  // namespace melkor

#endif  // MELKOR_PLY_WRITER_HPP
