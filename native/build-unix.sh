#!/usr/bin/env bash
set -euo pipefail

project_root="${1:?project root is required}"
output_root="${2:?output root is required}"
native_root="$project_root/native"

case "$(uname -s)" in
  Darwin) os_name="macos" ;;
  Linux) os_name="linux" ;;
  *) echo "Unsupported native build host: $(uname -s)" >&2; exit 1 ;;
esac

case "$(uname -m)" in
  arm64|aarch64) arch_name="aarch64" ;;
  x86_64|amd64) arch_name="x86_64" ;;
  *) echo "Unsupported native CPU: $(uname -m)" >&2; exit 1 ;;
esac

if [[ -z "${JAVA_HOME:-}" ]] && [[ "$os_name" == "macos" ]]; then
  export JAVA_HOME="$(/usr/libexec/java_home -v 25 2>/dev/null || /usr/libexec/java_home)"
fi

build_dir="$project_root/build/native/cmake-$os_name-$arch_name"
destination="$output_root/$os_name-$arch_name"
cmake -S "$native_root" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --config Release --parallel
mkdir -p "$destination"

if [[ "$os_name" == "macos" ]]; then
  cp "$build_dir/libmacfsrmc_fsr2.dylib" "$destination/libmacfsrmc_fsr2.dylib"
else
  cp "$build_dir/libmacfsrmc_fsr2.so" "$destination/libmacfsrmc_fsr2.so"
fi
