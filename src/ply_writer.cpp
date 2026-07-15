#include "melkor/ply_writer.hpp"

#include "melkor/budget.hpp"
#include "melkor/io/atomic_writer.hpp"
#include "melkor/limits.hpp"
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <cstring>
#include <iomanip>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>

namespace melkor {
namespace {

void writeFloatLittleEndian(std::ostream& stream, float value) {
    static_assert(sizeof(float) == sizeof(uint32_t), "PLY requires IEEE-754 32-bit floats");
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const char bytes[4] = {
        static_cast<char>(bits & 0xffu),
        static_cast<char>((bits >> 8u) & 0xffu),
        static_cast<char>((bits >> 16u) & 0xffu),
        static_cast<char>((bits >> 24u) & 0xffu),
    };
    stream.write(bytes, sizeof(bytes));
}

} // namespace

PlyWriter::PlyWriter() = default;
PlyWriter::~PlyWriter() = default;

PlyWriteResult PlyWriter::writeToFile(const std::string& filepath,
                                      const GaussianCloud& cloud,
                                      const PlyWriteConfig& config) {
    // Route through the atomic writer. Opening the destination directly with std::ofstream
    // truncates it on open, so a failure partway through a write left the user with a
    // half-written file where their good one used to be (P0-08).
    //
    // The bytes stream into an exclusively-created temporary in the same directory; the
    // destination is replaced atomically only after the write fully succeeds. Streaming --
    // rather than buffering the result and writing it in one go -- matters here: a
    // 25-million-splat PLY is several gigabytes, and trading a data-loss bug for an
    // out-of-memory bug would not be a fix.
    melkor::Budget budget(melkor::Limits::for_profile(melkor::LimitsProfile::desktop));
    melkor::OperationContext context = melkor::make_default_context(budget);

    melkor::io::WriteOptions options;
    // The legacy entry point has no --force plumbing yet; CLI v2 (WP15) makes overwrite an
    // explicit user decision. Preserving the historical replace-the-output behaviour keeps
    // this a pure data-safety fix rather than a silent contract change.
    options.overwrite = true;

    auto writer = melkor::io::AtomicWriter::create(filepath, options, context);
    if (!writer.has_value()) {
        const std::string message = writer.diagnostics().empty()
                                        ? "Failed to open file for writing: " + filepath
                                        : writer.diagnostics()[0].message;
        return {false, message, 0};
    }

    PlyWriteResult result;
    {
        melkor::io::AtomicOutputStream stream(*writer.value());
        result = writeToStream(stream, cloud, config);

        stream.flush();

        // An ostream swallows write failures by design. Committing after one would install a
        // silently truncated file, so the stream's own error state is checked explicitly
        // rather than trusting that writeToStream noticed.
        if (stream.failed()) {
            const auto& diagnostics = stream.diagnostics();
            return {false,
                    diagnostics.empty() ? "Failed writing PLY data" : diagnostics[0].message, 0};
        }
    }

    if (!result.success) {
        // The destination is untouched; the temporary is removed by the destructor.
        return result;
    }

    auto committed = writer.value()->commit();
    if (!committed.has_value()) {
        return {false,
                committed.diagnostics().empty() ? "Failed to commit PLY file"
                                                : committed.diagnostics()[0].message,
                0};
    }

    result.bytes_written = writer.value()->bytes_written();
    return result;
}

PlyWriteResult PlyWriter::writeToStream(std::ostream& stream,
                                        const GaussianCloud& cloud,
                                        const PlyWriteConfig& config) {
    PlyWriteResult result;
    result.success = true;
    
    // Calculate SH coefficients count based on degree
    int sh_rest_count = 0;
    if (config.include_sh_rest && cloud.shDegree() > 0) {
        // Degree 1: 9 coefficients (3 per band), Degree 2: 24, Degree 3: 45
        // Minus 3 for DC which is stored separately
        switch (cloud.shDegree()) {
            case 1: sh_rest_count = 9; break;
            case 2: sh_rest_count = 24; break;
            case 3: sh_rest_count = 45; break;
            default: sh_rest_count = 0;
        }
    }
    
    // Write header
    std::ostringstream header;
    header << "ply\n";
    if (config.format == PlyFormat::Binary) {
        header << "format binary_little_endian 1.0\n";
    } else {
        header << "format ascii 1.0\n";
    }
    
    if (!config.comment.empty()) {
        header << "comment " << config.comment << "\n";
    }
    
    header << "element vertex " << cloud.size() << "\n";
    
    // Position
    header << "property float x\n";
    header << "property float y\n";
    header << "property float z\n";
    
    // Normals (dummy, required by some viewers)
    header << "property float nx\n";
    header << "property float ny\n";
    header << "property float nz\n";
    
    // DC spherical harmonics (color)
    header << "property float f_dc_0\n";
    header << "property float f_dc_1\n";
    header << "property float f_dc_2\n";
    
    // Rest of spherical harmonics if enabled
    for (int i = 0; i < sh_rest_count; ++i) {
        header << "property float f_rest_" << i << "\n";
    }
    
    // Opacity
    header << "property float opacity\n";
    
    // Scale
    header << "property float scale_0\n";
    header << "property float scale_1\n";
    header << "property float scale_2\n";
    
    // Rotation (quaternion)
    header << "property float rot_0\n";
    header << "property float rot_1\n";
    header << "property float rot_2\n";
    header << "property float rot_3\n";
    
    header << "end_header\n";
    
    std::string header_str = header.str();
    stream.write(header_str.c_str(), header_str.size());
    result.bytes_written = header_str.size();
    
    // Write data
    const auto& splats = cloud.splats();
    
    if (config.format == PlyFormat::Binary) {
        // Binary format
        for (const auto& splat : splats) {
            // Position
            writeFloatLittleEndian(stream, splat.x);
            writeFloatLittleEndian(stream, splat.y);
            writeFloatLittleEndian(stream, splat.z);
            
            // Normals (dummy values)
            float nx = 0.0f, ny = 0.0f, nz = 1.0f;
            writeFloatLittleEndian(stream, nx);
            writeFloatLittleEndian(stream, ny);
            writeFloatLittleEndian(stream, nz);
            
            // DC color
            writeFloatLittleEndian(stream, splat.f_dc_0);
            writeFloatLittleEndian(stream, splat.f_dc_1);
            writeFloatLittleEndian(stream, splat.f_dc_2);
            
            // SH rest coefficients
            for (int i = 0; i < sh_rest_count; ++i) {
                float val = (i < static_cast<int>(splat.sh_rest.size())) ? splat.sh_rest[i] : 0.0f;
                writeFloatLittleEndian(stream, val);
            }
            
            // Opacity
            writeFloatLittleEndian(stream, splat.opacity);
            
            // Scale
            writeFloatLittleEndian(stream, splat.scale_0);
            writeFloatLittleEndian(stream, splat.scale_1);
            writeFloatLittleEndian(stream, splat.scale_2);
            
            // Rotation
            writeFloatLittleEndian(stream, splat.rot_0);
            writeFloatLittleEndian(stream, splat.rot_1);
            writeFloatLittleEndian(stream, splat.rot_2);
            writeFloatLittleEndian(stream, splat.rot_3);
            
            result.bytes_written += (6 + 3 + sh_rest_count + 1 + 3 + 4) * sizeof(float);
        }
    } else {
        // ASCII format — track bytes written via stream position
        stream << std::setprecision(8);
        auto pos_before = stream.tellp();
        for (const auto& splat : splats) {
            stream << splat.x << " " << splat.y << " " << splat.z << " ";
            stream << "0 0 1 ";  // dummy normals
            stream << splat.f_dc_0 << " " << splat.f_dc_1 << " " << splat.f_dc_2 << " ";
            
            for (int i = 0; i < sh_rest_count; ++i) {
                float val = (i < static_cast<int>(splat.sh_rest.size())) ? splat.sh_rest[i] : 0.0f;
                stream << val << " ";
            }
            
            stream << splat.opacity << " ";
            stream << splat.scale_0 << " " << splat.scale_1 << " " << splat.scale_2 << " ";
            stream << splat.rot_0 << " " << splat.rot_1 << " " << splat.rot_2 << " " << splat.rot_3 << "\n";
        }
        auto pos_after = stream.tellp();
        if (pos_after != decltype(pos_after)(-1) && pos_before != decltype(pos_before)(-1)) {
            result.bytes_written += static_cast<size_t>(pos_after - pos_before);
        }
    }
    
    if (!stream.good()) {
        result.success = false;
        result.error_message = "Stream error during write";
    }
    
    return result;
}

PlyWriteResult PlyWriter::writeToBuffer(std::vector<uint8_t>& buffer,
                                        const GaussianCloud& cloud,
                                        const PlyWriteConfig& config) {
    std::ostringstream stream(std::ios::binary);
    auto result = writeToStream(stream, cloud, config);
    
    if (result.success) {
        std::string str = stream.str();
        buffer.assign(str.begin(), str.end());
        result.bytes_written = buffer.size();
    }
    
    return result;
}

// PLY Reader implementation
PlyReader::PlyReader() = default;
PlyReader::~PlyReader() = default;

PlyReader::ReadResult PlyReader::readFromFile(const std::string& filepath) try {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        return {false, "Failed to open file: " + filepath, {}, {}};
    }
    
