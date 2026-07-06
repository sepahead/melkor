#include "melkor/feedforward_model.hpp"
#include "melkor/glb_reader.hpp"
#include "melkor/ply_writer.hpp"
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <array>
#include <filesystem>
#include <chrono>
#include <cmath>
#include <limits>
#include <cstdint>
#include <cstdio>
#ifdef __unix__
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;

namespace melkor {

// ============================================================================
// PythonBridge Implementation
// ============================================================================

namespace {

// Shell-safe a single argument: wrap in single quotes, escape any embedded
// single quote as '\''. This prevents command injection via popen when the
// argument contains metacharacters ($, `, ", ;, |, etc.).
std::string shellEscape(const std::string& arg) {
    std::string out;
    out.reserve(arg.size() + 2);
    out += '\'';
    for (char c : arg) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += '\'';
    return out;
}

}  // namespace

class PythonBridge::Impl {
public:
    std::string python_path_;
    bool initialized_ = false;
    
    std::string findPython() {
        // Try common Python locations
        std::vector<std::string> candidates = {
            "python3",
            "python",
            "/usr/bin/python3",
            "/usr/local/bin/python3",
            "/opt/homebrew/bin/python3"
        };
        
        for (const auto& path : candidates) {
            std::string cmd = shellEscape(path) + " --version 2>&1";
            FILE* pipe = popen(cmd.c_str(), "r");
            if (pipe) {
                char buffer[128];
                std::string result;
                while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                    result += buffer;
                }
                int status = pclose(pipe);
                if (status == 0 && result.find("Python 3") != std::string::npos) {
                    return path;
                }
            }
        }
        
        return "";
    }
    
    PythonBridge::ScriptResult execute(const std::string& command) {
        PythonBridge::ScriptResult result;
        
        std::array<char, 4096> buffer;
        std::string output;
        
        // Execute command with stderr redirected to stdout
        FILE* pipe = popen((command + " 2>&1").c_str(), "r");
        if (!pipe) {
            result.stderr_output = "Failed to execute command";
            return result;
        }
        
        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
            output += buffer.data();
        }
        
        result.exit_code = pclose(pipe);
        result.success = (result.exit_code == 0);
        
        // Put output in appropriate field based on success
        if (result.success) {
            result.stdout_output = output;
        } else {
            result.stderr_output = output;
            result.stdout_output = output; // Also include in stdout for debugging
        }
        
        return result;
    }
};
PythonBridge::PythonBridge() : impl_(std::make_unique<Impl>()) {}
PythonBridge::~PythonBridge() = default;

bool PythonBridge::isPythonAvailable() {
    Impl impl;
    return !impl.findPython().empty();
}

bool PythonBridge::checkDependencies(const std::string& model_type) {
    PythonBridge bridge;
    if (!bridge.initialize()) return false;
    
    std::string check_code;
    if (model_type == "splatter-image") {
        check_code = "import torch; import torchvision; print('ok')";
    } else if (model_type == "mvsplat") {
        check_code = "import torch; import einops; print('ok')";
    } else {
        check_code = "import numpy; print('ok')";
    }
    
    auto result = bridge.runCode(check_code);
    return result.success && result.stdout_output.find("ok") != std::string::npos;
}

bool PythonBridge::initialize(const std::string& python_path) {
    if (python_path.empty()) {
        impl_->python_path_ = impl_->findPython();
    } else {
        impl_->python_path_ = python_path;
    }
    
    impl_->initialized_ = !impl_->python_path_.empty();
    return impl_->initialized_;
}

PythonBridge::ScriptResult PythonBridge::runScript(
    const std::string& script_path,
    const std::vector<std::string>& args) {
    
    if (!impl_->initialized_) {
        ScriptResult result;
        result.stderr_output = "Python bridge not initialized";
        return result;
    }
    
    std::string cmd = shellEscape(impl_->python_path_) + " " + shellEscape(script_path);
    for (const auto& arg : args) {
        cmd += " " + shellEscape(arg);
    }
    
    return impl_->execute(cmd);
}

