#!/usr/bin/env bash
#
# Fetches the Linux FFmpeg package that FfmpegBuilder publishes as a release asset and prints the
# directory that CORAL_FFMPEG_DIR has to point at. The package is a CI product, so no shared library
# or header is checked into CoralMedia.
#
# Usage:
#   ./scripts/fetch-ffmpeg-linux.sh
#   CORAL_FFMPEG_DIR=/opt/ffmpeg ./scripts/fetch-ffmpeg-linux.sh   # already installed
#   FFMPEG_PACKAGE_URL=file:///tmp/ffmpeg-linux.tar.gz ./scripts/fetch-ffmpeg-linux.sh
#
# Environment:
#   FFMPEG_BUILDER_REPO   GitHub repository that publishes the assets
#                         (default: CoralFrameworks/FfmpegBuilder)
#   FFMPEG_VERSION        FFmpeg release to fetch (default: 9.1)
#   FFMPEG_DEST           download/extract directory (default: <repo>/.cache/ffmpeg)
#   FFMPEG_PACKAGE_URL    override for the archive URL, e.g. a CI artifact or a local file
#   FFMPEG_SKIP_CHECKSUM  set to 1 to skip the .sha256 check
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="${FFMPEG_BUILDER_REPO:-CoralFrameworks/FfmpegBuilder}"
VERSION="${FFMPEG_VERSION:-9.1}"
DEST="${FFMPEG_DEST:-${ROOT_DIR}/.cache/ffmpeg}"
ARCHIVE="ffmpeg-${VERSION}-linux-x86_64.tar.gz"
PACKAGE_DIR="${DEST}/ffmpeg-${VERSION}-linux-x86_64"

if [[ -n "${CORAL_FFMPEG_DIR:-}" && -f "${CORAL_FFMPEG_DIR}/cmake/FFmpegConfig.cmake" ]]; then
  echo "${CORAL_FFMPEG_DIR}"
  exit 0
fi

mkdir -p "${DEST}"

if [[ ! -f "${PACKAGE_DIR}/cmake/FFmpegConfig.cmake" ]]; then
  URL="${FFMPEG_PACKAGE_URL:-https://github.com/${REPO}/releases/latest/download/${ARCHIVE}}"
  echo "==> downloading ${URL}" >&2
  curl -fL --retry 3 --retry-delay 2 -o "${DEST}/${ARCHIVE}" "${URL}"

  if [[ "${FFMPEG_SKIP_CHECKSUM:-0}" != "1" && -z "${FFMPEG_PACKAGE_URL:-}" ]]; then
    if curl -fsSL --retry 3 -o "${DEST}/${ARCHIVE}.sha256" \
        "${URL}.sha256"; then
      (cd "${DEST}" && sha256sum -c "${ARCHIVE}.sha256")
    else
      echo "==> no .sha256 next to the asset, continuing without it" >&2
    fi
  fi

  rm -rf "${PACKAGE_DIR}"
  tar -xzf "${DEST}/${ARCHIVE}" -C "${DEST}"
  if [[ ! -f "${PACKAGE_DIR}/cmake/FFmpegConfig.cmake" ]]; then
    echo "error: ${PACKAGE_DIR} does not contain cmake/FFmpegConfig.cmake" >&2
    exit 1
  fi
fi

echo "==> FFmpeg Linux package: ${PACKAGE_DIR}" >&2
echo "${PACKAGE_DIR}"
