#!/usr/bin/env python3
"""Test the CMake feature setting contract."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path


def configure(
    cmake: str,
    source_dir: Path,
    *definitions: str,
    environment: dict[str, str] | None = None,
) -> tuple[subprocess.CompletedProcess[str], str]:
    """Configure a new build tree and return its cache text."""
    with tempfile.TemporaryDirectory(prefix="melkor-cmake-feature-") as temp_dir:
        build_dir = Path(temp_dir) / "build"
        command = [
            cmake,
            "-S",
            str(source_dir),
            "-B",
            str(build_dir),
            "-DBUILD_TESTING=OFF",
            "-DMELKOR_INSTALL=OFF",
        ]
        command.extend(f"-D{definition}" for definition in definitions)
        process_environment = os.environ.copy()
        if environment is not None:
            process_environment.update(environment)
        result = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            env=process_environment,
        )
        cache_path = build_dir / "CMakeCache.txt"
        cache = cache_path.read_text(encoding="utf-8") if cache_path.is_file() else ""
        return result, cache


def output(result: subprocess.CompletedProcess[str]) -> str:
    return result.stdout + result.stderr


def require_success(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode != 0:
        raise AssertionError(f"{label} failed:\n{output(result)}")


def require_failure(
    result: subprocess.CompletedProcess[str],
    expected: str,
    label: str,
) -> None:
    text = output(result)
    if result.returncode == 0:
        raise AssertionError(f"{label} unexpectedly succeeded")
    if expected not in text:
        raise AssertionError(f"{label} omitted '{expected}':\n{text}")


def verify_embedded_build(cmake: str, source_dir: Path) -> None:
    """Verify that add_subdirectory does not add Melkor tests."""
    with tempfile.TemporaryDirectory(prefix="melkor-cmake-parent-") as temp_dir:
        parent = Path(temp_dir)
        melkor_source = source_dir.as_posix().replace('"', '\\"')
        (parent / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.24)\n"
            "project(melkor_parent LANGUAGES CXX)\n"
            "set(MELKOR_BUILD_SPZ OFF)\n"
            f'add_subdirectory("{melkor_source}" melkor)\n',
            encoding="utf-8",
        )
        build_dir = parent / "build"
        configured = subprocess.run(
            [cmake, "-S", str(parent), "-B", str(build_dir)],
            check=False,
            capture_output=True,
            text=True,
        )
        require_success(configured, "Embedded Melkor configuration")
        targets = subprocess.run(
            [cmake, "--build", str(build_dir), "--target", "help"],
            check=False,
            capture_output=True,
            text=True,
        )
        require_success(targets, "Embedded Melkor target listing")
        if "test_safety_substrate" in output(targets):
            raise AssertionError("Embedded Melkor configuration added repository tests")


def verify_spz_license_requirement(cmake: str, source_dir: Path) -> None:
    """Verify that a custom SPZ source supplies its license."""
    with tempfile.TemporaryDirectory(prefix="melkor-spz-license-") as temp_dir:
        fake_spz = Path(temp_dir) / "spz"
        fake_spz.mkdir()
        (fake_spz / "CMakeLists.txt").write_text(
            "add_library(spz INTERFACE)\n",
            encoding="utf-8",
        )
        result, _ = configure(
            cmake,
            source_dir,
            "MELKOR_BUILD_SPZ=ON",
            f"MELKOR_SPZ_SOURCE_DIR={fake_spz}",
        )
        require_failure(result, "regular LICENSE file", "Missing custom SPZ license")

        license_target = fake_spz / "license-target"
        license_target.write_text("test license\n", encoding="utf-8")
        try:
            (fake_spz / "LICENSE").symlink_to(license_target.name)
        except OSError:
            pass
        else:
            result, _ = configure(
                cmake,
                source_dir,
                "MELKOR_BUILD_SPZ=ON",
                f"MELKOR_SPZ_SOURCE_DIR={fake_spz}",
            )
            require_failure(result, "regular LICENSE file", "Linked custom SPZ license")


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_cmake_features.py <cmake> <source-dir>")

    cmake = sys.argv[1]
    source_dir = Path(sys.argv[2]).resolve()

    verify_embedded_build(cmake, source_dir)
    verify_spz_license_requirement(cmake, source_dir)

    result, cache = configure(
        cmake,
        source_dir,
        "MELKOR_BUILD_SPZ=off",
    )
    require_success(result, "The SPZ-disabled configuration")
    if "MELKOR_BUILD_SPZ:STRING=OFF" not in cache:
        raise AssertionError("CMake did not normalize MELKOR_BUILD_SPZ")
    if "CMAKE_OBJCXX_COMPILER:" in cache:
        raise AssertionError("The format-only configuration enabled Objective-C++")
    if sys.platform == "darwin" and "CMAKE_OSX_DEPLOYMENT_TARGET:STRING=13.0" not in cache:
        raise AssertionError("CMake did not set the default macOS deployment target before project")

    if sys.platform == "darwin":
        result, cache = configure(
            cmake,
            source_dir,
            "MELKOR_BUILD_SPZ=OFF",
            "CMAKE_OSX_DEPLOYMENT_TARGET=14.0",
        )
        require_success(result, "Explicit macOS deployment target")
        if "CMAKE_OSX_DEPLOYMENT_TARGET:STRING=14.0" not in cache:
            raise AssertionError("CMake did not preserve the explicit macOS deployment target")

    result, _ = configure(
        cmake,
        source_dir,
        "MELKOR_BUILD_SPZ=OFF",
        f"MELKOR_BUILD_COMMIT={'a' * 40}",
        environment={"SOURCE_DATE_EPOCH": "4294967295"},
    )
    require_success(result, "Valid build identity")

    result, _ = configure(
        cmake,
        source_dir,
        "MELKOR_BUILD_SPZ=OFF",
        "MELKOR_BUILD_COMMIT=not-a-commit",
    )
    require_failure(result, "MELKOR_BUILD_COMMIT must be", "Invalid build commit")

    with tempfile.TemporaryDirectory(prefix="melkor-version-width-") as temp_dir:
        source_copy = Path(temp_dir) / "source"
        copied_cmake = source_copy / "cmake" / "MelkorVersion.cmake"
        copied_cmake.parent.mkdir(parents=True)
        copied_cmake.write_bytes(
            (source_dir / "cmake" / "MelkorVersion.cmake").read_bytes()
        )
        (source_copy / "VERSION").write_text("99999999999.0.0\n", encoding="utf-8")
        result = subprocess.run(
            [cmake, "-P", str(copied_cmake)],
            check=False,
            capture_output=True,
            text=True,
        )
        require_failure(result, "numeric core field exceeds uint32", "Oversized version core")

    result, _ = configure(
        cmake,
        source_dir,
        "MELKOR_BUILD_SPZ=OFF",
        environment={"SOURCE_DATE_EPOCH": '1" invalid'},
    )
    require_failure(result, "Invalid SOURCE_DATE_EPOCH", "Invalid source epoch")

    result, _ = configure(
        cmake,
        source_dir,
        "MELKOR_BUILD_SPZ=OFF",
        environment={"SOURCE_DATE_EPOCH": "4294967296"},
    )
    require_failure(result, "Invalid SOURCE_DATE_EPOCH", "Oversized source epoch")

    result, _ = configure(
        cmake,
        source_dir,
        "MELKOR_BUILD_SPZ=INVALID",
    )
    require_failure(
        result,
        "MELKOR_BUILD_SPZ must be AUTO, ON, or OFF",
        "Invalid SPZ setting",
    )

    result, _ = configure(
        cmake,
        source_dir,
        "MELKOR_BUILD_SPZ=ON",
        "MELKOR_SPZ_SOURCE_DIR=/melkor-intentionally-missing-spz",
    )
    require_failure(result, "MELKOR_BUILD_SPZ=ON requires", "Missing SPZ source")

    result, cache = configure(
        cmake,
        source_dir,
        "MELKOR_BUILD_SPZ=AUTO",
        "MELKOR_SPZ_SOURCE_DIR=/melkor-intentionally-missing-spz",
    )
    require_success(result, "Automatic SPZ detection")
    if "MELKOR_BUILD_SPZ:STRING=AUTO" not in cache:
        raise AssertionError("CMake did not preserve the SPZ AUTO setting")

    result, _ = configure(
        cmake,
        source_dir,
        "MELKOR_BUILD_SPZ=AUTO",
        "CMAKE_DISABLE_FIND_PACKAGE_ZLIB=TRUE",
    )
    require_success(result, "Automatic SPZ detection without zlib")
    if "zlib is unavailable. SPZ support is disabled." not in output(result):
        raise AssertionError("Automatic SPZ detection did not report the missing zlib")

    result, _ = configure(
        cmake,
        source_dir,
        "MELKOR_BUILD_SPZ=ON",
        "CMAKE_DISABLE_FIND_PACKAGE_ZLIB=TRUE",
    )
    require_failure(result, "MELKOR_BUILD_SPZ=ON requires zlib", "Required zlib")

    for setting in ("MELKOR_USE_METAL", "MELKOR_USE_CUDA"):
        result, _ = configure(
            cmake,
            source_dir,
            "MELKOR_BUILD_SPZ=OFF",
            f"{setting}=OFF",
        )
        require_failure(
            result,
            f"{setting} was removed",
            f"Retired option {setting}",
        )

    print("cmake feature settings: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