PythonBridge::ScriptResult PythonBridge::runCode(const std::string& code) {
    if (!impl_->initialized_) {
        ScriptResult result;
        result.stderr_output = "Python bridge not initialized";
        return result;
    }
    
    // Write inline code to a temp file and execute it, rather than passing
    // via `python -c "..."` which is vulnerable to shell-quote injection when
    // the code string contains double quotes or metacharacters.
    std::string temp_script = "/tmp/melkor_inline_" +
        std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count()) +
        ".py";
    {
        std::ofstream f(temp_script);
        if (!f.is_open()) {
            ScriptResult result;
            result.stderr_output = "Failed to write temp script: " + temp_script;
            return result;
        }
        f << code;
    }
    
    std::string cmd = shellEscape(impl_->python_path_) + " " + shellEscape(temp_script);
    auto result = impl_->execute(cmd);
    fs::remove(temp_script);
    return result;
}

// ============================================================================
// ModelWeightManager Implementation
// ============================================================================

class ModelWeightManager::Impl {
public:
    std::string weights_dir_;
    
    Impl() {
        // Default to ~/.melkor/models/
        const char* home = std::getenv("HOME");
        if (home) {
            weights_dir_ = std::string(home) + "/.melkor/models";
        } else {
            weights_dir_ = "/tmp/melkor/models";
        }
    }
    
    std::vector<ModelInfo> getModelRegistry() const {
        return {
            {
                "splatter-image",
                "Single-view 3D Gaussian Splatting (Szymanowicz et al. CVPR 2024)",
                "https://huggingface.co/szymanowiczs/splatter-image/resolve/main/model_latest.pth",
                500,
                false,
                false,  // supports_multi_gpu
                false   // supports_multi_view
            },
            {
                "mvsplat",
                "Multi-view 3D Gaussian Splatting (Chen et al. ECCV 2024)",
                "https://huggingface.co/donydchen/mvsplat/resolve/main/re10k.ckpt",
                800,
                false,
                false,  // supports_multi_gpu
                true    // supports_multi_view
            },
            {
                "da3-small",
                "Depth-Anything-3 Small - Fast feedforward depth-ray 3DGS (ByteDance)",
                "https://huggingface.co/depth-anything/DA3-SMALL",
                1000,
                false,
                true,   // supports_multi_gpu
                true    // supports_multi_view
            },
            {
                "da3-base",
                "Depth-Anything-3 Base - Balanced feedforward depth-ray 3DGS (ByteDance)",
                "https://huggingface.co/depth-anything/DA3-BASE",
                2000,
                false,
                true,   // supports_multi_gpu
                true    // supports_multi_view
            },
            {
                "da3-large",
                "Depth-Anything-3 Large - High quality feedforward depth-ray 3DGS (ByteDance)",
                "https://huggingface.co/depth-anything/DA3-LARGE",
                4000,
                false,
                true,   // supports_multi_gpu
                true    // supports_multi_view
            },
            {
                "da3-giant",
                "Depth-Anything-3 Giant - Best quality feedforward depth-ray 3DGS (~1.15B params, DINOv2-G backbone, ByteDance)",
                "https://huggingface.co/depth-anything/DA3-GIANT",
                8000,
                false,
                true,   // supports_multi_gpu
                true    // supports_multi_view
            },
            {
                "da3mono-large",
                "Depth-Anything-3 Monocular Large - Single-view relative depth (0-1 normalized, no 3D reconstruction)",
                "https://huggingface.co/depth-anything/DA3MONO-LARGE",
                4000,
                false,
                true,   // supports_multi_gpu (parallel single-image processing)
                false   // supports_multi_view (monocular only, outputs relative depth)
            },
            {
                "da3metric-large",
                "Depth-Anything-3 Metric Large - Single-view absolute depth in meters (robotics/navigation/measurement)",
                "https://huggingface.co/depth-anything/DA3METRIC-LARGE",
                4000,
                false,
                true,   // supports_multi_gpu (parallel single-image processing)
                false   // supports_multi_view (monocular only, outputs metric depth)
            },
            {
                "da3nested-giant-large",
                "Depth-Anything-3 Nested Giant+Large - Full 3D reconstruction with real-world metric scale (production quality)",
                "https://huggingface.co/depth-anything/DA3NESTED-GIANT-LARGE",
                12000,
                false,
                true,   // supports_multi_gpu
                true    // supports_multi_view (combines any-view + metric alignment)
            }
        };
    }
};

