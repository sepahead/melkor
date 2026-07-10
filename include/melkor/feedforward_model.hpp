#pragma once

#include "melkor/gaussian_data.hpp"
#include <memory>
#include <vector>
#include <string>
#include <functional>

namespace melkor {

// Configuration for feedforward model
struct FeedforwardConfig {
    // Model settings
    // Supported model types:
    //   - "splatter-image": Single-view 3DGS (Szymanowicz et al. CVPR 2024)
    //   - "mvsplat": Multi-view 3DGS (Chen et al. ECCV 2024)
    //   - "da3": Depth-Anything-3 (ByteDance, multi-view depth-ray)
    //   - "da3-small", "da3-base", "da3-large": DA3 model variants
    //   - "custom": Custom model
    std::string model_type = "splatter-image";
    std::string weights_path;                    // Path to model weights
    std::string python_env;                      // Python environment path (optional)
    
    // Input settings
    int input_width = 256;                       // Input image resolution
    int input_height = 256;
    int num_input_views = 1;                     // Number of input views (1 for single-view, 2+ for multi-view)
    
    // Output settings
    int output_resolution = 128;                 // Output Gaussian map resolution
    float scale_factor = 1.0f;                   // Scale factor for output positions
    float gaussian_scale = 0.01f;                // Base scale for Gaussians (DA3)
    int subsample = 1;                           // Pixel subsampling factor (DA3)
    float min_depth = 0.1f;                      // Minimum valid depth (DA3)
    float max_depth = 100.0f;                    // Maximum valid depth (DA3)
    
    // Device settings
    bool use_mlx = true;                         // Use MLX (Apple Silicon optimized)
    bool use_mps = true;                         // Use Metal Performance Shaders
    bool use_cuda = true;                        // Use CUDA (Linux)
    bool use_fp16 = true;                        // Use FP16 for inference
    int batch_size = 1;
    
    // Multi-GPU settings (DA3)
    std::vector<int> gpu_ids;                    // GPU IDs to use (empty = all)
    bool multi_gpu = false;                      // Enable multi-GPU inference
    
    // Progress callback
    std::function<void(const std::string&)> log_callback;
};

// Result of feedforward inference
struct FeedforwardResult {
    bool success = false;
    std::string error_message;
    GaussianCloud cloud;
    
    // Statistics
    float inference_time_ms = 0.0f;
    size_t num_gaussians = 0;
    int model_memory_mb = 0;
};

// Retired native facade retained for source compatibility. It fails closed;
// supported neural reconstruction is provided by the da3-infer Python CLI.
class FeedforwardModel {
public:
    FeedforwardModel();
    ~FeedforwardModel();
    
    // Check if model is available
    static bool isAvailable();
    
    // Check if specific model type is available
    static bool isModelAvailable(const std::string& model_type);
    
    // Get available model types
    static std::vector<std::string> getAvailableModels();
    
    // Initialize with configuration
    bool initialize(const FeedforwardConfig& config);
    
    // Check if initialized
    bool isInitialized() const;
    
    // Run inference on a single image
    FeedforwardResult predict(
        const std::vector<uint8_t>& image_rgba,
        int width, int height);
    
    // Run inference on multiple views
    FeedforwardResult predictMultiView(
        const std::vector<std::vector<uint8_t>>& images_rgba,
        const std::vector<int>& widths,
        const std::vector<int>& heights);
    
    // Run inference on rendered views of a GLB mesh
    FeedforwardResult predictFromGlb(
        const std::string& glb_path,
        int num_views = 4);
    
    // Get model info
    std::string getModelInfo() const;
    
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Python bridge for running feedforward models
class PythonBridge {
public:
    PythonBridge();
    ~PythonBridge();
    
    // Check if Python is available
    static bool isPythonAvailable();
    
    // Check if required packages are installed
    static bool checkDependencies(const std::string& model_type);
    
    // Initialize Python interpreter
    bool initialize(const std::string& python_path = "");
    
    // Run a Python script and capture output
    struct ScriptResult {
        bool success = false;
        int exit_code = 0;
        std::string stdout_output;
        std::string stderr_output;
    };
    
    ScriptResult runScript(const std::string& script_path,
                          const std::vector<std::string>& args = {});
    
    // Run inline Python code
    ScriptResult runCode(const std::string& code);
    
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Model weight manager
class ModelWeightManager {
public:
    ModelWeightManager();
    ~ModelWeightManager();
    
    // Get default weights directory
    static std::string getWeightsDirectory();
    
    // Check if weights are downloaded
    bool hasWeights(const std::string& model_type) const;
    
    // Get weights path
    std::string getWeightsPath(const std::string& model_type) const;
    
    // Download weights (requires network)
    struct DownloadResult {
        bool success = false;
        std::string error_message;
        std::string local_path;
        size_t bytes_downloaded = 0;
    };
    
    DownloadResult downloadWeights(
        const std::string& model_type,
        std::function<void(float)> progress_callback = nullptr);
    
    // List available models with their status
    struct ModelInfo {
        std::string name;
        std::string description;
        std::string url;
        size_t size_mb;
        bool downloaded;
        bool supports_multi_gpu;     // Whether model supports multi-GPU inference
        bool supports_multi_view;    // Whether model supports multi-view input
    };
    
    std::vector<ModelInfo> listModels() const;
    
    // Check if DA3 is available
    static bool isDA3Available();
    
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace melkor