    // Reject malformed/header-bomb inputs before allocating for the entire
    // file. Valid payloads still decode in memory because the public result is
    // a materialized GaussianCloud, but an unrelated sparse multi-GB file no
    // longer consumes its full size merely to discover a bad magic line.
    file.seekg(0, std::ios::end);
    const std::streamoff end = file.tellg();
    if (end <= 0 || static_cast<uintmax_t>(end) > std::numeric_limits<size_t>::max() ||
        static_cast<uintmax_t>(end) > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        return {false, "Invalid or oversized PLY file", {}, {}};
    }
    const size_t size = static_cast<size_t>(end);
    file.seekg(0, std::ios::beg);

    constexpr size_t kMaxHeaderBytes = 1024 * 1024;
    const size_t prefix_size = std::min(size, kMaxHeaderBytes);
    std::vector<uint8_t> prefix(prefix_size);
    if (!file.read(reinterpret_cast<char*>(prefix.data()),
                   static_cast<std::streamsize>(prefix_size))) {
        return {false, "Failed to read PLY header", {}, {}};
    }
    const std::string_view prefix_view(reinterpret_cast<const char*>(prefix.data()), prefix.size());
    if (!(prefix_view.rfind("ply\n", 0) == 0 || prefix_view.rfind("ply\r\n", 0) == 0)) {
        return {false, "Invalid PLY: missing ply magic line", {}, {}};
    }
    const bool has_header_end = prefix_view.find("\nend_header\n") != std::string_view::npos ||
                                prefix_view.find("\nend_header\r\n") != std::string_view::npos;
    if (!has_header_end && size > prefix_size) {
        return {false, "Invalid PLY: header exceeds 1 MiB limit", {}, {}};
    }

