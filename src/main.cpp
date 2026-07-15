#include "melkor/version.h"

#include "melkor/backend_registry.hpp"

#include "melkor/compute_provider.hpp"
#include "melkor/cloud_inspector.hpp"
#include "melkor/densifier.hpp"
#include "melkor/glb_reader.hpp"
#include "melkor/ply_writer.hpp"
#include "melkor/spz_encoder.hpp"
#include "melkor/enhanced_converter.hpp"
#include "inspect_command.hpp"
#include "convert_command.hpp"
#include "safe_text.hpp"

#include <iostream>
#include <string>
#include <chrono>
#include <filesystem>
#include <cerrno>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <new>
#include <stdexcept>

namespace fs = std::filesystem;

void printUsage(const char* program) {
    std::cout << "Melkor - Advanced Gaussian Splatting Toolkit\n";
    std::cout << "\nUsage: ";
    melkor::text::writeDisplayString(std::cout, program);
    std::cout << " <input.glb> <output.ply|output.spz> [options]\n       ";
    melkor::text::writeDisplayString(std::cout, program);
    std::cout << " inspect <input> [--json] [--strict]\n       ";
    melkor::text::writeDisplayString(std::cout, program);
    std::cout << " convert <input.glb> <output.glb> [--allow-loss CODE] [--limits-profile P]\n";
    std::cout << "\nSupported conversions:\n";
    std::cout << "  GLB -> PLY    Convert GLB mesh to Gaussian splatting PLY\n";
    std::cout << "  GLB -> SPZ    Convert GLB mesh to compressed SPZ format\n";
    std::cout << "  PLY -> SPZ    Convert PLY to compressed SPZ format\n";
    std::cout << "  SPZ -> PLY    Convert SPZ to PLY format\n";
    std::cout << "  inspect       Validate and report PLY/SPZ/GLB/glTF metadata without GPU init\n";
    std::cout << "\nConversion Modes:\n";
    std::cout << "  --basic              Basic vertex-to-splat conversion (default, fast)\n";
    std::cout << "  --enhanced           Enhanced conversion with adaptive scale & surface alignment\n";
    std::cout << "\nNeural reconstruction:\n";
    std::cout << "  Use ./da3-infer after running scripts/setup_da3.sh. The native CLI only\n";
    std::cout << "  performs deterministic format conversion.\n";
    std::cout << "\nBasic/Enhanced Options:\n";
    std::cout << "  --scale <float>      Scale factor for splat size (default: 0.01)\n";
    std::cout << "  --opacity <float>    Default opacity 0-1 (default: 1.0)\n";
    std::cout << "  --pos-scale <float>  Position scale factor (default: 1.0)\n";
    std::cout << "  --no-coord-convert   Don't convert Y-up to Z-up\n";
    std::cout << "\nEnhanced Mode Options:\n";
    std::cout << "  --knn <int>          K neighbors for density estimation (default: 8)\n";
    std::cout << "  --no-surface-align   Disable surface-aligned Gaussians\n";
    std::cout << "\nScene Completion Options (any splat input: PLY/SPZ/GLB):\n";
    std::cout << "  --fill-holes         Densify sparse regions and bridge interior holes\n";
    std::cout << "                       (3DGS densification, aka scene completion/inpainting)\n";
    std::cout << "  --fill-iterations <int>  Advancing-front passes (default: 3)\n";
    std::cout << "  --fill-strength <float>  Fill spacing in units of median splat spacing;\n";
    std::cout << "                       lower is denser (default: 1.0)\n";
    std::cout << "  --max-hole-size <float>  Largest bridgeable hole, in multiples of the\n";
    std::cout << "                       median splat spacing (default: 8.0)\n";
    std::cout << "\nGeneral Options:\n";
    std::cout << "  --ascii              Output ASCII PLY instead of binary\n";
    std::cout << "  --no-gpu             Disable GPU acceleration (use CPU)\n";
    std::cout << "  --no-metal           Alias for --no-gpu (deprecated)\n";
    std::cout << "  --info               Show GPU info and exit\n";
    std::cout << "  --list-models        List supported DA3 reconstruction checkpoints\n";
    std::cout << "  --version            Show version and exit\n";
    std::cout << "  -h, --help           Show this help\n";
}

