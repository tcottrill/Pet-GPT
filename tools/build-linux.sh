#!/usr/bin/env bash
set -euo pipefail
source_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-$source_root/build/linux}"
build_type="${PET_BUILD_TYPE:-Release}"
# The Linux binary stays in the build tree (dist/ holds the Windows exe).
configure_args=(-S "$source_root" -B "$build_dir" "-DCMAKE_BUILD_TYPE=$build_type" -DBUILD_TESTING=ON
    "-DPETGPT_OUTPUT_DIR=$build_dir/bin")
if [[ -n "${PET_WX_SOURCE:-}" ]]; then
    configure_args+=("-DPET_WX_SOURCE=$PET_WX_SOURCE")
fi
cmake "${configure_args[@]}"
cmake --build "$build_dir" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
ctest --test-dir "$build_dir" --output-on-failure
printf 'Executable: %s/bin/pet-gpt\n' "$build_dir"
