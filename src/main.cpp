#include "melkor/compute_provider.hpp"
#include "melkor/densifier.hpp"
#include "melkor/glb_reader.hpp"
#include "melkor/ply_writer.hpp"
#include "melkor/spz_encoder.hpp"
#include "melkor/enhanced_converter.hpp"
#include "melkor/gaussian_fitter.hpp"
#include "melkor/feedforward_model.hpp"

// Metal-specific features (EnhancedConverter GPU path, GaussianFitter,
// DifferentiableRenderer) still require metal::MetalContext directly.
#ifdef MELKOR_HAS_METAL
#include "melkor/metal_compute.hpp"
#endif

#include <iostream>
#include <string>
#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;

void printUsage(const char* program) {
    std::cout << "Melkor - Advanced Gaussian Splatting Toolkit\n";
    std::cout << "\nUsage: " << program << " <input.glb> <output.ply|output.spz> [options]\n";
    std::cout << "\nSupported conversions:\n";
    std::cout << "  GLB -> PLY    Convert GLB mesh to Gaussian splatting PLY\n";
    std::cout << "  GLB -> SPZ    Convert GLB mesh to compressed SPZ format\n";
    std::cout << "  PLY -> SPZ    Convert PLY to compressed SPZ format\n";
    std::cout << "  SPZ -> PLY    Convert SPZ to PLY format\n";
    std::cout << "\nConversion Modes:\n";
    std::cout << "  --basic              Basic vertex-to-splat conversion (default, fast)\n";
    std::cout << "  --enhanced           Enhanced conversion with adaptive scale & surface alignment\n";
    std::cout << "  --fit                Render-based Gaussian fitting (higher quality, slower)\n";
    std::cout << "  --feedforward        Use pre-trained neural network (requires setup)\n";
    std::cout << "\nBasic/Enhanced Options:\n";
    std::cout << "  --scale <float>      Scale factor for splat size (default: 0.01)\n";
    std::cout << "  --opacity <float>    Default opacity 0-1 (default: 1.0)\n";
    std::cout << "  --pos-scale <float>  Position scale factor (default: 1.0)\n";
    std::cout << "  --no-coord-convert   Don't convert Y-up to Z-up\n";
    std::cout << "\nEnhanced Mode Options:\n";
    std::cout << "  --knn <int>          K neighbors for density estimation (default: 8)\n";
    std::cout << "  --no-surface-align   Disable surface-aligned Gaussians\n";
    std::cout << "\nFit Mode Options:\n";
    std::cout << "  --iterations <int>   Number of fitting iterations (default: 3000)\n";
    std::cout << "  --views <int>        Number of camera views (default: 8)\n";
    std::cout << "  --resolution <int>   Render resolution (default: 512)\n";
    std::cout << "\nFeedforward Mode Options:\n";
    std::cout << "  --model <type>       Model type: splatter-image, mvsplat (default: splatter-image)\n";
    std::cout << "  --download-model     Download model weights if not present\n";
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
    std::cout << "  --list-models        List available feedforward models\n";
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
    for (char& c : ext) c = std::tolower(c);
    return ext;
}

enum class ConversionMode {
    Basic,
    Enhanced,
    Fit,
    Feedforward
};

