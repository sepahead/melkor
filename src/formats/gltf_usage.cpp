#include "gltf_usage.hpp"

#include "melkor/checked.hpp"
#include "melkor/format/gltf_khr.hpp"

#include <cstddef>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace melkor::format::gltf {
namespace {

struct TraversalEntry {
    std::uint64_t node = 0;
    std::uint64_t depth = 0;
};

Result<DefaultSceneUsage> fail_usage(ErrorCode error, const char* code, const char* message) {
    Diagnostic diagnostic(code, Severity::error, message);
    return Result<DefaultSceneUsage>::failure(error, std::move(diagnostic));
}

bool is_consumed_attribute(std::string_view semantic) {
    if (semantic == khr::kAttrPosition || semantic == khr::kAttrRotation ||
        semantic == khr::kAttrScale || semantic == khr::kAttrOpacity) {
        return true;
    }
    const auto address = khr::parse_sh_attribute(semantic);
    return address.has_value() && address->degree <= khr::kMaxProfileShDegree;
}

Result<std::uint64_t> marker_bytes(const Document& document) {
    auto bytes =
        checked_add(document.nodes.size(), document.meshes.size(), "glTF usage marker bytes");
    if (bytes.has_value()) {
        bytes = checked_add(bytes.value(), document.accessors.size(), "glTF usage marker bytes");
    }
    if (bytes.has_value()) {
        bytes = checked_add(bytes.value(), document.buffer_views.size(), "glTF usage marker bytes");
    }
    if (bytes.has_value()) {
        bytes = checked_add(bytes.value(), document.buffers.size(), "glTF usage marker bytes");
    }
    return bytes;
}

Result<std::uint64_t> traversal_entry_count(const Document& document,
                                            const OperationContext& context) {
    std::uint64_t count = document.scene_roots.size();
    for (std::size_t index = 0; index < document.nodes.size(); ++index) {
        if (index % 4096 == 0) {
            auto control = context.checkpoint(
                {"gltf.read", "plan_edges", index, document.nodes.size(), "nodes"});
            if (!control.has_value()) {
                return Result<std::uint64_t>::failure(control.error_code(), control.diagnostics());
            }
        }
        const NodeDesc& node = document.nodes[index];
        auto next = checked_add(count, node.children.size(), "glTF usage traversal entries");
        if (!next.has_value())
            return next;
        count = next.value();
    }
    auto completed = context.checkpoint(
        {"gltf.read", "plan_edges", document.nodes.size(), document.nodes.size(), "nodes"});
    if (!completed.has_value()) {
        return Result<std::uint64_t>::failure(completed.error_code(), completed.diagnostics());
    }
    return Result<std::uint64_t>::success(count);
}

}  // namespace