ModelWeightManager::ModelWeightManager() : impl_(std::make_unique<Impl>()) {}
ModelWeightManager::~ModelWeightManager() = default;

std::string ModelWeightManager::getWeightsDirectory() {
    Impl impl;
    return impl.weights_dir_;
}

bool ModelWeightManager::hasWeights(const std::string& model_type) const {
    std::string path = getWeightsPath(model_type);
    return fs::exists(path);
}

std::string ModelWeightManager::getWeightsPath(const std::string& model_type) const {
    if (model_type == "splatter-image") {
        return impl_->weights_dir_ + "/splatter-image/model_latest.pth";
    } else if (model_type == "mvsplat") {
        return impl_->weights_dir_ + "/mvsplat/re10k.ckpt";
    } else if (model_type == "da3" || model_type == "da3-base") {
        return impl_->weights_dir_ + "/da3/DA3-BASE";
    } else if (model_type == "da3-small") {
        return impl_->weights_dir_ + "/da3/DA3-SMALL";
    } else if (model_type == "da3-large") {
        return impl_->weights_dir_ + "/da3/DA3-LARGE";
    } else if (model_type == "da3-giant") {
        return impl_->weights_dir_ + "/da3/DA3-GIANT";
    } else if (model_type == "da3mono-large") {
        return impl_->weights_dir_ + "/da3/DA3MONO-LARGE";
    } else if (model_type == "da3metric-large") {
        return impl_->weights_dir_ + "/da3/DA3METRIC-LARGE";
    } else if (model_type == "da3nested-giant-large") {
        return impl_->weights_dir_ + "/da3/DA3NESTED-GIANT-LARGE";
    }
    return "";
}

bool ModelWeightManager::isDA3Available() {
    // Check if DA3 tools are installed
    std::string da3_dir;
    const char* home = std::getenv("HOME");
    if (home) {
        // Check for DA3 venv
        std::string venv_path = std::string(home) + "/.melkor/models/da3";
        if (fs::exists(venv_path)) {
            return true;
        }
    }
    
    // Also check project tools directory
    if (fs::exists("tools/da3/venv")) {
        return true;
    }
    
    return false;
}

ModelWeightManager::DownloadResult ModelWeightManager::downloadWeights(
    const std::string& model_type,
    std::function<void(float)> progress_callback) {
    
    DownloadResult result;
    
    auto models = impl_->getModelRegistry();
    std::string url;
    size_t size_mb = 0;

    for (const auto& model : models) {
        if (model.name == model_type) {
            url = model.url;
            size_mb = model.size_mb;
            break;
        }
    }

    if (url.empty()) {
        result.error_message = "Unknown model type: " + model_type;
        return result;
    }

    // Create directory
    std::string dir = impl_->weights_dir_ + "/" + model_type;
    fs::create_directories(dir);

    // Download using curl. Shell-escape both the output path and URL to
    // prevent command injection via metacharacters in either string.
    // -f/--fail makes curl exit non-zero on HTTP 4xx/5xx instead of saving the
    // server's error page as if it were the weights file.
    std::string output_path = getWeightsPath(model_type);
    std::string cmd = "curl -fL -o " + shellEscape(output_path) + " " + shellEscape(url);

    if (progress_callback) {
        progress_callback(0.0f);
    }

    int ret = std::system(cmd.c_str());
    bool cmd_ok = (ret != -1) && (ret == 0
#ifdef __unix__
                   || (WIFEXITED(ret) && WEXITSTATUS(ret) == 0)
#endif
                  );

    if (!cmd_ok || !fs::exists(output_path)) {
        // Drop any partial download so hasWeights()/isModelAvailable() don't
        // report a broken file as an installed model forever.
        if (fs::exists(output_path)) {
            fs::remove(output_path);
        }
        result.error_message = "Download failed: curl reported an error for " + url;
        return result;
    }

    // Validate the payload before declaring success. A repo landing page or an
    // HTML error page (e.g. the da3-* Hugging Face URLs, which point at the
    // repo, not a file) downloads "successfully" as HTML and then makes
    // inference fail opaquely later.
    std::string reject_reason;
    {
        std::ifstream f(output_path, std::ios::binary);
        std::array<char, 256> head{};
        f.read(head.data(), head.size());
        const size_t head_len = static_cast<size_t>(f.gcount());
        size_t i = 0;
        while (i < head_len && (head[i] == ' ' || head[i] == '\t' ||
                                head[i] == '\r' || head[i] == '\n')) {
            ++i;
        }
        if (head_len == 0) {
            reject_reason = "downloaded file is empty";
        } else if (i < head_len && head[i] == '<') {
            // Covers '<', '<!DOCTYPE', '<html', and any other markup.
            reject_reason = "downloaded file is an HTML page, not a weights "
                            "binary (URL likely points to a repo page)";
        }
    }

    // Sanity-check the size against the advertised size: an HTML/error page is
    // a few KB, real weights are hundreds of MB. Allow generous slack (1%).
    const std::uintmax_t actual_bytes = fs::file_size(output_path);
    if (reject_reason.empty() && size_mb > 0) {
        const std::uintmax_t min_bytes =
            static_cast<std::uintmax_t>(size_mb) * 1024ull * 1024ull / 100ull;
        if (actual_bytes < min_bytes) {
            reject_reason = "downloaded file is implausibly small (" +
                std::to_string(actual_bytes) + " bytes) for a ~" +
                std::to_string(size_mb) + " MB model";
        }
    }

    if (!reject_reason.empty()) {
        fs::remove(output_path);
        result.error_message =
            "Download validation failed for " + model_type + ": " + reject_reason;
        return result;
    }

    result.success = true;
    result.local_path = output_path;
    result.bytes_downloaded = actual_bytes;

    if (progress_callback) {
        progress_callback(1.0f);
    }

    return result;
}

