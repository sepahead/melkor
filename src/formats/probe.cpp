#include "melkor/format/probe.hpp"

#include <cstring>

namespace melkor {

const char* to_string(Confidence confidence) noexcept {
    switch (confidence) {
        case Confidence::none:
            return "none";
        case Confidence::low:
            return "low";
        case Confidence::high:
            return "high";
        case Confidence::certain:
            return "certain";
    }
    return "none";
}

namespace {

bool starts_with(const std::uint8_t* data, std::size_t size, const char* magic,
                 std::size_t magic_len) {
    return size >= magic_len && std::memcmp(data, magic, magic_len) == 0;
}

}  // namespace

ContainerProbe probe_container(const std::uint8_t* data, std::size_t size) {
    ContainerProbe result;
    if (data == nullptr || size == 0) {
        result.evidence.emplace_back("empty input");
        return result;
    }

    // glTF binary: the 4-byte magic "glTF". Distinctive, so high confidence.
    if (starts_with(data, size, "glTF", 4)) {
        result.format = FormatId::glb;
        result.confidence = Confidence::high;
        result.evidence.emplace_back("GLB magic 'glTF' at offset 0");
        return result;
    }

    // PLY: the ASCII header begins with "ply" followed by a newline. Distinctive.
    if (starts_with(data, size, "ply\n", 4) || starts_with(data, size, "ply\r", 4)) {
        result.format = FormatId::ply;
        result.confidence = Confidence::high;
        result.evidence.emplace_back("PLY header 'ply' at offset 0");
        return result;
    }

    // A leading '{' or whitespace-then-'{' suggests a JSON glTF (.gltf), but JSON is used by many
    // things, so this is only a low-confidence signal that a deeper structural probe must confirm.
    {
        std::size_t i = 0;
        while (i < size && (data[i] == ' ' || data[i] == '\t' || data[i] == '\n' || data[i] == '\r')) {
            ++i;
        }
        if (i < size && data[i] == '{') {
            result.format = FormatId::gltf;
            result.confidence = Confidence::low;
            result.evidence.emplace_back("leading '{' suggests JSON glTF; needs structural confirmation");
            return result;
        }
    }

    // SPZ is a gzip stream (magic 0x1f 0x8b). Its own NGSP magic lives inside the decompressed
    // data, so from the raw bytes an SPZ file is indistinguishable from any other gzip file. Say
    // 'spz' with LOW confidence, honestly: confirming it means decompressing a bounded prefix,
    // which is a deeper, budgeted step the probe does not do here.
    if (size >= 2 && data[0] == 0x1f && data[1] == 0x8b) {
        result.format = FormatId::spz;
        result.confidence = Confidence::low;
        result.evidence.emplace_back(
            "gzip magic 0x1f8b; may be SPZ, confirm by decompressing a bounded prefix and checking "
            "the NGSP magic");
        return result;
    }

    result.evidence.emplace_back("no recognised container magic");
    return result;
}

bool suffix_matches(FormatId probed, const std::string& suffix_lowercase) {
    switch (probed) {
        case FormatId::ply:
            return suffix_lowercase == ".ply";
        case FormatId::spz:
            return suffix_lowercase == ".spz";
        case FormatId::gltf:
            return suffix_lowercase == ".gltf";
        case FormatId::glb:
            return suffix_lowercase == ".glb";
        case FormatId::unknown:
            return false;
    }
    return false;
}

}  // namespace melkor