Result<DefaultSceneUsage> collect_default_scene_usage(const Document& document,
                                                      const OperationContext& context) try {
    if (context.budget == nullptr) {
        return fail_usage(ErrorCode::internal_error, "MK0310_NO_BUDGET",
                          "the glTF usage planner requires a resource budget");
    }
    auto started = context.check("gltf.read.plan_resources");
    if (!started.has_value()) {
        return Result<DefaultSceneUsage>::failure(started.error_code(), started.diagnostics());
    }

    auto markers = marker_bytes(document);
    auto stack_entries = traversal_entry_count(document, context);
    if (!stack_entries.has_value()) {
        return Result<DefaultSceneUsage>::failure(stack_entries.error_code(),
                                                  stack_entries.diagnostics());
    }
    auto stack_bytes =
        checked_mul(stack_entries.value(), sizeof(TraversalEntry), "glTF usage traversal bytes");
    if (!markers.has_value() || !stack_bytes.has_value()) {
        return fail_usage(ErrorCode::resource_limit, "MK2167_GLTF_BUDGET",
                          "the glTF usage plan size overflows");
    }
    auto working_bytes =
        checked_add(markers.value(), stack_bytes.value(), "glTF usage working memory");
    if (!working_bytes.has_value()) {
        return fail_usage(ErrorCode::resource_limit, "MK2167_GLTF_BUDGET",
                          "the glTF usage plan size overflows");
    }
    auto charge = context.budget->reserve(BudgetKind::memory_bytes, working_bytes.value(),
                                          "gltf.default_scene_usage");
    if (!charge.has_value()) {
        return Result<DefaultSceneUsage>::failure(charge.error_code(), charge.diagnostics());
    }

    DefaultSceneUsage usage;
    usage.memory_charge = std::move(charge).value();
    usage.nodes.resize(document.nodes.size());
    usage.meshes.resize(document.meshes.size());
    usage.accessors.resize(document.accessors.size());
    usage.buffer_views.resize(document.buffer_views.size());
    usage.buffers.resize(document.buffers.size());

    {
        std::vector<TraversalEntry> stack;
        auto stack_size = checked_size_cast(stack_entries.value(), "glTF usage traversal entries");
        if (!stack_size.has_value()) {
            return fail_usage(ErrorCode::resource_limit, "MK2167_GLTF_BUDGET",
                              "the glTF usage traversal size is not representable");
        }
        stack.reserve(stack_size.value());
        for (std::uint64_t root : document.scene_roots) {
            stack.push_back({root, 1});
        }

        while (!stack.empty()) {
            const TraversalEntry entry = stack.back();
            stack.pop_back();
            if (entry.node >= document.nodes.size()) {
                return fail_usage(ErrorCode::invalid_data, "MK2168_GLTF_NODE_GRAPH",
                                  "the default scene has an invalid node index");
            }
            if (entry.depth > context.budget->limits().max_scene_depth) {
                return fail_usage(ErrorCode::resource_limit, "MK2167_GLTF_BUDGET",
                                  "the glTF node graph exceeds the scene-depth limit");
            }
            const std::size_t node_index = static_cast<std::size_t>(entry.node);
            if (usage.nodes[node_index] != 0) {
                return fail_usage(ErrorCode::invalid_data, "MK2168_GLTF_NODE_GRAPH",
                                  "the default scene has a repeated node");
            }
            usage.nodes[node_index] = 1;
            ++usage.node_count;
            if (usage.node_count % 4096 == 0) {
                auto control = context.checkpoint({"gltf.read", "plan_resources", usage.node_count,
                                                   document.nodes.size(), "nodes"});
                if (!control.has_value()) {
                    return Result<DefaultSceneUsage>::failure(control.error_code(),
                                                              control.diagnostics());
                }
            }

            const NodeDesc& node = document.nodes[node_index];
            if (node.mesh.has_value()) {
                if (*node.mesh >= document.meshes.size()) {
                    return fail_usage(ErrorCode::invalid_data, "MK2136_GLTF_BAD_INDEX",
                                      "a default-scene node has an invalid mesh index");
                }
                const std::size_t mesh_index = static_cast<std::size_t>(*node.mesh);
                if (usage.meshes[mesh_index] == 0) {
                    usage.meshes[mesh_index] = 1;
                    ++usage.mesh_count;
                }
                for (const PrimitiveDesc& primitive : document.meshes[mesh_index].primitives) {
                    if (!primitive.gaussian.has_value())
                        continue;
                    for (const auto& [semantic, accessor_index] : primitive.attributes) {
                        if (!is_consumed_attribute(semantic))
                            continue;
                        if (accessor_index >= document.accessors.size()) {
                            return fail_usage(ErrorCode::invalid_data, "MK2136_GLTF_BAD_INDEX",
                                              "a Gaussian attribute has an invalid accessor index");
                        }
                        std::uint8_t& marker =
                            usage.accessors[static_cast<std::size_t>(accessor_index)];
                        if (marker == 0) {
                            marker = 1;
                            ++usage.accessor_count;
                        }
                    }
                }
            }
            for (std::uint64_t child : node.children) {
                stack.push_back({child, entry.depth + 1});
            }
        }
    }

    for (std::size_t index = 0; index < usage.accessors.size(); ++index) {
        if (index % 4096 == 0) {
            auto control = context.checkpoint(
                {"gltf.read", "plan_accessors", index, usage.accessors.size(), "accessors"});
            if (!control.has_value()) {
                return Result<DefaultSceneUsage>::failure(control.error_code(),
                                                          control.diagnostics());
            }
        }
        if (usage.accessors[index] == 0)
            continue;
        const AccessorDesc& accessor = document.accessors[index];
        if (!accessor.has_buffer_view)
            continue;
        if (accessor.buffer_view >= document.buffer_views.size()) {
            return fail_usage(ErrorCode::invalid_data, "MK2136_GLTF_BAD_INDEX",
                              "a used accessor has an invalid bufferView index");
        }
        const std::size_t view_index = static_cast<std::size_t>(accessor.buffer_view);
        if (usage.buffer_views[view_index] == 0) {
            usage.buffer_views[view_index] = 1;
            ++usage.buffer_view_count;
        }
    }
    for (std::size_t index = 0; index < usage.buffer_views.size(); ++index) {
        if (index % 4096 == 0) {
            auto control = context.checkpoint({"gltf.read", "plan_buffer_views", index,
                                               usage.buffer_views.size(), "buffer_views"});
            if (!control.has_value()) {
                return Result<DefaultSceneUsage>::failure(control.error_code(),
                                                          control.diagnostics());
            }
        }
        if (usage.buffer_views[index] == 0)
            continue;
        const std::uint64_t buffer_index = document.buffer_views[index].buffer;
        if (buffer_index >= document.buffers.size()) {
            return fail_usage(ErrorCode::invalid_data, "MK2136_GLTF_BAD_INDEX",
                              "a used bufferView has an invalid buffer index");
        }
        std::uint8_t& marker = usage.buffers[static_cast<std::size_t>(buffer_index)];
        if (marker == 0) {
            marker = 1;
            ++usage.buffer_count;
        }
    }

    usage.memory_charge.shrink_to(markers.value());
    return Result<DefaultSceneUsage>::success(std::move(usage));
} catch (const std::bad_alloc&) {
    return fail_usage(ErrorCode::resource_limit, "MK2167_GLTF_BUDGET",
                      "the glTF usage plan allocation failed");
} catch (const std::length_error&) {
    return fail_usage(ErrorCode::resource_limit, "MK2167_GLTF_BUDGET",
                      "the glTF usage plan exceeds a container limit");
}

}  // namespace melkor::format::gltf
