#!/usr/bin/env bash
# Bundle latest + recent historical firmwares into docs/firmware for the WebFlasher.
# Historical bins must be same-origin: browser fetch of GitHub release assets is
# blocked by CORS (TypeError: Failed to fetch).
set -euo pipefail

BOARDS="${BOARDS:-tencoder-pro waveshare-knob-18}"
REPO="${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is required}"
ARCHIVE_COUNT="${ARCHIVE_COUNT:-12}"
DOCS_FW="${DOCS_FW:-docs/firmware}"
RELEASE_DIR="${RELEASE_DIR:-}" # optional local dir with already-built latest assets

mkdir -p "${DOCS_FW}/archive"

if [[ -n "${RELEASE_NAME:-}" && -n "${RELEASE_TAG:-}" ]]; then
  :
else
  RELEASE_JSON="$(curl -fsSL "https://api.github.com/repos/${REPO}/releases/latest")"
  RELEASE_NAME="$(echo "${RELEASE_JSON}" | jq -r .name)"
  RELEASE_TAG="$(echo "${RELEASE_JSON}" | jq -r .tag_name)"
fi

BOARD_JSON='{}'

for board in ${BOARDS}; do
  merged="FlightScnr-${board}-merged.bin"
  app="FlightScnr-${board}-app.bin"

  if [[ -n "${RELEASE_DIR}" ]]; then
    test -f "${RELEASE_DIR}/${merged}"
    test -f "${RELEASE_DIR}/${app}"
    cp "${RELEASE_DIR}/${merged}" "${DOCS_FW}/${merged}"
    cp "${RELEASE_DIR}/${app}" "${DOCS_FW}/${app}"
  else
    merged_url="https://github.com/${REPO}/releases/latest/download/${merged}"
    app_url="https://github.com/${REPO}/releases/latest/download/${app}"
    if ! curl -fsSL -L -o "${DOCS_FW}/${merged}.tmp" "${merged_url}" ||
       ! curl -fsSL -L -o "${DOCS_FW}/${app}.tmp" "${app_url}"; then
      rm -f "${DOCS_FW}/${merged}.tmp" "${DOCS_FW}/${app}.tmp"
      echo "Latest release has no complete ${board} pair; skipping it."
      continue
    fi
    mv "${DOCS_FW}/${merged}.tmp" "${DOCS_FW}/${merged}"
    mv "${DOCS_FW}/${app}.tmp" "${DOCS_FW}/${app}"
  fi

  merged_size="$(stat -c%s "${DOCS_FW}/${merged}")"
  app_size="$(stat -c%s "${DOCS_FW}/${app}")"
  BOARD_JSON="$(jq -c \
    --arg board "${board}" \
    --arg merged "${merged}" \
    --arg app "${app}" \
    --argjson merged_size "${merged_size}" \
    --argjson app_size "${app_size}" \
    '. + {($board): {
      name: $board,
      chipFamily: "ESP32-S3",
      parts: [
        {path: $merged, offset: 0, role: "full", size: $merged_size},
        {path: $app, offset: 65536, role: "app", size: $app_size}
      ]
    }}' <<<"${BOARD_JSON}")"
  echo "Bundled ${board}: merged ${merged_size} bytes, app ${app_size} bytes"
done

if [[ "$(jq 'has("tencoder-pro")' <<<"${BOARD_JSON}")" != "true" ]]; then
  echo "Required tencoder-pro firmware is unavailable." >&2
  exit 1
fi

# `builds` is retained for compatibility with a previously deployed flasher.js.
# The new UI reads the board-keyed `boards` map.
jq -n \
  --arg name "${RELEASE_NAME}" \
  --arg version "${RELEASE_TAG}" \
  --argjson boards "${BOARD_JSON}" \
  '{
    name: $name,
    version: $version,
    boards: $boards,
    builds: (
      [$boards["tencoder-pro"] + {board: "tencoder-pro"}] +
      (if $boards["waveshare-knob-18"] then
         [$boards["waveshare-knob-18"] + {board: "waveshare-knob-18"}]
       else [] end)
    ),
    size: $boards["tencoder-pro"].parts[0].size
  }' > "${DOCS_FW}/manifest.json"

# Mirror older full images for the version dropdown (same-origin, CORS-safe).
RELEASES_JSON="$(curl -fsSL "https://api.github.com/repos/${REPO}/releases?per_page=30")"
ARCHIVE_JSON='[]'
COUNT=0

while IFS= read -r row; do
  tag="$(jq -r .tag <<<"${row}")"
  published="$(jq -r .published <<<"${row}")"
  if [[ -z "${tag}" || "${tag}" == "null" || "${tag}" == "${RELEASE_TAG}" ]]; then
    continue
  fi
  if [[ ${COUNT} -ge ${ARCHIVE_COUNT} ]]; then
    break
  fi

  release_boards='{}'
  dest_dir="${DOCS_FW}/archive/${tag}"
  mkdir -p "${dest_dir}"

  for board in ${BOARDS}; do
    asset="FlightScnr-${board}-merged.bin"
    asset_url="$(jq -r --arg asset "${asset}" \
      '[.assets[]? | select(.name == $asset) | .browser_download_url][0] // ""' <<<"${row}")"
    if [[ -z "${asset_url}" || "${asset_url}" == "null" ]]; then
      continue
    fi

    echo "Archiving ${tag} ${board}…"
    curl -fsSL -L -o "${dest_dir}/${asset}" "${asset_url}"
    size="$(stat -c%s "${dest_dir}/${asset}")"
    rel_path="archive/${tag}/${asset}"
    release_boards="$(jq -c \
      --arg board "${board}" \
      --arg path "${rel_path}" \
      --argjson size "${size}" \
      '. + {($board): {merged: $path, merged_size: $size}}' <<<"${release_boards}")"
  done

  if [[ "$(jq 'length' <<<"${release_boards}")" -eq 0 ]]; then
    rmdir "${dest_dir}" 2>/dev/null || true
    continue
  fi

  # Keep legacy top-level fields for old T-Encoder-only UI compatibility.
  t_path="$(jq -r '."tencoder-pro".merged // ""' <<<"${release_boards}")"
  t_size="$(jq -r '."tencoder-pro".merged_size // 0' <<<"${release_boards}")"
  ARCHIVE_JSON="$(jq -c \
    --arg version "${tag}" \
    --arg published "${published}" \
    --argjson boards "${release_boards}" \
    --arg merged "${t_path}" \
    --argjson merged_size "${t_size}" \
    '. + [{
      version: $version,
      published_at: $published,
      boards: $boards,
      merged: $merged,
      merged_size: $merged_size
    }]' <<<"${ARCHIVE_JSON}")"
  COUNT=$((COUNT + 1))
done < <(jq -c '
  .[] |
  select(.draft == false and .prerelease == false) |
  {tag: .tag_name, published: (.published_at // .created_at // ""), assets: .assets}
' <<<"${RELEASES_JSON}")

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
