#include "melkor/ply_writer.hpp"
#include <fstream>
#include <sstream>
#include <cstring>
#include <iomanip>

namespace melkor {

PlyWriter::PlyWriter() = default;
PlyWriter::~PlyWriter() = default;

PlyWriteResult PlyWriter::writeToFile(const std::string& filepath,
                                      const GaussianCloud& cloud,
                                      const PlyWriteConfig& config) {
    std::ofstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        return {false, "Failed to open file for writing: " + filepath, 0};
    }
    
    return writeToStream(file, cloud, config);
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
            stream.write(reinterpret_cast<const char*>(&splat.x), sizeof(float));
            stream.write(reinterpret_cast<const char*>(&splat.y), sizeof(float));
            stream.write(reinterpret_cast<const char*>(&splat.z), sizeof(float));
            
            // Normals (dummy values)
            float nx = 0.0f, ny = 0.0f, nz = 1.0f;
            stream.write(reinterpret_cast<const char*>(&nx), sizeof(float));
            stream.write(reinterpret_cast<const char*>(&ny), sizeof(float));
            stream.write(reinterpret_cast<const char*>(&nz), sizeof(float));
            
            // DC color
            stream.write(reinterpret_cast<const char*>(&splat.f_dc_0), sizeof(float));
            stream.write(reinterpret_cast<const char*>(&splat.f_dc_1), sizeof(float));
            stream.write(reinterpret_cast<const char*>(&splat.f_dc_2), sizeof(float));
            
            // SH rest coefficients
            for (int i = 0; i < sh_rest_count; ++i) {
                float val = (i < static_cast<int>(splat.sh_rest.size())) ? splat.sh_rest[i] : 0.0f;
                stream.write(reinterpret_cast<const char*>(&val), sizeof(float));
            }
            
            // Opacity
            stream.write(reinterpret_cast<const char*>(&splat.opacity), sizeof(float));
            
            // Scale
            stream.write(reinterpret_cast<const char*>(&splat.scale_0), sizeof(float));
            stream.write(reinterpret_cast<const char*>(&splat.scale_1), sizeof(float));
            stream.write(reinterpret_cast<const char*>(&splat.scale_2), sizeof(float));
            
            // Rotation
            stream.write(reinterpret_cast<const char*>(&splat.rot_0), sizeof(float));
            stream.write(reinterpret_cast<const char*>(&splat.rot_1), sizeof(float));
            stream.write(reinterpret_cast<const char*>(&splat.rot_2), sizeof(float));
            stream.write(reinterpret_cast<const char*>(&splat.rot_3), sizeof(float));
            
            result.bytes_written += (6 + 3 + sh_rest_count + 1 + 3 + 4) * sizeof(float);
        }
    } else {
        // ASCII format
        stream << std::setprecision(8);
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

PlyReader::ReadResult PlyReader::readFromFile(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        return {false, "Failed to open file: " + filepath, {}};
    }
    
    // Read entire file into memory
    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    std::vector<uint8_t> buffer(size);
    file.read(reinterpret_cast<char*>(buffer.data()), size);
    
    return readFromBuffer(buffer.data(), buffer.size());
}

PlyReader::ReadResult PlyReader::readFromBuffer(const uint8_t* data, size_t size) {
    ReadResult result;
    result.success = true;
    
    // Find end of header
    std::string_view view(reinterpret_cast<const char*>(data), size);
    auto header_end = view.find("end_header\n");
    if (header_end == std::string::npos) {
        return {false, "Invalid PLY: no end_header found", {}};
    }
    header_end += 11;  // length of "end_header\n"
    
    std::string header(view.substr(0, header_end));
    
    // Parse header
    bool is_binary = header.find("binary_little_endian") != std::string::npos;
    
    // Find vertex count
    size_t vertex_count = 0;
    auto elem_pos = header.find("element vertex ");
    if (elem_pos != std::string::npos) {
        vertex_count = std::stoull(header.substr(elem_pos + 15));
    }
    
    if (vertex_count == 0) {
        return {false, "Invalid PLY: no vertices found", {}};
    }
    
    // Count SH rest properties
    int sh_rest_count = 0;
    size_t search_pos = 0;
    while ((search_pos = header.find("property float f_rest_", search_pos)) != std::string::npos) {
        sh_rest_count++;
        search_pos++;
    }
    
    result.cloud.reserve(vertex_count);
    
    const uint8_t* vertex_data = data + header_end;
    
    if (is_binary) {
        // Binary reading
        size_t stride = (6 + 3 + sh_rest_count + 1 + 3 + 4) * sizeof(float);
        
        for (size_t i = 0; i < vertex_count; ++i) {
            const float* f = reinterpret_cast<const float*>(vertex_data + i * stride);
            
            GaussianSplat splat;
            splat.x = f[0];
            splat.y = f[1];
            splat.z = f[2];
            // Skip normals (f[3], f[4], f[5])
            splat.f_dc_0 = f[6];
            splat.f_dc_1 = f[7];
            splat.f_dc_2 = f[8];
            
            int idx = 9;
            if (sh_rest_count > 0) {
                splat.sh_rest.resize(sh_rest_count);
                for (int j = 0; j < sh_rest_count; ++j) {
                    splat.sh_rest[j] = f[idx++];
                }
            }
            
            splat.opacity = f[idx++];
            splat.scale_0 = f[idx++];
            splat.scale_1 = f[idx++];
            splat.scale_2 = f[idx++];
            splat.rot_0 = f[idx++];
            splat.rot_1 = f[idx++];
            splat.rot_2 = f[idx++];
            splat.rot_3 = f[idx++];
            
            result.cloud.addSplat(std::move(splat));
        }
    } else {
        // ASCII reading
        std::istringstream stream(std::string(reinterpret_cast<const char*>(vertex_data),
                                              size - header_end));
        
        for (size_t i = 0; i < vertex_count; ++i) {
            GaussianSplat splat;
            float nx, ny, nz;  // dummy normals
            
            stream >> splat.x >> splat.y >> splat.z;
            stream >> nx >> ny >> nz;
            stream >> splat.f_dc_0 >> splat.f_dc_1 >> splat.f_dc_2;
            
            if (sh_rest_count > 0) {
                splat.sh_rest.resize(sh_rest_count);
                for (int j = 0; j < sh_rest_count; ++j) {
                    stream >> splat.sh_rest[j];
                }
            }
            
            stream >> splat.opacity;
            stream >> splat.scale_0 >> splat.scale_1 >> splat.scale_2;
            stream >> splat.rot_0 >> splat.rot_1 >> splat.rot_2 >> splat.rot_3;
            
            result.cloud.addSplat(std::move(splat));
        }
    }
    
    return result;
}

} // namespace melkor
