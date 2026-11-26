#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"

VARIANT="all"
BUILD_DIR="${REPO_ROOT}/build/linux-release"
MCP_BUILD_DIR="${REPO_ROOT}/build/linux-release-mcp"
OUT_DIR="${REPO_ROOT}/build/release-artifacts"
ENABLE_APPIMAGE=0
APP_VERSION=""

usage() {
  cat <<'EOF'
Usage: build_release.sh [options]

Options:
  --variant <standard|mcp|all>   Release variant to package (default: all)
  --build-dir <path>             Standard CMake build directory (default: build/linux-release)
  --mcp-build-dir <path>         MCP CMake build directory (default: build/linux-release-mcp)
  --out-dir <path>               Output directory for packaged artifacts (default: build/release-artifacts)
  --appimage                     Also bundle AppImage packages via linuxdeploy
  --no-appimage                  Skip AppImage creation (default)
  --version <X.Y.Z>              Override version parsed from CMakeLists.txt
  -h, --help                     Show this help message
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --variant)
      VARIANT="${2:?Missing argument for --variant}"
      shift 2
      ;;
    --build-dir)
      BUILD_DIR="${2:?Missing argument for --build-dir}"
      shift 2
      ;;
    --mcp-build-dir)
      MCP_BUILD_DIR="${2:?Missing argument for --mcp-build-dir}"
      shift 2
      ;;
    --out-dir)
      OUT_DIR="${2:?Missing argument for --out-dir}"
      shift 2
      ;;
    --appimage)
      ENABLE_APPIMAGE=1
      shift
      ;;
    --no-appimage)
      ENABLE_APPIMAGE=0
      shift
      ;;
    --version)
      APP_VERSION="${2:?Missing argument for --version}"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

case "${VARIANT}" in
  standard|mcp|all) ;;
  *)
    echo "Invalid --variant '${VARIANT}'. Expected standard, mcp, or all." >&2
    exit 1
    ;;
esac

if [[ -z "${APP_VERSION}" ]]; then
  APP_VERSION="$(sed -nE 's/^[[:space:]]*project\([[:space:]]*IotaStudio[[:space:]]+VERSION[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "${REPO_ROOT}/CMakeLists.txt" | head -n1)"
fi
if [[ -z "${APP_VERSION}" ]]; then
  echo "Failed to parse IotaStudio VERSION from ${REPO_ROOT}/CMakeLists.txt" >&2
  exit 1
fi

ARCH="$(uname -m)"
mkdir -p "${OUT_DIR}"
OUT_DIR="$(cd -- "${OUT_DIR}" && pwd)"

WORK_TMP="$(mktemp -d)"
cleanup() {
  rm -rf "${WORK_TMP}"
}
trap cleanup EXIT

ensure_linuxdeploy() {
  if [[ -n "${LINUXDEPLOY:-}" && -x "${LINUXDEPLOY}" ]]; then
    echo "${LINUXDEPLOY}"
    return 0
  fi
  if command -v linuxdeploy >/dev/null 2>&1; then
    command -v linuxdeploy
    return 0
  fi
  local cached="${WORK_TMP}/linuxdeploy-${ARCH}.AppImage"
  local url="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-${ARCH}.AppImage"
  echo "Downloading linuxdeploy from ${url}..." >&2
  curl -fsSL "${url}" -o "${cached}"
  chmod +x "${cached}"
  echo "${cached}"
}