std::vector<ModelWeightManager::ModelInfo> ModelWeightManager::listModels() const {
    auto models = impl_->getModelRegistry();
    
    for (auto& model : models) {
        model.downloaded = hasWeights(model.name);
    }
    
    return models;
}

// ============================================================================
// FeedforwardModel Implementation
// ============================================================================

class FeedforwardModel::Impl {
public:
    FeedforwardConfig config_;
    bool initialized_ = false;
    PythonBridge python_bridge_;
    ModelWeightManager weight_manager_;
    
    bool checkSetup() {
        // Check Python
        if (!PythonBridge::isPythonAvailable()) {
            if (config_.log_callback) {
                config_.log_callback("Python 3 not found");
            }
            return false;
        }
        
        // Check weights
        if (!weight_manager_.hasWeights(config_.model_type)) {
            if (config_.log_callback) {
                config_.log_callback("Model weights not found. Run setup script first.");
            }
            return false;
        }
        
        return true;
    }
    
    bool runDA3Inference(const std::string& image_path,
                          const std::string& output_ply) {
        std::string da3_script = "tools/da3/inference.py";
        if (!fs::exists(da3_script)) {
            return false;
        }
        // runScript quotes each vector element as a separate argv token, so the
        // flag and its value MUST be two separate entries. Packing them as a
        // single "--model foo" string would pass one mangled argv token and the
        // argparse parser would reject it.
        auto result = python_bridge_.runScript(da3_script, {
            "--input", image_path,
            "--output", output_ply,
            "--model", config_.model_type
        });
        return result.success && fs::exists(output_ply);
    }
    

};

FeedforwardModel::FeedforwardModel() : impl_(std::make_unique<Impl>()) {}
FeedforwardModel::~FeedforwardModel() = default;

bool FeedforwardModel::isAvailable() {
    return PythonBridge::isPythonAvailable();
}

bool FeedforwardModel::isModelAvailable(const std::string& model_type) {
    ModelWeightManager manager;
    return manager.hasWeights(model_type);
}

std::vector<std::string> FeedforwardModel::getAvailableModels() {
    std::vector<std::string> available;
    ModelWeightManager manager;
    
    for (const auto& model : manager.listModels()) {
        if (model.downloaded) {
            available.push_back(model.name);
        }
    }
    
    return available;
}