int main(int argc, char* argv[]) {
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
    bool download_model = false;

    // Mode selection
    ConversionMode mode = ConversionMode::Basic;

    // Enhanced mode options
    int knn_neighbors = 8;
    bool surface_align = true;

    // Fit mode options
    int fit_iterations = 3000;
    int fit_views = 8;
    int fit_resolution = 512;

    // Feedforward mode options
    std::string model_type = "splatter-image";

    // Scene completion options
    bool fill_holes = false;
    int fill_iterations = 3;
    float fill_strength = 1.0f;
    float max_hole_size = 8.0f;

    // Helper: parse a numeric argument safely, returning false on failure.
    auto parseFloat = [&](const char* flag, const char* val, float& out) -> bool {
        try {
            out = std::stof(val);
            return true;
        } catch (const std::exception&) {
            std::cerr << "Error: Invalid number for " << flag << ": \"" << val << "\"\n";
            return false;
        }
    };
    auto parseInt = [&](const char* flag, const char* val, int& out) -> bool {
        try {
            out = std::stoi(val);
            return true;
        } catch (const std::exception&) {
            std::cerr << "Error: Invalid integer for " << flag << ": \"" << val << "\"\n";
            return false;
        }
    };

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--info") {
            show_info = true;
        } else if (arg == "--list-models") {
            list_models = true;
        } else if (arg == "--download-model") {
            download_model = true;
        } else if (arg == "--basic") {
            mode = ConversionMode::Basic;
        } else if (arg == "--enhanced") {
            mode = ConversionMode::Enhanced;
        } else if (arg == "--fit") {
            mode = ConversionMode::Fit;
        } else if (arg == "--feedforward") {
            mode = ConversionMode::Feedforward;
        } else if (arg == "--scale" && i + 1 < argc) {
            if (!parseFloat("--scale", argv[++i], splat_scale)) return 1;
        } else if (arg == "--opacity" && i + 1 < argc) {
            if (!parseFloat("--opacity", argv[++i], opacity)) return 1;
        } else if (arg == "--pos-scale" && i + 1 < argc) {
            if (!parseFloat("--pos-scale", argv[++i], pos_scale)) return 1;
        } else if (arg == "--no-coord-convert") {
            convert_coords = false;
        } else if (arg == "--knn" && i + 1 < argc) {
            if (!parseInt("--knn", argv[++i], knn_neighbors)) return 1;
        } else if (arg == "--no-surface-align") {
            surface_align = false;
        } else if (arg == "--iterations" && i + 1 < argc) {
            if (!parseInt("--iterations", argv[++i], fit_iterations)) return 1;
        } else if (arg == "--views" && i + 1 < argc) {
            if (!parseInt("--views", argv[++i], fit_views)) return 1;
        } else if (arg == "--resolution" && i + 1 < argc) {
            if (!parseInt("--resolution", argv[++i], fit_resolution)) return 1;
        } else if (arg == "--model" && i + 1 < argc) {
            model_type = argv[++i];
        } else if (arg == "--fill-holes") {
            fill_holes = true;
        } else if (arg == "--fill-iterations" && i + 1 < argc) {
            if (!parseInt("--fill-iterations", argv[++i], fill_iterations)) return 1;
        } else if (arg == "--fill-strength" && i + 1 < argc) {
            if (!parseFloat("--fill-strength", argv[++i], fill_strength)) return 1;
        } else if (arg == "--max-hole-size" && i + 1 < argc) {
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
                std::cerr << "Warning: Ignoring extra positional argument: " << arg << "\n";
            }
        } else {
            std::cerr << "Warning: Unknown option: " << arg << "\n";
        }
    }

    // Validate numeric options: out-of-range values produce NaN scales
    // (0/0 in k-NN averaging) or degenerate fits rather than clean errors.
    if (knn_neighbors < 1) {
        std::cerr << "Error: --knn must be >= 1 (got " << knn_neighbors << ")\n";
        return 1;
    }
    if (fit_iterations < 1 || fit_views < 1 || fit_resolution < 1) {
        std::cerr << "Error: --iterations, --views and --resolution must be >= 1\n";
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
        std::cout << "Available feedforward models:\n";
        melkor::ModelWeightManager manager;
        for (const auto& model : manager.listModels()) {
            std::cout << "  " << model.name;
            if (model.downloaded) {
                std::cout << " [downloaded]";
            } else {
                std::cout << " [not downloaded, ~" << model.size_mb << "MB]";
            }
            std::cout << "\n    " << model.description << "\n";
        }
        return 0;
    }

    if (download_model) {
        std::cout << "Downloading model weights for: " << model_type << "\n";
        melkor::ModelWeightManager manager;
        auto result = manager.downloadWeights(model_type, [](float progress) {
            std::cout << "\rProgress: " << static_cast<int>(progress * 100) << "%" << std::flush;
        });
        std::cout << "\n";
        if (result.success) {
            std::cout << "Downloaded to: " << result.local_path << "\n";
            std::cout << "Size: " << (result.bytes_downloaded / 1024 / 1024) << " MB\n";
        } else {
            std::cerr << "Download failed: " << result.error_message << "\n";
            return 1;
        }
        return 0;
    }

    if (input_path.empty() || output_path.empty()) {
        std::cerr << "Error: Input and output paths required.\n";
        printUsage(argv[0]);
        return 1;
    }

    // Check input exists
    if (!fs::exists(input_path)) {
        std::cerr << "Error: Input file not found: " << input_path << "\n";
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

    // Metal-specific features (EnhancedConverter GPU path, GaussianFitter)
    // need the raw Metal context handle.  Nullptr on non-Metal backends —
    // EnhancedConverter falls back to CPU when given nullptr.
    melkor::metal::MetalContext* metal_ctx = nullptr;
    if (provider && provider->backend() == melkor::ComputeBackend::Metal) {
        metal_ctx = static_cast<melkor::metal::MetalContext*>(provider->rawContext());
    }

    melkor::GaussianCloud cloud;

    // Load input
    if (input_ext == ".glb" || input_ext == ".gltf") {
        std::cout << "Loading GLB: " << input_path << "\n";

        // Handle different conversion modes for GLB input
        if (mode == ConversionMode::Enhanced) {
            std::cout << "Using enhanced conversion mode\n";

            melkor::EnhancedConverter converter(metal_ctx);
            melkor::EnhancedConversionConfig config;
            config.knn_neighbors = knn_neighbors;
            config.scale_factor = splat_scale * 50.0f;  // Adjust scale factor
            config.use_surface_alignment = surface_align;
            config.default_opacity = opacity;
            config.position_scale = pos_scale;
            config.convert_coordinate_system = convert_coords;

            auto result = converter.convertFromFile(input_path, config);
            if (!result.success) {
                std::cerr << "Error: " << result.error_message << "\n";
                return 1;
            }

            cloud = std::move(result.cloud);
            std::cout << "Enhanced conversion: " << result.original_vertices << " vertices -> "
                      << result.output_splats << " splats\n";
            std::cout << "Average scale: " << result.avg_scale << "\n";

        } else if (mode == ConversionMode::Fit) {
            std::cout << "Using render-based Gaussian fitting mode\n";
            std::cout << "This may take several minutes...\n";

#ifdef MELKOR_HAS_METAL
            if (!metal_ctx) {
                std::cerr << "Error: Fit mode requires Metal GPU acceleration\n";
                std::cerr << "Use OpenSplat with --backend cuda/metal for GPU-accelerated training\n";
                return 1;
            }

            melkor::GaussianFitter fitter(*metal_ctx);
            melkor::GaussianFitConfig config;
            config.num_iterations = fit_iterations;
            config.num_views = fit_views;
            config.render_width = fit_resolution;
            config.render_height = fit_resolution;
            config.progress_callback = [](int iter, float loss, size_t num_gaussians) {
                if (iter % 500 == 0) {
                    std::cout << "Iteration " << iter << ": loss=" << loss
                              << ", gaussians=" << num_gaussians << "\n";
                }
            };

            auto result = fitter.fitFromGlb(input_path, config);
            if (!result.success) {
                std::cerr << "Error: " << result.error_message << "\n";
                return 1;
            }

            cloud = std::move(result.cloud);
            std::cout << "Fitting complete: " << cloud.size() << " Gaussians\n";
            std::cout << "Final loss: " << result.final_loss << "\n";
            std::cout << "Time: " << result.fitting_time_seconds << "s\n";
#else
            std::cerr << "Error: Fit mode requires Metal GPU acceleration (not compiled)\n";
            std::cerr << "Build with Metal support or use OpenSplat for GPU training\n";
            return 1;
#endif

        } else if (mode == ConversionMode::Feedforward) {
            std::cout << "Using feedforward neural network mode\n";

            melkor::FeedforwardModel model;
            melkor::FeedforwardConfig config;
            config.model_type = model_type;
            config.log_callback = [](const std::string& msg) {
                std::cout << msg << "\n";
            };

            if (!model.initialize(config)) {
                std::cerr << "Error: Failed to initialize feedforward model\n";
                std::cerr << "Run with --download-model to download weights first\n";
                return 1;
            }

            auto result = model.predictFromGlb(input_path);
            if (!result.success) {
                std::cerr << "Error: " << result.error_message << "\n";
                return 1;
            }

            cloud = std::move(result.cloud);
            std::cout << "Inference complete: " << cloud.size() << " Gaussians\n";
            std::cout << "Inference time: " << result.inference_time_ms << "ms\n";

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
                std::cerr << "Error loading GLB: " << result.error_message << "\n";
                return 1;
            }

            cloud = std::move(result.cloud);
            std::cout << "Loaded " << cloud.size() << " vertices from "
                      << result.total_meshes << " meshes\n";
        }

    } else if (input_ext == ".ply") {
        std::cout << "Loading PLY: " << input_path << "\n";

        melkor::PlyReader reader;
        auto result = reader.readFromFile(input_path);
        if (!result.success) {
            std::cerr << "Error loading PLY: " << result.error_message << "\n";
            return 1;
        }

        cloud = std::move(result.cloud);
        std::cout << "Loaded " << cloud.size() << " splats\n";

    } else if (input_ext == ".spz") {
#ifdef MELKOR_HAS_SPZ
        std::cout << "Loading SPZ: " << input_path << "\n";

        melkor::SpzDecoder decoder;
        auto result = decoder.decodeFromFile(input_path);
        if (!result.success) {
            std::cerr << "Error loading SPZ: " << result.error_message << "\n";
            return 1;
        }

        cloud = std::move(result.cloud);
        std::cout << "Loaded " << cloud.size() << " splats\n";
#else
        std::cerr << "Error: SPZ support not compiled. Rebuild with SPZ library.\n";
        return 1;
#endif
    } else {
        std::cerr << "Error: Unsupported input format: " << input_ext << "\n";
        return 1;
    }

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

    // Process with compute backend if available (normalize quaternions)
    if (provider && !cloud.empty()) {
        std::cout << "Processing with " << provider->backendName() << "...\n";
        provider->normalizeQuaternions(cloud);
    }

    // Write output
    if (output_ext == ".ply") {
        std::cout << "Writing PLY: " << output_path << "\n";

        melkor::PlyWriter writer;
        melkor::PlyWriteConfig config;
        config.format = use_binary ? melkor::PlyFormat::Binary : melkor::PlyFormat::Ascii;

        auto result = writer.writeToFile(output_path, cloud, config);
        if (!result.success) {
            std::cerr << "Error writing PLY: " << result.error_message << "\n";
            return 1;
        }

        std::cout << "Written " << result.bytes_written << " bytes\n";

    } else if (output_ext == ".spz") {
#ifdef MELKOR_HAS_SPZ
        std::cout << "Writing SPZ: " << output_path << "\n";

        melkor::SpzEncoder encoder;
        melkor::SpzEncodeConfig config;
        config.sh_degree = cloud.shDegree();

        auto result = encoder.encodeToFile(output_path, cloud, config);
        if (!result.success) {
            std::cerr << "Error writing SPZ: " << result.error_message << "\n";
            return 1;
        }

        std::cout << "Written " << result.bytes_written << " bytes\n";
#else
        std::cerr << "Error: SPZ support not compiled. Rebuild with SPZ library.\n";
        return 1;
#endif
    } else {
        std::cerr << "Error: Unsupported output format: " << output_ext << "\n";
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
}
