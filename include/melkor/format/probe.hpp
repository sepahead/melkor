// Container probing.
//
// A filename suffix identifies, at most, a container -- and often not even that, because a file
// can be misnamed. So Melkor probes the actual bytes. This is the *container* probe: it answers
// "what kind of file is this?" from magic bytes, with a confidence, and it is deliberately
// separate from semantic-profile probing ("is this Graphdeco 3DGS or a generic point cloud?"),
// which requires reading structure, not just the first few bytes.
//
// Honesty about ambiguity is the point. An SPZ file is a gzip stream, and its own NGSP magic
// lives *inside* the decompressed data, so from the first bytes alone an SPZ file is
// indistinguishable from any other gzip file. The probe says `spz` with `low` confidence in that
// case rather than pretending certainty; confirming it means decompressing a bounded prefix,
// which is a deeper, budgeted step.

#ifndef MELKOR_FORMAT_PROBE_HPP
#define MELKOR_FORMAT_PROBE_HPP

#include "melkor/format/format_id.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace melkor {

enum class Confidence : std::uint8_t {
    none = 0,     // nothing recognised
    low = 1,      // a plausible but non-exclusive signal (a gzip stream, which might be SPZ)
    high = 2,     // a distinctive magic (glTF's "glTF", PLY's "ply")
    certain = 3,  // a full structural match; only a deeper probe can grant this
};

const char* to_string(Confidence confidence) noexcept;

struct ContainerProbe {
    FormatId format = FormatId::unknown;
    Confidence confidence = Confidence::none;
    // Human-readable reasons the probe reached its conclusion, for the inspection report. A
    // consumer never has to guess why a format was chosen.
    std::vector<std::string> evidence;
};

// Probes a bounded prefix of the file's bytes. Reads only what the magic requires -- it never
// needs the whole file -- so it is cheap and safe on an untrusted input. Passing fewer bytes than
// a magic needs simply lowers the confidence; it never reads past `size`.
ContainerProbe probe_container(const std::uint8_t* data, std::size_t size);

// Whether the probed container is consistent with a filename suffix. A mismatch (a `.ply` file
// whose bytes are glTF) is a warning in inspection and an error in conversion unless the user
// forces the format explicitly. This does not itself decide; it reports, so the caller applies
// the right policy for its mode.
bool suffix_matches(FormatId probed, const std::string& suffix_lowercase);

}  // namespace melkor

#endif  // MELKOR_FORMAT_PROBE_HPP
