/* Test the installed Melkor C ABI from a standalone C project. */

#include <melkor/c/melkor.h>

#include <stdio.h>
#include <string.h>

typedef char
    melkor_status_typedef_must_be_32_bits[sizeof(melkor_status) == sizeof(uint32_t) ? 1 : -1];

static melkor_status status_from_typedef(void) {
    return MELKOR_RESOURCE_LIMIT;
}

static enum melkor_status status_from_enum_tag(void) {
    return MELKOR_RESOURCE_LIMIT;
}

static int write_text_file(const char* path, const char* text) {
    FILE* file = fopen(path, "wb");
    if (file == NULL) {
        return 0;
    }

    const size_t size = strlen(text);
    const int wrote_all = fwrite(text, 1, size, file) == size;
    const int closed = fclose(file) == 0;
    return wrote_all && closed;
}

static int write_oversized_web_header(const char* path) {
    FILE* file = fopen(path, "wb");
    if (file == NULL) {
        return 0;
    }
    if (fputs("ply\nformat ascii 1.0\n", file) < 0) {
        fclose(file);
        return 0;
    }
    for (size_t line = 0; line < 120000; ++line) {
        if (fputs("comment x\n", file) < 0) {
            fclose(file);
            return 0;
        }
    }
    return fclose(file) == 0;
}

static int require_status(melkor_status actual, melkor_status expected, const char* operation) {
    if (actual == expected) {
        return 1;
    }
    fprintf(stderr, "%s returned %s. Expected %s.\n", operation, melkor_status_string(actual),
            melkor_status_string(expected));
    return 0;
}

