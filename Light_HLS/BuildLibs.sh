#!/usr/bin/env bash

set -euo pipefail

script_directory="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
library_directory="${script_directory}/Tests/LLVM_Learner_Libs"
build_directory="${library_directory}/build"
build_jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-8}"
cmake_arguments=()

# All remaining arguments are forwarded to CMake, preserving no-argument use.
while [[ $# -gt 0 ]]; do
    case "$1" in
        --parallel|-j)
            if [[ $# -lt 2 ]]; then
                echo "$1 requires a positive job count" >&2
                exit 2
            fi
            build_jobs="$2"
            shift 2
            ;;
        *)
            cmake_arguments+=("$1")
            shift
            ;;
    esac
done

if [[ ! "${build_jobs}" =~ ^[1-9][0-9]*$ ]]; then
    echo "Build job count must be a positive integer: ${build_jobs}" >&2
    exit 2
fi

cmake -S "${library_directory}" -B "${build_directory}" "${cmake_arguments[@]}"
cmake --build "${build_directory}" --parallel "${build_jobs}"
while IFS= read -r -d '' archive; do
    cp -f -- "${archive}" "${library_directory}/$(basename "${archive}")"
done < <(find "${build_directory}" -type f -name '*.a' -print0)
