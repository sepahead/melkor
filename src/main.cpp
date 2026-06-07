#include "melkor/glb_reader.hpp"
#include "melkor/ply_writer.hpp"
#ifdef MELKOR_HAS_METAL
#include "melkor/metal_compute.hpp"
#elif defined(MELKOR_HAS_CUDA)
#include "melkor/cuda_compute.hpp"
#else
#include "melkor/metal_compute.hpp"  // Uses stub implementation
#endif
#include "melkor/spz_encoder.hpp"
#include "melkor/enhanced_converter.hpp"
#include "melkor/gaussian_fitter.hpp"
#include "melkor/feedforward_model.hpp"

#include <iostream>
#include <string>
#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;

void printUsage(const char* program) {
#ifdef MELKOR_HAS_METAL
    std::cout << "Melkor - Advanced Gaussian Splatting Toolkit (Metal-accelerated)\n";
#elif defined(MELKOR_HAS_CUDA)
    std::cout << "Melkor - Advanced Gaussian Splatting Toolkit (CUDA-accelerated)\n";
#else
    std::cout << "Melkor - Advanced Gaussian Splatting Toolkit (CPU)\n";
#endif
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
    std::cout << "\nGeneral Options:\n";
    std::cout << "  --ascii              Output ASCII PLY instead of binary\n";
#ifdef MELKOR_HAS_METAL
    std::cout << "  --no-metal           Disable Metal acceleration\n";
#endif
    std::cout << "  --info               Show GPU info and exit\n";
    std::cout << "  --list-models        List available feedforward models\n";
    std::cout << "  -h, --help           Show this help\n";
}

void printDeviceInfo() {
#ifdef MELKOR_HAS_METAL
    if (!melkor::metal::MetalContext::isAvailable()) {
        std::cout << "Metal is not available on this system.\n";
        return;
    }
    
    melkor::metal::MetalContext ctx;
    if (!ctx.initialize()) {
        std::cout << "Failed to initialize Metal.\n";
        return;
    }
    
    auto info = ctx.getDeviceInfo();
    std::cout << "GPU Information:\n";
    std::cout << "  Backend: Metal\n";
    std::cout << "  Name: " << info.name << "\n";
    std::cout << "  Max Working Set: " << (info.recommended_max_working_set_size / (1024*1024)) << " MB\n";
    std::cout << "  Max Threads/Threadgroup: " << info.max_threads_per_threadgroup << "\n";
    std::cout << "  Apple Silicon (Family 7+): " << (info.supports_family_apple7 ? "Yes" : "No") << "\n";
#elif defined(MELKOR_HAS_CUDA)
    melkor::cuda::CudaContext ctx;
    if (!ctx.initialize()) {
        std::cout << "Failed to initialize CUDA.\n";
        return;
    }
    
    auto info = ctx.getDeviceInfo();
    std::cout << "GPU Information:\n";
    std::cout << "  Backend: CUDA\n";
    std::cout << "  Name: " << info.name << "\n";
    std::cout << "  Total Memory: " << (info.total_memory / (1024*1024)) << " MB\n";
    std::cout << "  Compute Capability: " << info.compute_capability_major << "." << info.compute_capability_minor << "\n";
    std::cout << "  Max Threads/Block: " << info.max_threads_per_block << "\n";
#else
    std::cout << "GPU Information:\n";
    std::cout << "  Backend: CPU-only (no GPU acceleration)\n";
    std::cout << "  Note: Use OpenSplat with CUDA on Linux for GPU training\n";
#endif
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
    bool use_metal = true;
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
            splat_scale = std::stof(argv[++i]);
        } else if (arg == "--opacity" && i + 1 < argc) {
            opacity = std::stof(argv[++i]);
        } else if (arg == "--pos-scale" && i + 1 < argc) {
            pos_scale = std::stof(argv[++i]);
        } else if (arg == "--no-coord-convert") {
            convert_coords = false;
        } else if (arg == "--knn" && i + 1 < argc) {
            knn_neighbors = std::stoi(argv[++i]);
        } else if (arg == "--no-surface-align") {
            surface_align = false;
        } else if (arg == "--iterations" && i + 1 < argc) {
            fit_iterations = std::stoi(argv[++i]);
        } else if (arg == "--views" && i + 1 < argc) {
            fit_views = std::stoi(argv[++i]);
        } else if (arg == "--resolution" && i + 1 < argc) {
            fit_resolution = std::stoi(argv[++i]);
        } else if (arg == "--model" && i + 1 < argc) {
            model_type = argv[++i];
        } else if (arg == "--ascii") {
            use_binary = false;
        } else if (arg == "--no-metal") {
            use_metal = false;
        } else if (arg[0] != '-') {
            if (input_path.empty()) {
                input_path = arg;
            } else if (output_path.empty()) {
                output_path = arg;
            }
        }
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
    
    // Initialize Metal if requested
    std::unique_ptr<melkor::metal::MetalContext> metal_ctx;
    std::unique_ptr<melkor::metal::GaussianProcessor> processor;
    
    if (use_metal && melkor::metal::MetalContext::isAvailable()) {
        metal_ctx = std::make_unique<melkor::metal::MetalContext>();
        if (metal_ctx->initialize()) {
            processor = std::make_unique<melkor::metal::GaussianProcessor>(*metal_ctx);
            auto info = metal_ctx->getDeviceInfo();
            std::cout << "Using Metal acceleration: " << info.name << "\n";
        } else {
            std::cout << "Metal initialization failed, using CPU\n";
            metal_ctx.reset();
        }
    } else if (use_metal) {
        std::cout << "Metal not available, using CPU\n";
    }
    
    melkor::GaussianCloud cloud;
    
    // Load input
    if (input_ext == ".glb" || input_ext == ".gltf") {
        std::cout << "Loading GLB: " << input_path << "\n";
        
        // Handle different conversion modes for GLB input
        if (mode == ConversionMode::Enhanced) {
            std::cout << "Using enhanced conversion mode\n";
            
            melkor::EnhancedConverter converter(metal_ctx.get());
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
            
            if (!metal_ctx) {
                std::cerr << "Error: Fit mode requires GPU acceleration (Metal on macOS)\n";
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
    
    // Process with Metal if available (for additional optimizations)
    if (processor && !cloud.empty()) {
        std::cout << "Processing with Metal...\n";
        
        melkor::metal::GaussianProcessor::ProcessConfig proc_config;
        proc_config.normalize_quaternions = true;
        proc_config.convert_colors_to_sh = false;  // Already converted in loader
        proc_config.convert_opacity_to_logit = false;  // Already converted
        proc_config.position_scale = 1.0f;  // Already scaled
        proc_config.transform_y_up_to_z_up = false;  // Already transformed
        
        processor->normalizeQuaternions(cloud);
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