bool FeedforwardModel::initialize(const FeedforwardConfig& config) {
    impl_->config_ = config;
    
    if (!impl_->python_bridge_.initialize(config.python_env)) {
        return false;
    }
    
    impl_->initialized_ = impl_->checkSetup();
    return impl_->initialized_;
}

bool FeedforwardModel::isInitialized() const {
    return impl_->initialized_;
}

FeedforwardResult FeedforwardModel::predict(
    const std::vector<uint8_t>& image_rgba,
    int width, int height) {
    
    FeedforwardResult result;
    
    if (!impl_->initialized_) {
        result.error_message = "Model not initialized";
        return result;
    }
    
    auto start_time = std::chrono::high_resolution_clock::now();

    // Run DA3 inference via tools/da3/inference.py. The bridge consumes a file
    // path, so persist the incoming RGBA buffer as a PPM (binary, no external
    // image deps required from the C++ side). DA3's loader reads via PIL, which
    // understands PPM.
    PlyReader ply_reader;
    std::string temp_dir = "/tmp/melkor_feedforward";
    std::string output_ply = temp_dir + "/output.ply";
    std::string image_path = temp_dir + "/input.ppm";

    fs::create_directories(temp_dir);
    {
        std::ofstream img_file(image_path, std::ios::binary);
        if (!img_file.is_open()) {
            result.error_message = "Failed to open temp image for write: " + image_path;
            return result;
        }
        img_file << "P6\n" << width << " " << height << "\n255\n";
        for (int i = 0; i < width * height; ++i) {
            img_file.put(static_cast<char>(image_rgba[i * 4 + 0]));
            img_file.put(static_cast<char>(image_rgba[i * 4 + 1]));
            img_file.put(static_cast<char>(image_rgba[i * 4 + 2]));
        }
    }
    
    if (!impl_->runDA3Inference(image_path, output_ply)) {
        result.error_message = "DA3 inference failed. Run setup_da3.sh first.";
        fs::remove_all(temp_dir);
        return result;
    }
    
    // Parse PLY output
    auto read_result = ply_reader.readFromFile(output_ply);
    if (!read_result.success) {
        result.error_message = "Failed to parse DA3 output: " + read_result.error_message;
        fs::remove_all(temp_dir);
        return result;
    }
    result.cloud = std::move(read_result.cloud);
    result.num_gaussians = result.cloud.size();
    
    // Clean up temp files after successful read
    fs::remove_all(temp_dir);
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    result.inference_time_ms = static_cast<float>(duration.count());
    
    result.success = true;
    return result;
}

FeedforwardResult FeedforwardModel::predictMultiView(
    const std::vector<std::vector<uint8_t>>& images_rgba,
    const std::vector<int>& widths,
    const std::vector<int>& heights) {

    FeedforwardResult result;

    if (!impl_->initialized_) {
        result.error_message = "Model not initialized";
        return result;
    }
    if (images_rgba.empty()) {
        result.error_message = "No input views";
        return result;
    }
    if (images_rgba.size() != widths.size() || images_rgba.size() != heights.size()) {
        result.error_message = "Mismatched image/width/height counts";
        return result;
    }

    // Persist every view as a PPM in a temp directory and hand the directory to
    // the bridge. inference.py treats a directory as a multi-view set and calls
    // DA3.inference() exactly once, which is what makes cross-view geometry
    // consistent. (Writing one file per view avoids any in-memory image-size
    // assumptions inside the Python loader.)
    std::string temp_dir = "/tmp/melkor_feedforward_multiview";
    fs::remove_all(temp_dir);
    fs::create_directories(temp_dir);

    for (size_t i = 0; i < images_rgba.size(); ++i) {
        std::string path = temp_dir + "/view_" + std::to_string(i) + ".ppm";
        std::ofstream img(path, std::ios::binary);
        if (!img.is_open()) {
            result.error_message = "Failed to open temp view for write: " + path;
            fs::remove_all(temp_dir);
            return result;
        }
        const int w = widths[i];
        const int h = heights[i];
        img << "P6\n" << w << " " << h << "\n255\n";
        for (int px = 0; px < w * h; ++px) {
            img.put(static_cast<char>(images_rgba[i][px * 4 + 0]));
            img.put(static_cast<char>(images_rgba[i][px * 4 + 1]));
            img.put(static_cast<char>(images_rgba[i][px * 4 + 2]));
        }
    }

    std::string output_ply = temp_dir + "/output.ply";
    if (!impl_->runDA3Inference(temp_dir, output_ply)) {
        result.error_message = "DA3 multi-view inference failed. Run setup_da3.sh first.";
        fs::remove_all(temp_dir);
        return result;
    }

    PlyReader ply_reader;
    auto read_result = ply_reader.readFromFile(output_ply);
    if (!read_result.success) {
        result.error_message = "Failed to parse DA3 output: " + read_result.error_message;
        fs::remove_all(temp_dir);
        return result;
    }
    result.cloud = std::move(read_result.cloud);
    result.num_gaussians = result.cloud.size();
    fs::remove_all(temp_dir);
    result.success = true;
    return result;
}