package_edition() {
  local include_mcp="$1"
  local build_dir="$2"
  local prefix
  local edition_label

  if [[ "${include_mcp}" -eq 1 ]]; then
    prefix="IotaStudio-MCP-linux-${ARCH}-${APP_VERSION}"
    edition_label="MCP edition"
  else
    prefix="IotaStudio-linux-${ARCH}-${APP_VERSION}"
    edition_label="standard edition"
  fi

  if [[ ! -x "${build_dir}/apps/studio/iota_studio" ]]; then
    echo "Built executable not found at ${build_dir}/apps/studio/iota_studio" >&2
    exit 1
  fi
  if [[ "${include_mcp}" -eq 1 && ! -x "${build_dir}/apps/mcp/iota_mcp" ]]; then
    echo "Built MCP executable not found at ${build_dir}/apps/mcp/iota_mcp" >&2
    exit 1
  fi

  echo "Packaging Iota Studio v${APP_VERSION} (${edition_label}) from ${build_dir}..."

  # 1. Portable tar.gz archive
  local tar_stage="${WORK_TMP}/${prefix}-tar"
  local tar_root="${tar_stage}/${prefix}"
  mkdir -p "${tar_root}"
  cmake --install "${build_dir}" --prefix "${tar_root}"
  cp "${REPO_ROOT}/README.md" "${tar_root}/README.md"
  cp "${REPO_ROOT}/LICENSE" "${tar_root}/LICENSE"
  if [[ "${include_mcp}" -eq 1 ]]; then
    cp "${REPO_ROOT}/docs/MCP.md" "${tar_root}/MCP.md"
  fi

  local tar_path="${OUT_DIR}/${prefix}.tar.gz"
  tar -C "${tar_stage}" -czf "${tar_path}" "${prefix}"
  echo "Created tarball:"
  echo "  Path:   ${tar_path}"
  echo "  Size:   $(wc -c < "${tar_path}") bytes"
  echo "  SHA256: $(sha256sum "${tar_path}" | awk '{print $1}')"

  # 2. Optional AppImage via linuxdeploy
  if [[ "${ENABLE_APPIMAGE}" -eq 1 ]]; then
    local ld_bin
    ld_bin="$(ensure_linuxdeploy)"

    local appdir="${WORK_TMP}/${prefix}.AppDir"
    mkdir -p "${appdir}"
    DESTDIR="${appdir}" cmake --install "${build_dir}" --prefix /usr

    local icon_dir="${appdir}/usr/share/icons/hicolor/512x512/apps"
    mkdir -p "${icon_dir}"
    local icon_file="${icon_dir}/iota-studio.png"
    if command -v convert >/dev/null 2>&1; then
      convert "${REPO_ROOT}/assets/icon.png" -resize 512x512 "${icon_file}"
    elif command -v magick >/dev/null 2>&1; then
      magick "${REPO_ROOT}/assets/icon.png" -resize 512x512 "${icon_file}"
    else
      cp "${REPO_ROOT}/assets/icon.png" "${icon_file}"
    fi

    local apprun="${WORK_TMP}/AppRun-${prefix}.sh"
    if [[ "${include_mcp}" -eq 1 ]]; then
      cat > "${apprun}" <<'EOF'
#!/usr/bin/env bash
set -e
HERE="$(dirname "$(readlink -f "$0")")"
if [[ "${1:-}" == "--mcp" ]]; then
  shift
  exec "$HERE/usr/bin/iota_mcp" "$@"
fi
exec "$HERE/usr/bin/iota_studio" "$@"
EOF
    else
      cat > "${apprun}" <<'EOF'
#!/usr/bin/env bash
set -e
HERE="$(dirname "$(readlink -f "$0")")"
exec "$HERE/usr/bin/iota_studio" "$@"
EOF
    fi
    chmod +x "${apprun}"

    local appimage_path="${OUT_DIR}/${prefix}.AppImage"
    rm -f "${appimage_path}"

    local ld_args=(
      --appimage-extract-and-run
      --appdir "${appdir}"
      --executable "${appdir}/usr/bin/iota_studio"
      --desktop-file "${appdir}/usr/share/applications/io.github.vorlie.iota_studio.desktop"
      --icon-file "${icon_file}"
      --custom-apprun "${apprun}"
      --output appimage
    )
    if [[ "${include_mcp}" -eq 1 ]]; then
      ld_args+=(--executable "${appdir}/usr/bin/iota_mcp")
    fi

    ARCH="${ARCH}" NO_STRIP=1 OUTPUT="${appimage_path}" "${ld_bin}" "${ld_args[@]}"
    chmod +x "${appimage_path}"
    echo "Created AppImage:"
    echo "  Path:   ${appimage_path}"
    echo "  Size:   $(wc -c < "${appimage_path}") bytes"
    echo "  SHA256: $(sha256sum "${appimage_path}" | awk '{print $1}')"
  fi
}

if [[ "${VARIANT}" == "standard" || "${VARIANT}" == "all" ]]; then
  package_edition 0 "${BUILD_DIR}"
fi

if [[ "${VARIANT}" == "mcp" || "${VARIANT}" == "all" ]]; then
  package_edition 1 "${MCP_BUILD_DIR}"
fi
