#!/usr/bin/env bash
# Bundle latest + recent historical firmwares into docs/firmware for the WebFlasher.
# Historical bins must be same-origin: browser fetch of GitHub release assets is
# blocked by CORS (TypeError: Failed to fetch).
set -euo pipefail

MERGED_ASSET="${MERGED_ASSET:-FlightScnr-tencoder-pro-merged.bin}"
APP_ASSET="${APP_ASSET:-FlightScnr-tencoder-pro-app.bin}"
REPO="${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is required}"
# How many older releases (excluding latest) to mirror for the version picker.
ARCHIVE_COUNT="${ARCHIVE_COUNT:-12}"
DOCS_FW="${DOCS_FW:-docs/firmware}"
RELEASE_DIR="${RELEASE_DIR:-}" # optional local dir with already-built latest assets

mkdir -p "${DOCS_FW}/archive"

if [[ -n "${RELEASE_DIR}" && -f "${RELEASE_DIR}/${MERGED_ASSET}" && -f "${RELEASE_DIR}/${APP_ASSET}" ]]; then
  cp "${RELEASE_DIR}/${MERGED_ASSET}" "${DOCS_FW}/${MERGED_ASSET}"
  cp "${RELEASE_DIR}/${APP_ASSET}" "${DOCS_FW}/${APP_ASSET}"
else
  for asset in "${MERGED_ASSET}" "${APP_ASSET}"; do
    URL="https://github.com/${REPO}/releases/latest/download/${asset}"
    curl -fsSL -L -o "${DOCS_FW}/${asset}" "${URL}"
  done
fi

MERGED_SIZE="$(stat -c%s "${DOCS_FW}/${MERGED_ASSET}")"
APP_SIZE="$(stat -c%s "${DOCS_FW}/${APP_ASSET}")"

if [[ -n "${RELEASE_NAME:-}" && -n "${RELEASE_TAG:-}" ]]; then
  :
else
  RELEASE_JSON="$(curl -fsSL "https://api.github.com/repos/${REPO}/releases/latest")"
  RELEASE_NAME="$(echo "${RELEASE_JSON}" | jq -r .name)"
  RELEASE_TAG="$(echo "${RELEASE_JSON}" | jq -r .tag_name)"
fi

jq -n \
  --arg name "${RELEASE_NAME}" \
  --arg version "${RELEASE_TAG}" \
  --arg merged "${MERGED_ASSET}" \
  --arg app "${APP_ASSET}" \
  --argjson merged_size "${MERGED_SIZE}" \
  --argjson app_size "${APP_SIZE}" \
  '{name: $name, version: $version, size: $merged_size, builds: [{chipFamily: "ESP32-S3", parts: [
    {path: $merged, offset: 0, role: "full", size: $merged_size},
    {path: $app, offset: 65536, role: "app", size: $app_size}
  ]}]}' \
  > "${DOCS_FW}/manifest.json"

echo "Bundled latest ${RELEASE_NAME} (${RELEASE_TAG}): merged ${MERGED_SIZE} bytes, app ${APP_SIZE} bytes"

# Mirror older full images for the version dropdown (same-origin, CORS-safe).
RELEASES_JSON="$(curl -fsSL "https://api.github.com/repos/${REPO}/releases?per_page=30")"
ARCHIVE_JSON='[]'
COUNT=0

while IFS= read -r row; do
  tag="$(echo "${row}" | jq -r .tag)"
  published="$(echo "${row}" | jq -r .published)"
  asset_url="$(echo "${row}" | jq -r .asset_url)"
  if [[ -z "${tag}" || "${tag}" == "null" || "${tag}" == "${RELEASE_TAG}" ]]; then
    continue
  fi
  if [[ ${COUNT} -ge ${ARCHIVE_COUNT} ]]; then
    break
  fi
  dest_dir="${DOCS_FW}/archive/${tag}"
  mkdir -p "${dest_dir}"
  echo "Archiving ${tag}…"
  curl -fsSL -L -o "${dest_dir}/${MERGED_ASSET}" "${asset_url}"
  size="$(stat -c%s "${dest_dir}/${MERGED_ASSET}")"
  ARCHIVE_JSON="$(jq -c \
    --arg version "${tag}" \
    --arg published "${published}" \
    --arg path "archive/${tag}/${MERGED_ASSET}" \
    --argjson size "${size}" \
    '. + [{version: $version, published_at: $published, merged: $path, merged_size: $size}]' \
    <<<"${ARCHIVE_JSON}")"
  COUNT=$((COUNT + 1))
done < <(echo "${RELEASES_JSON}" | jq -c '
  [.[] | select(.draft == false and .prerelease == false) | {
    tag: .tag_name,
    published: (.published_at // .created_at // ""),
    asset_url: (
      (.assets[] | select(.name == "FlightScnr-tencoder-pro-merged.bin") | .browser_download_url)
      // empty
    )
  } | select(.asset_url != null and .asset_url != "")]
  | .[]
')

jq -n \
  --argjson releases "${ARCHIVE_JSON}" \
  --arg latest "${RELEASE_TAG}" \
  '{latest: $latest, releases: $releases}' \
  > "${DOCS_FW}/archive-index.json"

echo "Archived ${COUNT} historical release(s) for WebFlasher"
ls -la "${DOCS_FW}"
ls -la "${DOCS_FW}/archive" || true
cat "${DOCS_FW}/manifest.json"
cat "${DOCS_FW}/archive-index.json"