void printDeviceInfo() {
    auto provider = melkor::ComputeProvider::create();
    if (!provider || !provider->isInitialized()) {
        std::cout << "No compute backend available.\n";
        return;
    }

    auto info = provider->deviceInfo();
    std::cout << "GPU Information:\n";
    std::cout << "  Backend: " << provider->backendName() << "\n";
    std::cout << "  Name: " << info.name << "\n";
    if (info.total_memory > 0) {
        std::cout << "  Memory: " << (info.total_memory / (1024*1024)) << " MB\n";
    }
    if (info.max_threads > 0) {
        std::cout << "  Max Threads: " << info.max_threads << "\n";
    }
    if (info.compute_capability_major > 0) {
        std::cout << "  Compute Capability: " << info.compute_capability_major
                  << "." << info.compute_capability_minor << "\n";
    }
    if (provider->backend() == melkor::ComputeBackend::CPU) {
        std::cout << "  Note: Use OpenSplat with CUDA on Linux for GPU training\n";
    }
}

std::string getExtension(const std::string& path) {
    auto pos = path.rfind('.');
    if (pos == std::string::npos) return "";
    std::string ext = path.substr(pos);
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext;
}

enum class ConversionMode {
    Basic,
    Enhanced
};

int main(int argc, char* argv[]) try {
    // Populate the backend registry.
    //
    // melkor_core owns the registry but knows nothing about Metal or CUDA; melkor_runtime is
    // the only layer allowed to name them, and this call is what connects the two. It is an
    // explicit call rather than a static initializer because a self-registering backend in a
    // static library is silently stripped by the linker when nothing else in its translation
    // unit is referenced -- and the symptom would be "my GPU is not detected", reported by a
    // user months later on a build nobody tested.
    melkor::register_builtin_backends();

    if (argc >= 2 && std::string(argv[1]) == "inspect") {
        return melkor::cli::runInspectCommand(argc - 2, argv + 2, argv[0]);
    }
    if (argc >= 2 && std::string(argv[1]) == "convert") {
        return melkor::cli::runConvertCommand(argc - 2, argv + 2, argv[0]);
    }
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    // Parse arguments
    std::string input_path;
    std::string output_path;
    float splat_scale = 0.01f;
    float opacity = 1.0f;
    float pos_scale = 1.0f;
    bool convert_coords = true;
    bool use_binary = true;
    bool use_gpu = true;
    bool show_info = false;
    bool list_models = false;

    // Mode selection
    ConversionMode mode = ConversionMode::Basic;
    bool mode_explicit = false;

    // Enhanced mode options
    int knn_neighbors = 8;
    bool surface_align = true;

    // Scene completion options
    bool fill_holes = false;
    int fill_iterations = 3;
    float fill_strength = 1.0f;
    float max_hole_size = 8.0f;

    // Parse complete, finite tokens. std::stof/std::stoi accept a valid prefix
    // (for example "1oops") and comparisons alone do not reject NaN.
    auto parseFloat = [&](const char* flag, const char* val, float& out) -> bool {
        if (!val || *val == '\0') {
            std::cerr << "Error: Invalid number for " << flag << ": \"";
            melkor::text::writeDisplayString(std::cerr, val ? std::string(val) : std::string());
            std::cerr << "\"\n";
            return false;
        }
        errno = 0;
        char* end = nullptr;
        const float parsed = std::strtof(val, &end);
        if (end == val || *end != '\0' || errno == ERANGE || !std::isfinite(parsed)) {
            std::cerr << "Error: Invalid finite number for " << flag << ": \"";
            melkor::text::writeDisplayString(std::cerr, val);
            std::cerr << "\"\n";
            return false;
        }
        out = parsed;
        return true;
    };
    auto parseInt = [&](const char* flag, const char* val, int& out) -> bool {
        if (!val || *val == '\0') {
            std::cerr << "Error: Invalid integer for " << flag << ": \"";
            melkor::text::writeDisplayString(std::cerr, val ? std::string(val) : std::string());
            std::cerr << "\"\n";
            return false;
        }
        int parsed = 0;
        const char* end = val + std::char_traits<char>::length(val);
        const auto parsed_result = std::from_chars(val, end, parsed);
        if (parsed_result.ec != std::errc{} || parsed_result.ptr != end) {
            std::cerr << "Error: Invalid integer for " << flag << ": \"";
            melkor::text::writeDisplayString(std::cerr, val);
            std::cerr << "\"\n";
            return false;
        }
        out = parsed;
        return true;
    };
    auto requireValue = [&](int index, const std::string& flag) -> bool {
        if (index + 1 < argc) return true;
        std::cerr << "Error: Missing value for ";
        melkor::text::writeDisplayString(std::cerr, flag);
        std::cerr << '\n';
        return false;
    };
    auto selectMode = [&](ConversionMode selected, const char* flag) -> bool {
        if (mode_explicit && mode != selected) {
            std::cerr << "Error: Conflicting conversion mode: " << flag << "\n";
            return false;
        }
        mode = selected;
        mode_explicit = true;
        return true;
    };

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--version") {
            std::cout << "melkor " << MELKOR_VERSION_STRING << "\n";
            return 0;
        } else if (arg == "--info") {
            show_info = true;
        } else if (arg == "--list-models") {
            list_models = true;
        } else if (arg == "--basic") {
            if (!selectMode(ConversionMode::Basic, "--basic")) return 1;
        } else if (arg == "--enhanced") {
            if (!selectMode(ConversionMode::Enhanced, "--enhanced")) return 1;
        } else if (arg == "--fit") {
            std::cerr << "Error: --fit is unavailable because the previous implementation did not\n"
                         "perform optimization. Use OpenSplat for trained fitting.\n";
            return 1;
        } else if (arg == "--feedforward") {
            std::cerr << "Error: Native --feedforward has been retired because it did not provide\n"
                         "model-correct adapters. Run ./da3-infer instead.\n";
            return 1;
        } else if (arg == "--download-model" || arg == "--model") {
            std::cerr << "Error: ";
            melkor::text::writeDisplayString(std::cerr, arg);
            std::cerr << " belongs to the retired native feedforward facade.\n"
                         "Run scripts/setup_da3.sh and use ./da3-infer.\n";
            return 1;
        } else if (arg == "--iterations" || arg == "--views" || arg == "--resolution") {
            std::cerr << "Error: ";
            melkor::text::writeDisplayString(std::cerr, arg);
            std::cerr << " belongs to the unavailable --fit mode.\n";
            return 1;
        } else if (arg == "--scale") {
            if (!requireValue(i, arg)) return 1;
            if (!parseFloat("--scale", argv[++i], splat_scale)) return 1;
        } else if (arg == "--opacity") {
            if (!requireValue(i, arg)) return 1;
            if (!parseFloat("--opacity", argv[++i], opacity)) return 1;
        } else if (arg == "--pos-scale") {
            if (!requireValue(i, arg)) return 1;
            if (!parseFloat("--pos-scale", argv[++i], pos_scale)) return 1;
        } else if (arg == "--no-coord-convert") {
            convert_coords = false;
        } else if (arg == "--knn") {
            if (!requireValue(i, arg)) return 1;
            if (!parseInt("--knn", argv[++i], knn_neighbors)) return 1;
        } else if (arg == "--no-surface-align") {
            surface_align = false;
        } else if (arg == "--fill-holes") {
            fill_holes = true;
        } else if (arg == "--fill-iterations") {
            if (!requireValue(i, arg)) return 1;
            if (!parseInt("--fill-iterations", argv[++i], fill_iterations)) return 1;
        } else if (arg == "--fill-strength") {
            if (!requireValue(i, arg)) return 1;
            if (!parseFloat("--fill-strength", argv[++i], fill_strength)) return 1;
        } else if (arg == "--max-hole-size") {
            if (!requireValue(i, arg)) return 1;
            if (!parseFloat("--max-hole-size", argv[++i], max_hole_size)) return 1;
        } else if (arg == "--ascii") {
            use_binary = false;
        } else if (arg == "--no-gpu" || arg == "--no-metal") {
            use_gpu = false;
        } else if (arg[0] != '-') {
            if (input_path.empty()) {
                input_path = arg;
            } else if (output_path.empty()) {
                output_path = arg;
            } else {
                std::cerr << "Error: Unexpected extra positional argument: ";
                melkor::text::writeDisplayString(std::cerr, arg);
                std::cerr << '\n';
                return 1;
            }
        } else {
            std::cerr << "Error: Unknown option: ";
            melkor::text::writeDisplayString(std::cerr, arg);
            std::cerr << '\n';
            return 1;
        }
    }

    // Validate numeric options: out-of-range values produce NaN scales
    // (0/0 in k-NN averaging) or degenerate fits rather than clean errors.
    if (knn_neighbors < 1 || knn_neighbors > 1024) {
        std::cerr << "Error: --knn must be in [1, 1024] (got " << knn_neighbors << ")\n";
        return 1;
    }
    if (splat_scale <= 0.0f || pos_scale <= 0.0f) {
        std::cerr << "Error: --scale and --pos-scale must be > 0\n";
        return 1;
    }
    if (opacity <= 0.0f || opacity > 1.0f) {
        std::cerr << "Error: --opacity must be in (0, 1] (got " << opacity << ")\n";
        return 1;
    }
    if (fill_iterations < 1) {
        std::cerr << "Error: --fill-iterations must be >= 1\n";
        return 1;
    }
    if (fill_strength <= 0.0f || max_hole_size <= 0.0f) {
        std::cerr << "Error: --fill-strength and --max-hole-size must be > 0\n";
        return 1;
    }

    if (show_info) {
        printDeviceInfo();
        return 0;
    }

    if (list_models) {
        std::cout << "Supported da3-infer reconstruction checkpoints:\n"
                     "  da3-small, da3-base (Apache-2.0)\n"
                     "  da3-large-1.1, da3-giant-1.1, da3nested-giant-large-1.1 "
                     "(noncommercial)\n"
                     "Run scripts/setup_da3.sh, then ./da3-infer --help.\n";
        return 0;
    }

    if (input_path.empty() || output_path.empty()) {
        std::cerr << "Error: Input and output paths required.\n";
        printUsage(argv[0]);
        return 1;
    }

    // Check input exists
    if (!fs::exists(input_path)) {
        std::cerr << "Error: Input file not found: ";
        melkor::text::writeDisplayString(std::cerr, input_path);
        std::cerr << '\n';
        return 1;
    }

    std::string input_ext = getExtension(input_path);
    std::string output_ext = getExtension(output_path);

    auto start_time = std::chrono::high_resolution_clock::now();

    // Initialize compute backend
    std::unique_ptr<melkor::ComputeProvider> provider;
    if (use_gpu) {
        provider = melkor::ComputeProvider::create();
    } else {
        provider = melkor::ComputeProvider::create(melkor::ComputeBackend::CPU);
    }

    if (provider && provider->isInitialized()) {
        auto info = provider->deviceInfo();
        std::cout << "Using " << provider->backendName() << " acceleration: " << info.name << "\n";
    } else {
        std::cout << "No GPU acceleration available, using CPU\n";
        provider = melkor::ComputeProvider::create(melkor::ComputeBackend::CPU);
    }

    // No raw context handle is extracted here any more. Everything a caller needs is on the
    // ComputeProvider interface, so the provider is simply passed along; nothing in this file
    // knows or cares which backend it happens to be.

    melkor::GaussianCloud cloud;

    // Load input
    if (input_ext == ".glb" || input_ext == ".gltf") {
        std::cout << "Loading GLB: ";
        melkor::text::writeDisplayString(std::cout, input_path);
        std::cout << '\n';

        // Handle different conversion modes for GLB input
        if (mode == ConversionMode::Enhanced) {
            std::cout << "Using enhanced conversion mode\n";

            melkor::EnhancedConverter converter(provider.get());
            melkor::EnhancedConversionConfig config;
            config.use_gpu = use_gpu;
            config.knn_neighbors = knn_neighbors;
            // The enhanced path multiplies its k-NN-derived local spacing by this factor to
            // get each splat's base scale. The default splat_scale (0.01) maps to the
            // converter's default multiplier (0.5); the historical constant that produced that
            // mapping is named here rather than left as a magic literal (P0-11). The principled
            // replacement is the mesh-init surface sampler's scale_multiplier * local_spacing.
            constexpr float kEnhancedScaleMultiplierPerUnit = 50.0f;
            config.scale_factor = splat_scale * kEnhancedScaleMultiplierPerUnit;
            config.use_surface_alignment = surface_align;
            config.default_opacity = opacity;
            config.position_scale = pos_scale;
            config.convert_coordinate_system = convert_coords;

            auto result = converter.convertFromFile(input_path, config);
            if (!result.success) {
                std::cerr << "Error: ";
                melkor::text::writeDisplayString(std::cerr, result.error_message);
                std::cerr << '\n';
                return 1;
            }

            cloud = std::move(result.cloud);
            std::cout << "Enhanced conversion: " << result.original_vertices << " vertices -> "
                      << result.output_splats << " splats\n";
            std::cout << "Average scale: " << result.avg_scale << "\n";

        } else {
            // Basic mode (original implementation)
            melkor::GlbReader reader;
            melkor::GlbConversionConfig config;
            config.default_scale = splat_scale;
            config.default_opacity = opacity;
            config.position_scale = pos_scale;
            config.convert_coordinate_system = convert_coords;

            auto result = reader.loadFromFile(input_path, config);
            if (!result.success) {
                std::cerr << "Error loading GLB: ";
                melkor::text::writeDisplayString(std::cerr, result.error_message);
                std::cerr << '\n';
                return 1;
            }

            cloud = std::move(result.cloud);
            std::cout << "Loaded " << cloud.size() << " vertices from "
                      << result.total_meshes << " meshes\n";
        }

    } else if (input_ext == ".ply") {
        std::cout << "Loading PLY: ";
        melkor::text::writeDisplayString(std::cout, input_path);
        std::cout << '\n';

        melkor::PlyReader reader;
        auto result = reader.readFromFile(input_path);
        if (!result.success) {
            std::cerr << "Error loading PLY: ";
            melkor::text::writeDisplayString(std::cerr, result.error_message);
            std::cerr << '\n';
            return 1;
        }

        cloud = std::move(result.cloud);
        std::cout << "Loaded " << cloud.size() << " splats\n";

    } else if (input_ext == ".spz") {
#ifdef MELKOR_HAS_SPZ
        std::cout << "Loading SPZ: ";
        melkor::text::writeDisplayString(std::cout, input_path);
        std::cout << '\n';

        melkor::SpzDecoder decoder;
        auto result = decoder.decodeFromFile(input_path);
        if (!result.success) {
            std::cerr << "Error loading SPZ: ";
            melkor::text::writeDisplayString(std::cerr, result.error_message);
            std::cerr << '\n';
            return 1;
        }

        cloud = std::move(result.cloud);
        std::cout << "Loaded " << cloud.size() << " splats\n";
#else
        std::cerr << "Error: SPZ support not compiled. Rebuild with SPZ library.\n";
        return 1;
#endif
    } else {
        std::cerr << "Error: Unsupported input format: ";
        melkor::text::writeDisplayString(std::cerr, input_ext);
        std::cerr << '\n';
        return 1;
    }

    const auto validateCloud = [&](const char* phase) {
        const melkor::CloudInspection inspection = melkor::inspectLegacyCloud(cloud);
        if (inspection.valid) return true;
        std::cerr << "Error: " << phase << " cloud failed numeric validation\n";
        for (const auto& issue : inspection.issues) {
            if (issue.severity != melkor::InspectionSeverity::Error) continue;
            std::cerr << "  [";
            melkor::text::writeDisplayString(std::cerr, issue.code);
            std::cerr << "] ";
            melkor::text::writeDisplayString(std::cerr, issue.message);
            if (issue.count > 1) {
                std::cerr << " (" << issue.count << " occurrences)";
            }
            if (issue.has_index) {
                std::cerr << " First splat: " << issue.first_index << '.';
            }
            std::cerr << '\n';
        }
        return false;
    };

    // Treat decoded clouds as untrusted data before any GPU kernel, spatial
    // operation, or quantizing encoder sees them. In particular, SPZ position
    // packing converts floats to integers, which is undefined for NaN/Inf.
    if (!validateCloud("input")) return 1;

    // Scene completion: densify sparse regions and bridge interior holes.
    if (fill_holes && !cloud.empty()) {
        std::cout << "Scene completion (densification)...\n";
        melkor::Densifier densifier(provider.get());
        melkor::DensifyConfig fill_config;
        fill_config.k_neighbors = knn_neighbors;
        fill_config.max_iterations = fill_iterations;
        fill_config.spacing_multiplier = fill_strength;
        fill_config.max_hole_size = max_hole_size;
        fill_config.use_gpu = use_gpu;

        auto fill_stats = densifier.fillHoles(cloud, fill_config);
        std::cout << "Scene completion: +" << fill_stats.added << " splats in "
                  << fill_stats.passes << " pass(es), median spacing "
                  << fill_stats.median_spacing << "\n";
    }

    // A conversion that produced nothing is an error, not an empty output
    // file with exit code 0.
    if (cloud.empty()) {
        std::cerr << "Error: no splats produced from input\n";
        return 1;
    }

    // Process with compute backend if available (normalize quaternions)
    if (provider && !cloud.empty()) {
        std::cout << "Processing with " << provider->backendName() << "...\n";
        provider->normalizeQuaternions(cloud);
    }

    // Re-check after every mutating processing stage so a densifier/backend
    // regression cannot hand invalid numeric data to either output writer.
    if (!validateCloud("processed")) return 1;

    // Write output
    if (output_ext == ".ply") {
        std::cout << "Writing PLY: ";
        melkor::text::writeDisplayString(std::cout, output_path);
        std::cout << '\n';

        melkor::PlyWriter writer;
        melkor::PlyWriteConfig config;
        config.format = use_binary ? melkor::PlyFormat::Binary : melkor::PlyFormat::Ascii;
        // Preserve higher-order SH when the cloud carries it, matching the
        // SPZ path (which encodes up to cloud.shDegree()). Without this a
        // degree>0 input silently loses all view-dependent color on PLY output.
        config.include_sh_rest = cloud.shDegree() > 0;

        auto result = writer.writeToFile(output_path, cloud, config);
        if (!result.success) {
            std::cerr << "Error writing PLY: ";
            melkor::text::writeDisplayString(std::cerr, result.error_message);
            std::cerr << '\n';
            return 1;
        }

        std::cout << "Written " << result.bytes_written << " bytes\n";

    } else if (output_ext == ".spz") {
#ifdef MELKOR_HAS_SPZ
        std::cout << "Writing SPZ: ";
        melkor::text::writeDisplayString(std::cout, output_path);
        std::cout << '\n';

        melkor::SpzEncoder encoder;
        melkor::SpzEncodeConfig config;
        config.sh_degree = cloud.shDegree();

        auto result = encoder.encodeToFile(output_path, cloud, config);
        if (!result.success) {
            std::cerr << "Error writing SPZ: ";
            melkor::text::writeDisplayString(std::cerr, result.error_message);
            std::cerr << '\n';
            return 1;
        }

        std::cout << "Written " << result.bytes_written << " bytes\n";
#else
        std::cerr << "Error: SPZ support not compiled. Rebuild with SPZ library.\n";
        return 1;
#endif
    } else {
        std::cerr << "Error: Unsupported output format: ";
        melkor::text::writeDisplayString(std::cerr, output_ext);
        std::cerr << '\n';
        return 1;
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    std::cout << "Done in " << duration.count() << " ms\n";

    // Print bounding box
    float minX, minY, minZ, maxX, maxY, maxZ;
    cloud.computeBoundingBox(minX, minY, minZ, maxX, maxY, maxZ);
    std::cout << "Bounding box: ("
              << minX << ", " << minY << ", " << minZ << ") - ("
              << maxX << ", " << maxY << ", " << maxZ << ")\n";

    return 0;
} catch (const std::bad_alloc&) {
    std::cerr << "Error: operation exceeded available memory\n";
    return 1;
} catch (const std::length_error&) {
    std::cerr << "Error: operation exceeded a container size limit\n";
    return 1;
} catch (const std::exception& error) {
    std::cerr << "Error: operation failed safely: ";
    melkor::text::writeDisplayString(std::cerr, error.what());
    std::cerr << '\n';
    return 1;
} catch (...) {
    std::cerr << "Error: operation failed safely with an unknown error\n";
    return 1;
}