FeedforwardResult FeedforwardModel::predictFromGlb(
    const std::string& glb_path,
    int num_views) {
    
    FeedforwardResult result;
    
    if (!impl_->initialized_) {
        result.error_message = "Model not initialized";
        return result;
    }
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Load GLB file
    GlbReader reader;
    GlbConversionConfig glb_config;
    auto glb_result = reader.loadFromFile(glb_path, glb_config);
    
    if (!glb_result.success) {
        result.error_message = "Failed to load GLB: " + glb_result.error_message;
        return result;
    }
    
    if (glb_result.cloud.empty()) {
        result.error_message = "GLB file contains no vertices";
        return result;
    }
    
    // Compute bounding box for camera placement
    float minX, minY, minZ, maxX, maxY, maxZ;
    glb_result.cloud.computeBoundingBox(minX, minY, minZ, maxX, maxY, maxZ);
    
    float centerX = (minX + maxX) * 0.5f;
    float centerY = (minY + maxY) * 0.5f;
    float centerZ = (minZ + maxZ) * 0.5f;
    float extent = std::max({maxX - minX, maxY - minY, maxZ - minZ});
    if (extent < 0.0001f) extent = 1.0f; // Guard against degenerate mesh
    float camera_distance = extent * 2.0f;
    
    // Render views from different angles
    int render_width = impl_->config_.input_width;
    int render_height = impl_->config_.input_height;
    
    std::vector<std::vector<uint8_t>> rendered_views;
    std::vector<int> widths, heights;
    
    for (int view = 0; view < num_views; ++view) {
        float angle = (2.0f * 3.14159265f * view) / num_views;
        
        // Camera position orbiting around center
        float camX = centerX + camera_distance * std::cos(angle);
        float camY = centerY;
        float camZ = centerZ + camera_distance * std::sin(angle);
        
        // Render orthographic projection of vertices to image
        std::vector<uint8_t> image_rgba(render_width * render_height * 4, 0);
        
        // Simple z-buffer for depth testing
        std::vector<float> depth_buffer(render_width * render_height, std::numeric_limits<float>::max());
        
        // Compute view direction
        float viewDirX = centerX - camX;
        float viewDirY = centerY - camY;
        float viewDirZ = centerZ - camZ;
        float viewLen = std::sqrt(viewDirX*viewDirX + viewDirY*viewDirY + viewDirZ*viewDirZ);
        if (viewLen < 1e-9f) viewLen = 1.0f;  // Guard against degenerate camera position
        viewDirX /= viewLen;
        viewDirY /= viewLen;
        viewDirZ /= viewLen;
        
        // Up vector (Y-up)
        float upX = 0.0f, upY = 1.0f, upZ = 0.0f;
        
        // Right vector = view x up
        float rightX = viewDirY * upZ - viewDirZ * upY;
        float rightY = viewDirZ * upX - viewDirX * upZ;
        float rightZ = viewDirX * upY - viewDirY * upX;
        float rightLen = std::sqrt(rightX*rightX + rightY*rightY + rightZ*rightZ);
        if (rightLen > 0.001f) {
            rightX /= rightLen;
            rightY /= rightLen;
            rightZ /= rightLen;
        }
        
        // Recompute up = right x view
        upX = rightY * viewDirZ - rightZ * viewDirY;
        upY = rightZ * viewDirX - rightX * viewDirZ;
        upZ = rightX * viewDirY - rightY * viewDirX;
        
        // Project each vertex
        const auto& splats = glb_result.cloud.splats();
        for (const auto& splat : splats) {
            // Vector from camera to vertex
            float dx = splat.x - camX;
            float dy = splat.y - camY;
            float dz = splat.z - camZ;
            
            // Project onto view plane
            float depth = dx * viewDirX + dy * viewDirY + dz * viewDirZ;
            if (depth <= 0) continue; // Behind camera
            
            float projX = dx * rightX + dy * rightY + dz * rightZ;
            float projY = dx * upX + dy * upY + dz * upZ;
            
            // Normalize to image coordinates
            float scale = 1.0f / extent;
            int px = static_cast<int>((projX * scale + 0.5f) * render_width);
            int py = static_cast<int>((0.5f - projY * scale) * render_height);
            
            if (px < 0 || px >= render_width || py < 0 || py >= render_height) continue;
            
            int idx = py * render_width + px;
            if (depth < depth_buffer[idx]) {
                depth_buffer[idx] = depth;
                
                // Convert SH DC to RGB
                float r = utils::shDcToRgb(splat.f_dc_0);
                float g = utils::shDcToRgb(splat.f_dc_1);
                float b = utils::shDcToRgb(splat.f_dc_2);
                
                image_rgba[idx * 4 + 0] = static_cast<uint8_t>(std::clamp(r * 255.0f, 0.0f, 255.0f));
                image_rgba[idx * 4 + 1] = static_cast<uint8_t>(std::clamp(g * 255.0f, 0.0f, 255.0f));
                image_rgba[idx * 4 + 2] = static_cast<uint8_t>(std::clamp(b * 255.0f, 0.0f, 255.0f));
                image_rgba[idx * 4 + 3] = 255;
            }
        }
        
        // Fill small holes with dilation pass
        std::vector<uint8_t> dilated = image_rgba;
        for (int y = 1; y < render_height - 1; ++y) {
            for (int x = 1; x < render_width - 1; ++x) {
                int idx = y * render_width + x;
                if (image_rgba[idx * 4 + 3] == 0) {
                    // Check neighbors
                    int count = 0;
                    float r = 0, g = 0, b = 0;
                    for (int ny = -1; ny <= 1; ++ny) {
                        for (int nx = -1; nx <= 1; ++nx) {
                            int nidx = (y + ny) * render_width + (x + nx);
                            if (image_rgba[nidx * 4 + 3] > 0) {
                                r += image_rgba[nidx * 4 + 0];
                                g += image_rgba[nidx * 4 + 1];
                                b += image_rgba[nidx * 4 + 2];
                                count++;
                            }
                        }
                    }
                    if (count >= 3) {
                        dilated[idx * 4 + 0] = static_cast<uint8_t>(r / count);
                        dilated[idx * 4 + 1] = static_cast<uint8_t>(g / count);
                        dilated[idx * 4 + 2] = static_cast<uint8_t>(b / count);
                        dilated[idx * 4 + 3] = 255;
                    }
                }
            }
        }
        
        rendered_views.push_back(std::move(dilated));
        widths.push_back(render_width);
        heights.push_back(render_height);
    }
    
    // Dispatch to inference. The single-image predict() path writes one PPM
    // and invokes the bridge. For multiple views we cannot use predict()
    // per-image (that would lose DA3's cross-view geometry); instead we write
    // every rendered view and hand the directory to the bridge so inference.py
    // can run a single batched DA3 call. predictMultiView() implements that.
    if (num_views > 1 && impl_->config_.num_input_views > 1) {
        result = predictMultiView(rendered_views, widths, heights);
    } else {
        // Single (front) view for single-view prediction.
        result = predict(rendered_views[0], widths[0], heights[0]);
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    result.inference_time_ms = static_cast<float>(duration.count());

    return result;
}

std::string FeedforwardModel::getModelInfo() const {
    std::stringstream ss;
    ss << "Model: " << impl_->config_.model_type << "\n";
    ss << "Initialized: " << (impl_->initialized_ ? "yes" : "no") << "\n";
    ss << "Input resolution: " << impl_->config_.input_width << "x" << impl_->config_.input_height << "\n";
    return ss.str();
}

} // namespace melkor