int main(void) {
    static const char fixture[] = "ply\n"
                                  "format ascii 1.0\n"
                                  "comment melkor_profile da3-gaussian-v1\n"
                                  "comment melkor_coordinate_system ply-rdf\n"
                                  "comment melkor_length_unit meter\n"
                                  "comment melkor_color_space lin_rec709_display\n"
                                  "comment melkor_quaternion_order wxyz\n"
                                  "comment melkor_scale_domain log\n"
                                  "comment melkor_opacity_domain logit\n"
                                  "comment melkor_sh_basis real_condon_shortley\n"
                                  "comment melkor_sh_degree 1\n"
                                  "comment melkor_antialiased 0\n"
                                  "comment source note\n"
                                  "element vertex 2\n"
                                  "property float x\n"
                                  "property float y\n"
                                  "property float z\n"
                                  "property float f_dc_0\n"
                                  "property float f_dc_1\n"
                                  "property float f_dc_2\n"
                                  "property float opacity\n"
                                  "property float scale_0\n"
                                  "property float scale_1\n"
                                  "property float scale_2\n"
                                  "property float rot_0\n"
                                  "property float rot_1\n"
                                  "property float rot_2\n"
                                  "property float rot_3\n"
                                  "property float f_rest_0\n"
                                  "property float f_rest_1\n"
                                  "property float f_rest_2\n"
                                  "property float f_rest_3\n"
                                  "property float f_rest_4\n"
                                  "property float f_rest_5\n"
                                  "property float f_rest_6\n"
                                  "property float f_rest_7\n"
                                  "property float f_rest_8\n"
                                  "property float custom\n"
                                  "end_header\n"
                                  "0 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0\n"
                                  "1 0 0 0 0 0 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0\n";
    static const char fixture_path[] = "melkor-sdk-consumer.PLY";

    const melkor_status typedef_status = status_from_typedef();
    const enum melkor_status tagged_status = status_from_enum_tag();
    melkor_status (*const typedef_get_version)(melkor_version_info*) = &melkor_get_version;
    enum melkor_status (*const tagged_get_version)(melkor_version_info*) = &melkor_get_version;
    (void)typedef_get_version;
    (void)tagged_get_version;
    if (typedef_status != MELKOR_RESOURCE_LIMIT || tagged_status != MELKOR_RESOURCE_LIMIT) {
        fprintf(stderr, "The status declarations have incorrect values.\n");
        return 1;
    }

    melkor_version_info version;
    memset(&version, 0, sizeof(version));
    version.struct_size = sizeof(version);
    if (!require_status(melkor_get_version(&version), MELKOR_OK, "melkor_get_version")) {
        return 1;
    }
    if (version.abi_version != MELKOR_C_ABI_VERSION) {
        fprintf(stderr, "The C header and the linked library use different ABI versions.\n");
        return 1;
    }

    {
        static const char untouched[] = "untouched";
        melkor_version_info partial;
        memset(&partial, 0, sizeof(partial));
        partial.struct_size =
            offsetof(melkor_version_info, version_string) + sizeof(partial.version_string) - 1;
        partial.version_string = untouched;
        if (!require_status(melkor_get_version(&partial), MELKOR_OK, "partial-version query") ||
            partial.version_string != untouched) {
            fprintf(stderr, "A partial version field was changed.\n");
            return 1;
        }
    }

    if (!write_text_file(fixture_path, fixture)) {
        fprintf(stderr, "Could not write the PLY test file.\n");
        return 1;
    }

    melkor_ply_inspect_options options = MELKOR_PLY_INSPECT_OPTIONS_INIT;
    options.limits_profile = MELKOR_LIMITS_PROFILE_WEB;
    options.profile = MELKOR_PLY_PROFILE_DA3_GAUSSIAN;
    options.source_frame = MELKOR_COORDINATE_FRAME_PLY_RDF;
    options.source_color_space = MELKOR_COLOR_SPACE_LIN_REC709_DISPLAY;
    options.source_unit_to_meter = 1.0;
    melkor_ply_info info = MELKOR_PLY_INFO_INIT;
    if (!require_status(melkor_inspect_ply_file(fixture_path, &options, &info), MELKOR_OK,
                        "melkor_inspect_ply_file")) {
        remove(fixture_path);
        return 1;
    }
    if (info.splat_count != 2 || info.sh_degree != 1 ||
        info.profile != MELKOR_PLY_PROFILE_DA3_GAUSSIAN ||
        info.source_frame != MELKOR_COORDINATE_FRAME_PLY_RDF ||
        info.source_color_space != MELKOR_COLOR_SPACE_LIN_REC709_DISPLAY ||
        info.antialiased != MELKOR_OPTIONAL_BOOL_FALSE || info.source_unit_to_meter != 1.0 ||
        info.loss_count != UINT64_C(2) || info.blocking_loss_count != UINT64_C(2)) {
        fprintf(stderr, "PLY inspection returned incorrect metadata.\n");
        remove(fixture_path);
        return 1;
    }

    struct future_options {
        melkor_ply_inspect_options current;
        uint64_t future_field;
    } future_options = {MELKOR_PLY_INSPECT_OPTIONS_INIT, UINT64_C(0x1122334455667788)};
    future_options.current.struct_size = sizeof(struct future_options);
    future_options.current.limits_profile = MELKOR_LIMITS_PROFILE_SERVER;
    struct future_info {
        melkor_ply_info current;
        uint64_t future_field;
    } future_info = {MELKOR_PLY_INFO_INIT, UINT64_C(0x8877665544332211)};
    future_info.current.struct_size = sizeof(struct future_info);
    if (!require_status(
            melkor_inspect_ply_file(fixture_path, &future_options.current, &future_info.current),
            MELKOR_OK, "extended-structure PLY inspection") ||
        future_info.current.struct_size != sizeof(struct future_info) ||
        future_info.current.splat_count != UINT64_C(2) ||
        future_info.current.sh_degree != UINT32_C(1) ||
        future_info.future_field != UINT64_C(0x8877665544332211)) {
        fprintf(stderr, "PLY inspection did not preserve an unknown trailing field.\n");
        remove(fixture_path);
        return 1;
    }

    info = (melkor_ply_info)MELKOR_PLY_INFO_INIT;
    if (!require_status(melkor_inspect_ply_file(fixture_path, NULL, &info), MELKOR_OK,
                        "default-profile PLY inspection")) {
        remove(fixture_path);
        return 1;
    }

    options = (melkor_ply_inspect_options)MELKOR_PLY_INSPECT_OPTIONS_INIT;
    options.struct_size = offsetof(melkor_ply_inspect_options, profile);
    options.limits_profile = MELKOR_LIMITS_PROFILE_WEB;
    options.profile = UINT32_C(999);
    if (!require_status(melkor_inspect_ply_file(fixture_path, &options, &info), MELKOR_OK,
                        "short-compatible options PLY inspection")) {
        remove(fixture_path);
        return 1;
    }

    info = (melkor_ply_info)MELKOR_PLY_INFO_INIT;
    info.struct_size = offsetof(melkor_ply_info, reserved) + sizeof(info.reserved) - 1;
    info.reserved = UINT32_C(0xa5a5a5a5);
    if (!require_status(melkor_inspect_ply_file(fixture_path, NULL, &info), MELKOR_OK,
                        "partial-result PLY inspection") ||
        info.splat_count != UINT64_C(2) || info.sh_degree != UINT32_C(1) ||
        info.reserved != UINT32_C(0xa5a5a5a5)) {
        fprintf(stderr, "A partial PLY result field was changed.\n");
        remove(fixture_path);
        return 1;
    }

    {
        static const char covariance_overflow_fixture[] =
            "ply\n"
            "format ascii 1.0\n"
            "comment melkor_profile melkor-canonical-v1\n"
            "comment melkor_coordinate_system gltf-luf\n"
            "comment melkor_length_unit meter\n"
            "comment melkor_color_space srgb_rec709_display\n"
            "comment melkor_quaternion_order xyzw\n"
            "comment melkor_scale_domain linear\n"
            "comment melkor_opacity_domain linear\n"
            "comment melkor_sh_basis real_condon_shortley\n"
            "comment melkor_sh_degree 0\n"
            "element vertex 1\n"
            "property float x\nproperty float y\nproperty float z\n"
            "property float scale_x\nproperty float scale_y\nproperty float scale_z\n"
            "property float rotation_x\nproperty float rotation_y\n"
            "property float rotation_z\nproperty float rotation_w\n"
            "property float opacity\n"
            "property float sh_0_0_r\nproperty float sh_0_0_g\nproperty float sh_0_0_b\n"
            "end_header\n"
            "0 0 0 1e30 1 1 0 0 0 1 0.5 0 0 0\n";
        info.splat_count = UINT64_C(71);
        info.sh_degree = UINT32_C(4);
        if (!write_text_file(fixture_path, covariance_overflow_fixture) ||
            !require_status(melkor_inspect_ply_file(fixture_path, NULL, &info), MELKOR_OK,
                            "large-scale PLY inspection") ||
            info.splat_count != UINT64_C(1) || info.sh_degree != UINT32_C(0)) {
            fprintf(stderr, "A valid finite scale was rejected or reported incorrectly.\n");
            remove(fixture_path);
            return 1;
        }
    }

    options = (melkor_ply_inspect_options)MELKOR_PLY_INSPECT_OPTIONS_INIT;
    options.limits_profile = MELKOR_LIMITS_PROFILE_WEB;
    if (!write_oversized_web_header(fixture_path) ||
        !require_status(melkor_inspect_ply_file(fixture_path, &options, &info),
                        MELKOR_RESOURCE_LIMIT, "resource-limited PLY inspection") ||
        info.splat_count != UINT64_C(1) || info.sh_degree != UINT32_C(0)) {
        fprintf(stderr, "A resource-limited call changed the output structure.\n");
        remove(fixture_path);
        return 1;
    }

    options.limits_profile = UINT32_C(999);
    info.splat_count = UINT64_C(99);
    info.sh_degree = UINT32_C(4);
    if (!require_status(melkor_inspect_ply_file(fixture_path, &options, &info),
                        MELKOR_INVALID_ARGUMENT, "invalid-profile PLY inspection") ||
        info.splat_count != UINT64_C(99) || info.sh_degree != UINT32_C(4)) {
        fprintf(stderr, "A failed call changed the output structure.\n");
        remove(fixture_path);
        return 1;
    }

    options = (melkor_ply_inspect_options)MELKOR_PLY_INSPECT_OPTIONS_INIT;
    options.struct_size = sizeof(options.struct_size);
    if (!require_status(melkor_inspect_ply_file(fixture_path, &options, &info),
                        MELKOR_INVALID_ARGUMENT, "short-options PLY inspection")) {
        remove(fixture_path);
        return 1;
    }

    options = (melkor_ply_inspect_options)MELKOR_PLY_INSPECT_OPTIONS_INIT;
    info = (melkor_ply_info)MELKOR_PLY_INFO_INIT;
    info.struct_size = sizeof(info.struct_size);
    if (!require_status(melkor_inspect_ply_file(fixture_path, &options, &info),
                        MELKOR_INVALID_ARGUMENT, "short-result PLY inspection")) {
        remove(fixture_path);
        return 1;
    }

    info = (melkor_ply_info)MELKOR_PLY_INFO_INIT;
    {
        static const char invalid_utf8_path[] = {'b', 'a', 'd', (char)0xc0, (char)0xaf, '\0'};
        info.splat_count = UINT64_C(77);
        if (!require_status(melkor_inspect_ply_file(invalid_utf8_path, &options, &info),
                            MELKOR_INVALID_ARGUMENT, "invalid-UTF-8 path") ||
            info.splat_count != UINT64_C(77)) {
            remove(fixture_path);
            return 1;
        }
    }
    {
        char oversized_path[32770];
        memset(oversized_path, 'a', sizeof(oversized_path));
        oversized_path[sizeof(oversized_path) - 1] = '\0';
        info.splat_count = UINT64_C(78);
        if (!require_status(melkor_inspect_ply_file(oversized_path, &options, &info),
                            MELKOR_INVALID_ARGUMENT, "oversized path") ||
            info.splat_count != UINT64_C(78)) {
            remove(fixture_path);
            return 1;
        }
    }

    info = (melkor_ply_info)MELKOR_PLY_INFO_INIT;
    if (!write_text_file("scene.data", fixture) ||
        !require_status(melkor_inspect_ply_file("scene.data", &options, &info), MELKOR_OK,
                        "suffix-independent PLY inspection") ||
        remove("scene.data") != 0 ||
        !require_status(melkor_inspect_ply_file("missing.ply", &options, &info), MELKOR_IO_ERROR,
                        "missing-file PLY inspection")) {
        remove(fixture_path);
        return 1;
    }

    if (!write_text_file(fixture_path, "not a PLY file\n") ||
        !require_status(melkor_inspect_ply_file(fixture_path, &options, &info), MELKOR_INVALID_DATA,
                        "invalid-data PLY inspection")) {
        remove(fixture_path);
        return 1;
    }

    if (remove(fixture_path) != 0) {
        fprintf(stderr, "Could not remove the PLY test file.\n");
        return 1;
    }

    printf("Melkor C SDK consumed successfully.\n");
    printf("  version: %s\n", version.version_string);
    printf("  abi:     %u\n", version.abi_version);
    return 0;
}
