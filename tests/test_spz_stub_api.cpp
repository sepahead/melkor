#include "melkor/spz_encoder.hpp"

#include <type_traits>

int main() {
    melkor::SpzDecoder::Metadata metadata;
    melkor::SpzDecoder::DecodeResult result;
    static_assert(std::is_same_v<decltype(result.metadata), melkor::SpzDecodeMetadata>);
    return metadata.declared_points == 0 && !result.success ? 0 : 1;
}
