#!/usr/bin/env bash
# Install the CUDA toolkit the release lanes and the local AppImage build compile against, using
# NVIDIA's redistributable archives instead of the monolithic runfile installer.
#
#   scripts/install-cuda-redist.sh [destination] [compiler-version] [runtime-version]
#
# destination defaults to build/cuda, which is exactly where scripts/linux_build.sh looks for
# nvcc (detect_nvcc_path) and the path it hands cmake as CMAKE_CUDA_COMPILER, so installing
# there makes the deps step skip its own CUDA install and keeps both routes identical.
#
# Why the redistributables and not cuda_<version>_<driver>_linux.run:
#   * the runfile unpacks through an installer that links libxml2.so.2, a SONAME recent
#     distributions no longer ship (fedora:44), so --silent fails there;
#   * it writes the ~4 GB runfile plus the ~4 GB toolkit tree inside the workspace;
#   * the component archives are what packaging/linux/copr/Sunshine.spec has been building the
#     Fedora packages against already (see install_cuda_from_redistributables there), they need
#     no installer at all, and they are a few hundred MB.
#
# nvcc alone is not enough: cmake's CUDA language also needs the libcu++/CRT headers (cuda_cccl,
# cuda_crt) and the libnvvm + libnvptxcompiler libraries nvcc invokes for device code, which is
# why the component list below matches the spec's rather than "just nvcc".
set -euo pipefail

readonly redist_base="https://developer.download.nvidia.com/compute/cuda/redist"
# the versions scripts/linux_build.sh and the Fedora spec build against for CUDA 13.x
readonly default_compiler_version="13.1.115"
readonly default_runtime_version="13.1.80"

destination="${1:-build/cuda}"
compiler_version="${2:-${default_compiler_version}}"
runtime_version="${3:-${default_runtime_version}}"

case "$(uname -m)" in
  x86_64 | amd64)
    redist_arch="linux-x86_64"
    target_arch="x86_64-linux"
    ;;
  aarch64 | arm64)
    redist_arch="linux-sbsa"
    target_arch="sbsa-linux"
    ;;
  *)
    echo "unsupported architecture: $(uname -m)" >&2
    exit 1
    ;;
esac

target_dir="${destination}/targets/${target_arch}"

if [[ -x "${destination}/bin/nvcc" ]]; then
  echo "cuda already installed in ${destination}"
  "${destination}/bin/nvcc" --version | tail -2
  exit 0
fi

# component:version:destination ("root" = toolkit root, "target" = targets/<arch>)
readonly components=(
  "cuda_nvcc:${compiler_version}:root"
  "libnvvm:${compiler_version}:root"
  "cuda_cccl:${compiler_version}:target"
  "cuda_crt:${compiler_version}:target"
  "cuda_cudart:${runtime_version}:target"
  "cuda_culibos:${compiler_version}:target"
  "libnvptxcompiler:${compiler_version}:target"
)

if ! command -v wget >/dev/null 2>&1; then
  echo "wget is required to fetch the cuda components" >&2
  exit 1
fi

mkdir -p "${destination}" "${target_dir}"
work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

for component in "${components[@]}"; do
  IFS=: read -r name version destination_kind <<< "${component}"
  archive="${name}-${redist_arch}-${version}-archive.tar.xz"
  url="${redist_base}/${name}/${redist_arch}/${archive}"
  extract_dir="${target_dir}"
  if [[ "${destination_kind}" == "root" ]]; then
    extract_dir="${destination}"
  fi

  echo "cuda component: ${name} ${version} -> ${extract_dir}"
  wget --quiet --show-progress --tries=3 --retry-connrefused -O "${work_dir}/${archive}" "${url}"
  tar -xJf "${work_dir}/${archive}" --directory="${extract_dir}" --strip-components=1
  rm -f "${work_dir}/${archive}"
done

# nvcc expects this header in its target-specific include directory, not in the toolkit root
if [[ -f "${destination}/include/fatbinary_section.h" ]]; then
  mv "${destination}/include/fatbinary_section.h" "${target_dir}/include/"
fi

"${destination}/bin/nvcc" --version | tail -2
echo "cuda toolkit installed in ${destination}"