    std::vector<uint8_t> buffer;
    try {
        buffer.resize(size);
    } catch (const std::bad_alloc&) {
        return {false, "PLY file exceeds available memory", {}, {}};
    }
    file.clear();
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(size))) {
        return {false, "Failed to read complete PLY file", {}, {}};
    }
    
    return readFromBuffer(buffer.data(), buffer.size());
} catch (const std::bad_alloc&) {
    return {false, "PLY file exceeds available memory", {}, {}};
} catch (const std::length_error&) {
    return {false, "PLY file exceeds a container size limit", {}, {}};
}

PlyReader::ReadResult PlyReader::readFromBuffer(const uint8_t* data, size_t size) try {
    if (data == nullptr || size == 0) {
        return {false, "Invalid PLY: input buffer is null or empty", {}, {}};
    }
    ReadResult result;
    result.success = true;

    // Canonical scalar type table. PLY 1.0 allows both the classic names
    // (char/uchar/short/ushort/int/uint/float/double) and the sized aliases
    // (int8/uint8/int16/uint16/int32/uint32/float32/float64); both spellings
    // must resolve to the same size and decoding, otherwise stride computation
    // and binary decoding silently corrupt valid files.
    enum class PropKind { F32, F64, I8, U8, I16, U16, I32, U32, Unknown };
    struct PropType { PropKind kind; size_t size; };
    auto prop_type = [](const std::string& t) -> PropType {
        if (t == "float"  || t == "float32") return {PropKind::F32, 4};
        if (t == "double" || t == "float64") return {PropKind::F64, 8};
        if (t == "uchar"  || t == "uint8")   return {PropKind::U8,  1};
        if (t == "char"   || t == "int8")    return {PropKind::I8,  1};
        if (t == "ushort" || t == "uint16")  return {PropKind::U16, 2};
        if (t == "short"  || t == "int16")   return {PropKind::I16, 2};
        if (t == "uint"   || t == "uint32")  return {PropKind::U32, 4};
        if (t == "int"    || t == "int32")   return {PropKind::I32, 4};
        return {PropKind::Unknown, 4};  // unknown name: assume a 4-byte float
    };

    // Parse the header line by line. The header ends at the first line whose
    // content is exactly "end_header" (with an optional trailing CR for CRLF
    // files); the data section starts immediately after that line's newline.
    // Scanning whole lines (instead of a raw substring find) means CRLF
    // headers are accepted and a comment that merely contains "end_header"
    // cannot truncate the header early.
    //
    // The reader is header-driven: we map each property name to its index
    // within the vertex record, so PLYs authored by different tools (e.g. those
    // using red/green/blue instead of f_dc_*, or omitting normals) are parsed
    // correctly instead of silently misaligned.
    struct Property {
        std::string name;
        PropType type;
        size_t offset;  // byte offset within one binary vertex record
    };
    std::vector<Property> vertex_props;
    size_t vertex_count = 0;
    bool is_binary = false;
    bool is_big_endian = false;
    bool in_vertex = false;
    bool saw_format = false;
    bool saw_vertex = false;
    std::vector<std::string> element_names;

    std::string_view view(reinterpret_cast<const char*>(data), size);
    size_t header_bytes = 0;
    {
        bool found_end = false;
        size_t line_start = 0;
        size_t line_number = 0;
        while (line_start < size) {
            size_t nl = view.find('\n', line_start);
            if (nl == std::string_view::npos) break;  // header lines must end in '\n'
            std::string line(view.substr(line_start, nl - line_start));
            // Strip trailing CR.
            if (!line.empty() && line.back() == '\r') line.pop_back();
            line_start = nl + 1;
            if (line_number++ == 0) {
                if (line != "ply") {
                    return {false, "Invalid PLY: missing ply magic line", {}, {}};
                }
                continue;
            }
            if (line == "end_header") {
                header_bytes = line_start;
                found_end = true;
                break;
            }
            if (line.rfind("format ", 0) == 0) {
                if (saw_format) {
                    return {false, "Invalid PLY: duplicate format declaration", {}, {}};
                }
                saw_format = true;
                if (line == "format ascii 1.0") {
                    is_binary = false;
                    is_big_endian = false;
                } else if (line == "format binary_little_endian 1.0") {
                    is_binary = true;
                    is_big_endian = false;
                } else if (line == "format binary_big_endian 1.0") {
                    is_binary = true;
                    is_big_endian = true;
                } else {
                    return {false, "Invalid PLY: unsupported format declaration", {}, {}};
                }
            } else if (line.rfind("element ", 0) == 0) {
                std::istringstream declaration(line);
                std::string keyword;
                std::string element_name;
                std::string count_token;
                std::string extra;
                declaration >> keyword >> element_name >> count_token >> extra;
                if (keyword != "element" || element_name.empty() || count_token.empty() ||
                    !extra.empty()) {
                    return {false, "Invalid PLY: malformed element declaration", {}, {}};
                }
                if (std::find(element_names.begin(), element_names.end(), element_name) !=
                    element_names.end()) {
                    return {false, "Invalid PLY: duplicate element declaration", {}, {}};
                }
                element_names.push_back(element_name);

                size_t element_count = 0;
                const char* begin = count_token.data();
                const char* end = begin + count_token.size();
                const auto parsed = std::from_chars(begin, end, element_count);
                if (parsed.ec != std::errc{} || parsed.ptr != end) {
                    return {false, "Invalid PLY: malformed element count in header", {}, {}};
                }

                in_vertex = element_name == "vertex";
                if (in_vertex) {
                    saw_vertex = true;
                    vertex_count = element_count;
                } else if (!saw_vertex && element_count != 0) {
                    // This reader intentionally extracts only vertex records.
                    // Without decoding the preceding element schema, treating
                    // its first record as a vertex would silently corrupt both
                    // ASCII rows and binary strides.
                    return {false,
                            "Invalid PLY: non-vertex data precedes the vertex element",
                            {}, {}};
                }
            } else if (line.rfind("property ", 0) == 0 && in_vertex) {
                std::istringstream property(line);
                std::string keyword;
                std::string type_name;
                std::string property_name;
                std::string extra;
                property >> keyword >> type_name >> property_name >> extra;
                if (keyword != "property" || type_name.empty() || property_name.empty() ||
                    !extra.empty()) {
                    return {false, "Invalid PLY: malformed vertex property", {}, {}};
                }
                // A list has a data-dependent record width. Skipping it would
                // misalign every later binary field, so fail closed.
                if (type_name == "list") {
                    return {false, "Invalid PLY: vertex list properties are unsupported", {}, {}};
                }
                const PropType type = prop_type(type_name);
                if (type.kind == PropKind::Unknown) {
                    return {false, "Invalid PLY: unsupported scalar property type " + type_name,
                            {}, {}};
                }
                if (std::any_of(vertex_props.begin(), vertex_props.end(),
                                [&](const Property& existing) {
                                    return existing.name == property_name;
                                })) {
                    return {false, "Invalid PLY: duplicate vertex property " + property_name,
                            {}, {}};
                }
                vertex_props.push_back({property_name, type, 0});
            }
            // comment/obj_info and unrecognized lines are skipped.
        }
        if (!found_end) {
            return {false, "Invalid PLY: no end_header found", {}, {}};
        }
        if (!saw_format) {
            return {false, "Invalid PLY: missing format declaration", {}, {}};
        }
        if (!saw_vertex) {
            return {false, "Invalid PLY: no vertex element found", {}, {}};
        }
    }

    result.metadata.declared_vertices = vertex_count;
    result.metadata.encoding = is_big_endian
        ? Metadata::Encoding::BinaryBigEndian
        : is_binary ? Metadata::Encoding::BinaryLittleEndian
                    : Metadata::Encoding::Ascii;

    if (vertex_count == 0) {
        // An empty cloud is a valid PLY (the writer emits one for empty input,
        // and DA3 save_ply can produce one when all samples are filtered out).
        // Return success with an empty cloud rather than treating it as an
        // error, so round-trips and downstream consumers don't break on the
        // degenerate-but-legal case.
        result.success = true;
        return result;
    }
    if (vertex_props.empty()) {
        return {false, "Invalid PLY: no vertex properties found", {}, {}};
    }

    // Resolve the canonical splat fields by name. Both 3DGS (f_dc_0/...)
    // and plain RGB (red/green/blue) color conventions are accepted; if both
    // are present, 3DGS SH-DC takes precedence.
    auto find_idx = [&](const std::string& name) -> int {
        for (size_t i = 0; i < vertex_props.size(); ++i) {
            if (vertex_props[i].name == name) return static_cast<int>(i);
        }
        return -1;
    };
    int ix = find_idx("x"), iy = find_idx("y"), iz = find_idx("z");
    int ifdc0 = find_idx("f_dc_0"), ifdc1 = find_idx("f_dc_1"), ifdc2 = find_idx("f_dc_2");
    int ir = find_idx("red"), ig = find_idx("green"), ib = find_idx("blue");
    int iopacity = find_idx("opacity");
    int is0 = find_idx("scale_0"), is1 = find_idx("scale_1"), is2 = find_idx("scale_2");
    int ir0 = find_idx("rot_0"), ir1 = find_idx("rot_1"), ir2 = find_idx("rot_2"), ir3 = find_idx("rot_3");

    result.metadata.has_position = ix >= 0 && iy >= 0 && iz >= 0;
    result.metadata.has_sh_dc = ifdc0 >= 0 && ifdc1 >= 0 && ifdc2 >= 0;
    result.metadata.has_rgb = ir >= 0 && ig >= 0 && ib >= 0;
    result.metadata.has_opacity = iopacity >= 0;
    result.metadata.has_scale = is0 >= 0 && is1 >= 0 && is2 >= 0;
    result.metadata.has_rotation = ir0 >= 0 && ir1 >= 0 && ir2 >= 0 && ir3 >= 0;

    // Gather f_rest_* indices by their numeric suffix. Property order is not
    // semantically significant, but suffixes must uniquely and contiguously
    // describe one supported SH degree; accepting gaps or duplicates silently
    // corrupts view-dependent color.
    std::vector<int> sh_rest_idx;
    std::vector<std::pair<size_t, int>> indexed_sh_rest;
    for (size_t property_index = 0; property_index < vertex_props.size(); ++property_index) {
        const auto& p = vertex_props[property_index];
        if (p.name.rfind("f_rest_", 0) == 0) {
            const std::string suffix = p.name.substr(7);
            size_t coefficient = 0;
            const char* begin = suffix.data();
            const char* end = begin + suffix.size();
            const auto parsed = std::from_chars(begin, end, coefficient);
            if (suffix.empty() || parsed.ec != std::errc{} || parsed.ptr != end ||
                coefficient > 44) {
                return {false, "Invalid PLY: malformed f_rest coefficient name", {}, {}};
            }
            if (std::any_of(indexed_sh_rest.begin(), indexed_sh_rest.end(),
                            [&](const auto& item) { return item.first == coefficient; })) {
                return {false, "Invalid PLY: duplicate f_rest coefficient", {}, {}};
            }
            indexed_sh_rest.emplace_back(coefficient, static_cast<int>(property_index));
        }
    }
    if (!indexed_sh_rest.empty()) {
        std::sort(indexed_sh_rest.begin(), indexed_sh_rest.end());
        const size_t coefficient_count = indexed_sh_rest.size();
        if (coefficient_count != 9 && coefficient_count != 24 && coefficient_count != 45) {
            return {false, "Invalid PLY: f_rest count does not match SH degree 1, 2, or 3",
                    {}, {}};
        }
        for (size_t coefficient = 0; coefficient < coefficient_count; ++coefficient) {
            if (indexed_sh_rest[coefficient].first != coefficient) {
                return {false, "Invalid PLY: f_rest coefficients must be contiguous from zero",
                        {}, {}};
            }
            sh_rest_idx.push_back(indexed_sh_rest[coefficient].second);
        }
        result.metadata.has_sh_rest = true;
        result.cloud.setShDegree([&] {
            switch (static_cast<int>(coefficient_count)) {
                case 9: return 1;
                case 24: return 2;
                case 45: return 3;
                default: return 0;
            }
        }());
    }

    if (ix < 0 || iy < 0 || iz < 0) {
        return {false, "Invalid PLY: missing x/y/z position properties", {}, {}};
    }

    // Record stride and per-property byte offsets from the canonical type
    // sizes (used consistently for validation and binary decoding).
    size_t stride = 0;
    for (auto& p : vertex_props) {
        p.offset = stride;
        stride += p.type.size;
    }

    // Validate the declared vertex count against what the remaining buffer
    // could possibly hold before reserving, so a malformed count fails cleanly
    // instead of causing a huge allocation or an out-of-bounds read. All
    // arithmetic is done as 64-bit division to avoid count*stride overflow.
    const size_t remaining = size - header_bytes;
    if (is_binary) {
        if (stride == 0 || vertex_count > remaining / stride) {
            return {false, "Invalid PLY: data section too small for declared vertex count", {}, {}};
        }
    } else {
        // ASCII data size is not stride-predictable, but each value needs at
        // least one character plus a separator, so one vertex record cannot
        // occupy fewer than 2*props - 1 bytes. Cap the count accordingly.
        const size_t min_record = 2 * vertex_props.size() - 1;
        if (vertex_count > remaining / min_record + 1) {
            return {false, "Invalid PLY: declared vertex count exceeds data size", {}, {}};
        }
    }

    result.cloud.reserve(vertex_count);
    const uint8_t* vertex_data = data + header_bytes;

    // True when the file's byte order differs from the host's; the host order
    // is probed at runtime so the swap stays generic instead of assuming a
    // little-endian machine.
    const bool host_is_little = [] {
        const uint16_t probe = 1;
        uint8_t first;
        std::memcpy(&first, &probe, 1);
        return first == 1;
    }();
    const bool needs_swap = (is_big_endian == host_is_little);

    // Read a single property value (at the given property index) for the
    // current vertex record starting at `base`, normalized to float. Multi-byte
    // values are byte-swapped as needed for the file's declared endianness.
    auto read_prop = [&](const uint8_t* base, int prop_idx) -> float {
        const auto& p = vertex_props[static_cast<size_t>(prop_idx)];
        uint8_t raw[8];
        std::memcpy(raw, base + p.offset, p.type.size);
        if (needs_swap && p.type.size > 1) {
            std::reverse(raw, raw + p.type.size);
        }
        switch (p.type.kind) {
            case PropKind::F64: { double d;   std::memcpy(&d, raw, 8); return static_cast<float>(d); }
            case PropKind::I8:  return static_cast<float>(static_cast<int8_t>(raw[0]));
            case PropKind::U8:  return static_cast<float>(raw[0]);
            case PropKind::I16: { int16_t s;  std::memcpy(&s, raw, 2); return static_cast<float>(s); }
            case PropKind::U16: { uint16_t u; std::memcpy(&u, raw, 2); return static_cast<float>(u); }
            case PropKind::I32: { int32_t v;  std::memcpy(&v, raw, 4); return static_cast<float>(v); }
            case PropKind::U32: { uint32_t u; std::memcpy(&u, raw, 4); return static_cast<float>(u); }
            case PropKind::F32:
            case PropKind::Unknown:
            default:            { float f;    std::memcpy(&f, raw, 4); return f; }
        }
    };

    // Colour normalisation depends on the DECLARED SOURCE TYPE, never on the property name.
    //
    // This is the fix for P0-07. The reader previously divided every red/green/blue value by
    // 255 unconditionally, as though every PLY stored colour as an 8-bit byte. A point cloud
    // authored with `property float red` holds a value already in [0,1]; dividing it by 255
    // turned mid-grey (0.5) into 0.00196 -- essentially black -- and the scene rendered unlit.
    //
    // The scaling that actually reconstructs a value in [0,1]:
    //   - unsigned 8-bit  : divide by 255
    //   - unsigned 16-bit : divide by 65535
    //   - unsigned 32-bit : divide by 4294967295
    //   - float / double  : already normalised; divide by nothing
    //
    // Signed integer colour has no single conventional mapping (is -1 the minimum, or an HDR
    // value?), so we normalise by the type's unsigned-width maximum rather than guess a signed
    // convention; a profile-aware reader (WP07) will reject it explicitly instead. float/double
    // pass straight through, which is the whole point of this change.
    auto color_to_shdc = [](float c, PropKind kind) {
        float divisor = 1.0f;
        switch (kind) {
            case PropKind::U8:
            case PropKind::I8:  divisor = 255.0f; break;
            case PropKind::U16:
            case PropKind::I16: divisor = 65535.0f; break;
            case PropKind::U32:
            case PropKind::I32: divisor = 4294967295.0f; break;
            case PropKind::F32:
            case PropKind::F64:
            case PropKind::Unknown:
            default:            divisor = 1.0f; break;  // already in [0,1]
        }
        return utils::rgbToShDc(c / divisor);
    };

    auto parse_ascii_value = [](const std::string& token, PropKind kind,
                                float& output) -> bool {
        if (token.empty()) return false;
        char* end = nullptr;
        errno = 0;
        switch (kind) {
            case PropKind::F32: {
                const float value = std::strtof(token.c_str(), &end);
                if (end != token.c_str() + token.size() || errno == ERANGE) return false;
                output = value;
                return true;
            }
            case PropKind::F64: {
                const double value = std::strtod(token.c_str(), &end);
                if (end != token.c_str() + token.size() || errno == ERANGE) return false;
                if (std::isfinite(value) &&
                    (std::abs(value) > std::numeric_limits<float>::max() ||
                     (value != 0.0 && std::abs(value) < std::numeric_limits<float>::denorm_min()))) {
                    return false;
                }
                output = static_cast<float>(value);
                return true;
            }
            case PropKind::U8:
            case PropKind::U16:
            case PropKind::U32: {
                if (token.front() == '-') return false;
                const unsigned long long value = std::strtoull(token.c_str(), &end, 10);
                const unsigned long long maximum = kind == PropKind::U8
                    ? std::numeric_limits<uint8_t>::max()
                    : kind == PropKind::U16 ? std::numeric_limits<uint16_t>::max()
                                            : std::numeric_limits<uint32_t>::max();
                if (end != token.c_str() + token.size() || errno == ERANGE || value > maximum) {
                    return false;
                }
                output = static_cast<float>(value);
                return true;
            }
            case PropKind::I8:
            case PropKind::I16:
            case PropKind::I32: {
                const long long value = std::strtoll(token.c_str(), &end, 10);
                const long long minimum = kind == PropKind::I8
                    ? std::numeric_limits<int8_t>::min()
                    : kind == PropKind::I16 ? std::numeric_limits<int16_t>::min()
                                           : std::numeric_limits<int32_t>::min();
                const long long maximum = kind == PropKind::I8
                    ? std::numeric_limits<int8_t>::max()
                    : kind == PropKind::I16 ? std::numeric_limits<int16_t>::max()
                                           : std::numeric_limits<int32_t>::max();
                if (end != token.c_str() + token.size() || errno == ERANGE ||
                    value < minimum || value > maximum) {
                    return false;
                }
                output = static_cast<float>(value);
                return true;
            }
            case PropKind::Unknown:
            default: return false;
        }
    };

    if (is_binary) {
        for (size_t i = 0; i < vertex_count; ++i) {
            const uint8_t* base = vertex_data + i * stride;
            GaussianSplat splat;
            splat.x = read_prop(base, ix);
            splat.y = read_prop(base, iy);
            splat.z = read_prop(base, iz);

            if (ifdc0 >= 0 && ifdc1 >= 0 && ifdc2 >= 0) {
                splat.f_dc_0 = read_prop(base, ifdc0);
                splat.f_dc_1 = read_prop(base, ifdc1);
                splat.f_dc_2 = read_prop(base, ifdc2);
            } else if (ir >= 0 && ig >= 0 && ib >= 0) {
                splat.f_dc_0 = color_to_shdc(read_prop(base, ir),
                                            vertex_props[static_cast<size_t>(ir)].type.kind);
                splat.f_dc_1 = color_to_shdc(read_prop(base, ig),
                                            vertex_props[static_cast<size_t>(ig)].type.kind);
                splat.f_dc_2 = color_to_shdc(read_prop(base, ib),
                                            vertex_props[static_cast<size_t>(ib)].type.kind);
            } else {
                splat.f_dc_0 = splat.f_dc_1 = splat.f_dc_2 = utils::rgbToShDc(0.5f);
            }

            if (!sh_rest_idx.empty()) {
                splat.sh_rest.resize(sh_rest_idx.size());
                for (size_t j = 0; j < sh_rest_idx.size(); ++j) {
                    splat.sh_rest[j] = read_prop(base, sh_rest_idx[j]);
                }
            }

            splat.opacity = (iopacity >= 0) ? read_prop(base, iopacity) : utils::logit(0.9f);
            splat.scale_0 = (is0 >= 0) ? read_prop(base, is0) : std::log(0.01f);
            splat.scale_1 = (is1 >= 0) ? read_prop(base, is1) : std::log(0.01f);
            splat.scale_2 = (is2 >= 0) ? read_prop(base, is2) : std::log(0.01f);
            splat.rot_0 = (ir0 >= 0) ? read_prop(base, ir0) : 1.0f;
            splat.rot_1 = (ir1 >= 0) ? read_prop(base, ir1) : 0.0f;
            splat.rot_2 = (ir2 >= 0) ? read_prop(base, ir2) : 0.0f;
            splat.rot_3 = (ir3 >= 0) ? read_prop(base, ir3) : 0.0f;

            result.cloud.addSplat(std::move(splat));
        }
    } else {
        std::istringstream stream(std::string(reinterpret_cast<const char*>(vertex_data),
                                              size - header_bytes));
        std::vector<float> vals(vertex_props.size());
        std::string token;
        for (size_t i = 0; i < vertex_count; ++i) {
            std::string record_line;
            if (!std::getline(stream, record_line)) {
                return {false, "Invalid PLY: missing ASCII record for vertex " +
                    std::to_string(i), {}, {}};
            }
            if (!record_line.empty() && record_line.back() == '\r') record_line.pop_back();
            std::istringstream record(record_line);
            for (size_t j = 0; j < vertex_props.size(); ++j) {
                if (!(record >> token) ||
                    !parse_ascii_value(token, vertex_props[j].type.kind, vals[j])) {
                    return {false, "Invalid PLY: malformed scalar at vertex " +
                        std::to_string(i), {}, {}};
                }
            }
            if (record >> token) {
                return {false, "Invalid PLY: extra scalar at vertex " +
                    std::to_string(i), {}, {}};
            }
            GaussianSplat splat;
            splat.x = vals[ix];
            splat.y = vals[iy];
            splat.z = vals[iz];

            if (ifdc0 >= 0 && ifdc1 >= 0 && ifdc2 >= 0) {
                splat.f_dc_0 = vals[ifdc0];
                splat.f_dc_1 = vals[ifdc1];
                splat.f_dc_2 = vals[ifdc2];
            } else if (ir >= 0 && ig >= 0 && ib >= 0) {
                splat.f_dc_0 = color_to_shdc(vals[ir],
                                            vertex_props[static_cast<size_t>(ir)].type.kind);
                splat.f_dc_1 = color_to_shdc(vals[ig],
                                            vertex_props[static_cast<size_t>(ig)].type.kind);
                splat.f_dc_2 = color_to_shdc(vals[ib],
                                            vertex_props[static_cast<size_t>(ib)].type.kind);
            } else {
                splat.f_dc_0 = splat.f_dc_1 = splat.f_dc_2 = utils::rgbToShDc(0.5f);
            }

            if (!sh_rest_idx.empty()) {
                splat.sh_rest.resize(sh_rest_idx.size());
                for (size_t j = 0; j < sh_rest_idx.size(); ++j) {
                    splat.sh_rest[j] = vals[sh_rest_idx[j]];
                }
            }

            splat.opacity = (iopacity >= 0) ? vals[iopacity] : utils::logit(0.9f);
            splat.scale_0 = (is0 >= 0) ? vals[is0] : std::log(0.01f);
            splat.scale_1 = (is1 >= 0) ? vals[is1] : std::log(0.01f);
            splat.scale_2 = (is2 >= 0) ? vals[is2] : std::log(0.01f);
            splat.rot_0 = (ir0 >= 0) ? vals[ir0] : 1.0f;
            splat.rot_1 = (ir1 >= 0) ? vals[ir1] : 0.0f;
            splat.rot_2 = (ir2 >= 0) ? vals[ir2] : 0.0f;
            splat.rot_3 = (ir3 >= 0) ? vals[ir3] : 0.0f;

            result.cloud.addSplat(std::move(splat));
        }
    }

    return result;
} catch (const std::bad_alloc&) {
    return {false, "PLY buffer exceeds available memory", {}, {}};
} catch (const std::length_error&) {
    return {false, "PLY buffer exceeds a container size limit", {}, {}};
}

} // namespace melkor
