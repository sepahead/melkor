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

    // Find end of header. Accept both "end_header\n" and "end_header\r\n".
    std::string_view view(reinterpret_cast<const char*>(data), size);
    auto header_end = view.find("end_header\n");
    if (header_end == std::string::npos) {
        return {false, "Invalid PLY: no end_header found", {}};
    }
    size_t header_bytes = header_end + std::string_view("end_header\n").size();

    // Walk the header line by line to parse the vertex element's property list.
    // The reader is now header-driven: we map each property name to its index
    // within the vertex record, so PLYs authored by different tools (e.g. those
    // using red/green/blue instead of f_dc_*, or omitting normals) are parsed
    // correctly instead of silently misaligned.
    struct Property {
        std::string name;
        std::string type;  // "float", "double", "uchar", "char", "ushort", "short", "uint", "int"
    };
    std::vector<Property> vertex_props;
    size_t vertex_count = 0;
    bool is_binary = false;
    bool in_vertex = false;

    {
        std::string header_str(view.substr(0, header_bytes));
        std::istringstream hs(header_str);
        std::string line;
        while (std::getline(hs, line)) {
            // Strip trailing CR.
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("format binary", 0) == 0) {
                is_binary = true;
            }
            if (line.rfind("element ", 0) == 0) {
                in_vertex = (line.find("element vertex ") == 0);
                if (in_vertex) {
                    // "element vertex <count>"
                    auto p = line.find_first_of("0123456789");
                    if (p != std::string::npos) {
                        vertex_count = std::stoull(line.substr(p));
                    }
                }
            } else if (line.rfind("property ", 0) == 0 && in_vertex) {
                // Forms: "property <type> <name>" or "property <count> <type> <name>"
                // For splat PLYs we only care about scalar properties; list
                // properties ("property list ...") are skipped.
                if (line.find("property list") == std::string::npos) {
                    std::istringstream ls(line);
                    std::string kw, t, n;
                    ls >> kw >> t >> n;
                    if (!n.empty()) {
                        vertex_props.push_back({n, t});
                    }
                }
            }
        }
    }

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
        return {false, "Invalid PLY: no vertex properties found", {}};
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

    // Gather f_rest_* indices in order.
    std::vector<int> sh_rest_idx;
    for (const auto& p : vertex_props) {
        if (p.name.rfind("f_rest_", 0) == 0) {
            sh_rest_idx.push_back(find_idx(p.name));
        }
    }
    if (!sh_rest_idx.empty()) {
        result.cloud.setShDegree([&] {
            switch (static_cast<int>(sh_rest_idx.size())) {
                case 9: return 1;
                case 24: return 2;
                case 45: return 3;
                default: return 0;
            }
        }());
    }

    if (ix < 0 || iy < 0 || iz < 0) {
        return {false, "Invalid PLY: missing x/y/z position properties", {}};
    }

    // Byte size of one property value by type.
    auto type_size = [](const std::string& t) -> size_t {
        if (t == "float" || t == "int" || t == "uint") return 4;
        if (t == "double") return 8;
        if (t == "uchar" || t == "char") return 1;
        if (t == "ushort" || t == "short") return 2;
        return 4;  // default to float
    };
    size_t stride = 0;
    for (const auto& p : vertex_props) stride += type_size(p.type);

    result.cloud.reserve(vertex_count);
    const uint8_t* vertex_data = data + header_bytes;

    // Read a single property value (at the given property index) for the
    // current vertex record starting at `base`, normalized to float.
    auto read_prop = [&](const uint8_t* base, int prop_idx) -> float {
        const auto& p = vertex_props[static_cast<size_t>(prop_idx)];
        size_t off = 0;
        for (int i = 0; i < prop_idx; ++i) off += type_size(vertex_props[static_cast<size_t>(i)].type);
        const uint8_t* v = base + off;
        if (p.type == "float") {
            float f; std::memcpy(&f, v, 4); return f;
        } else if (p.type == "double") {
            double d; std::memcpy(&d, v, 8); return static_cast<float>(d);
        } else if (p.type == "int") {
            int32_t i; std::memcpy(&i, v, 4); return static_cast<float>(i);
        } else if (p.type == "uint") {
            uint32_t u; std::memcpy(&u, v, 4); return static_cast<float>(u);
        } else if (p.type == "uchar") {
            return static_cast<float>(*v);
        } else if (p.type == "char") {
            return static_cast<float>(static_cast<signed char>(*v));
        } else if (p.type == "ushort") {
            uint16_t u; std::memcpy(&u, v, 2); return static_cast<float>(u);
        } else if (p.type == "short") {
            int16_t s; std::memcpy(&s, v, 2); return static_cast<float>(s);
        }
        float f; std::memcpy(&f, v, 4); return f;
    };

    auto color_byte_to_shdc = [](float c) {
        // 0..255 uint8 stored as float -> [0,1] -> SH DC.
        return utils::rgbToShDc(c / 255.0f);
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
                splat.f_dc_0 = color_byte_to_shdc(read_prop(base, ir));
                splat.f_dc_1 = color_byte_to_shdc(read_prop(base, ig));
                splat.f_dc_2 = color_byte_to_shdc(read_prop(base, ib));
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
        for (size_t i = 0; i < vertex_count; ++i) {
            for (size_t j = 0; j < vertex_props.size(); ++j) {
                stream >> vals[j];
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
                splat.f_dc_0 = color_byte_to_shdc(vals[ir]);
                splat.f_dc_1 = color_byte_to_shdc(vals[ig]);
                splat.f_dc_2 = color_byte_to_shdc(vals[ib]);
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
}

} // namespace melkor
