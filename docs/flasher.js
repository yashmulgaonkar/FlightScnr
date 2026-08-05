import { ESPLoader, Transport } from "./vendor/esptool-js.bundle.js";

const BOARD_AUTO = "auto";
const BOARD_TENCODER = "tencoder-pro";
const BOARD_WAVESHARE = "waveshare-knob-18";
const BOARD_LABELS = {
  [BOARD_TENCODER]: "LilyGO T-Encoder Pro",
  [BOARD_WAVESHARE]: "Waveshare Knob Touch LCD 1.8",
};
const BOARD_STORAGE_KEY = "flightscnr-webflasher-board";
const FULL_FLASH_OFFSET = 0;
const APP_FLASH_OFFSET = 0x10000;
const FIRMWARE_BASE = "./firmware";
const MANIFEST_URL = `${FIRMWARE_BASE}/manifest.json`;
const ARCHIVE_INDEX_URL = `${FIRMWARE_BASE}/archive-index.json`;
const BUNDLED_RELEASE_ID = "__bundled_latest__";

const els = {
  connectBtn: document.getElementById("connect-btn"),
  disconnectBtn: document.getElementById("disconnect-btn"),
  flashLatestBtn: document.getElementById("flash-latest-btn"),
  eraseBtn: document.getElementById("erase-btn"),
  eraseDialog: document.getElementById("erase-dialog"),
  eraseCancelBtn: document.getElementById("erase-cancel-btn"),
  eraseConfirmBtn: document.getElementById("erase-confirm-btn"),
  installModeFull: document.getElementById("install-mode-full"),
  installModeApp: document.getElementById("install-mode-app"),
  installModeAppLabel: document.getElementById("install-mode-app-label"),
  boardSelect: document.getElementById("board-select"),
  boardStatus: document.getElementById("board-status"),
  releaseSelect: document.getElementById("release-select"),
  status: document.getElementById("status"),
  releaseMeta: document.getElementById("release-meta"),
  releaseHelp: document.getElementById("release-help"),
  progressWrap: document.getElementById("progress-wrap"),
  progress: document.getElementById("progress"),
  progressLabel: document.getElementById("progress-label"),
  log: document.getElementById("log"),
};

let port = null;
let transport = null;
let esploader = null;
let busy = false;
/** True after chip erase in this session - app-only install is invalid until full install. */
let chipErased = false;
let bundledManifestPromise = null;
let releaseChoices = [];
let releaseLoadWarning = "";
let detectedBoard = null;
let detectedPortInfo = null;

function log(line) {
  const ts = new Date().toLocaleTimeString();
  els.log.textContent += `[${ts}] ${line}\n`;
  els.log.scrollTop = els.log.scrollHeight;
}

function setStatus(text) {
  els.status.textContent = text;
}

function formatSizeMb(bytes) {
  return bytes ? `${(bytes / (1024 * 1024)).toFixed(2)} MB` : "? MB";
}

function formatPublishedDate(iso) {
  if (!iso) {
    return "";
  }
  const date = new Date(iso);
  if (Number.isNaN(date.getTime())) {
    return "";
  }
  return date.toLocaleDateString(undefined, {
    year: "numeric",
    month: "short",
    day: "numeric",
  });
}

function selectedRelease() {
  return (
    releaseChoices.find((release) => release.id === els.releaseSelect.value) ??
    releaseChoices[0] ??
    null
  );
}

function resolvedBoard() {
  return els.boardSelect.value === BOARD_AUTO ? detectedBoard : els.boardSelect.value;
}

function selectedBoardParts(release = selectedRelease()) {
  const board = resolvedBoard();
  return board && release?.boards ? release.boards[board] ?? null : null;
}

function releaseAllowsAppInstall(release = selectedRelease()) {
  const parts = selectedBoardParts(release);
  return Boolean(parts?.allowAppOnly && parts?.appPart);
}

function formatUsbId(value) {
  return typeof value === "number"
    ? `0x${value.toString(16).toUpperCase().padStart(4, "0")}`
    : "unknown";
}

function detectBoardFromPortInfo(info) {
  if (info?.usbVendorId === 0x1a86) {
    return BOARD_WAVESHARE;
  }
  if (info?.usbVendorId === 0x303a) {
    return BOARD_TENCODER;
  }
  return null;
}

function updateBoardStatus() {
  const selected = els.boardSelect.value;
  if (selected !== BOARD_AUTO) {
    els.boardStatus.textContent = `Manual override: ${BOARD_LABELS[selected]}.`;
    return;
  }
  if (detectedBoard) {
    const vid = formatUsbId(detectedPortInfo?.usbVendorId);
    const pid = formatUsbId(detectedPortInfo?.usbProductId);
    els.boardStatus.textContent =
      `Auto detected: ${BOARD_LABELS[detectedBoard]} (USB ${vid}:${pid}). Confirm before flashing.`;
    return;
  }
  if (port) {
    els.boardStatus.textContent =
      "Auto could not identify this USB port. Select the board manually before Install.";
    return;
  }
  els.boardStatus.textContent =
    "Auto: connect a board to detect it. You can always override this selection.";
}

function updateReleaseMeta() {
  const release = selectedRelease();
  const board = resolvedBoard();
  const boardParts = selectedBoardParts(release);
  if (!release) {
    els.releaseMeta.textContent = "Loading firmware releases...";
    els.releaseHelp.textContent = "";
    return;
  }

  const isLatest = release.source === "bundled" || release.source === "latest";
  const prefix = isLatest ? "Latest" : "Selected";
  const details = [
    release.name || release.version,
    formatPublishedDate(release.publishedAt),
    board ? BOARD_LABELS[board] : "board not resolved",
    boardParts?.fullPart
      ? `${formatSizeMb(boardParts.fullPart.size)} full image`
      : null,
  ].filter(Boolean);
  els.releaseMeta.textContent = `${prefix}: ${details.join(" | ")}`;

  const notes = [];
  if (chipErased) {
    notes.push("Chip erase in this session requires Full install.");
  }
  if (isLatest && boardParts?.appPart) {
    notes.push("Latest release supports both Full install and Update app only.");
  } else if (isLatest && boardParts?.fullPart) {
    notes.push("This board only has a Full installer in the selected release.");
  } else if (!isLatest && boardParts?.fullPart) {
    notes.push("Historical releases use Full install only for safety.");
  } else {
    notes.push("Choose another board or firmware version.");
  }
  if (releaseLoadWarning) {
    notes.push(releaseLoadWarning);
  }
  if (!board) {
    notes.push("Connect for Auto detection or choose a board manually.");
  } else if (!boardParts?.fullPart) {
    notes.push(`This release has no ${BOARD_LABELS[board]} firmware.`);
  }
  els.releaseHelp.textContent = notes.join(" ");
}

function getInstallMode() {
  if (chipErased || !releaseAllowsAppInstall()) {
    return "full";
  }
  return els.installModeApp.checked ? "app" : "full";
}

function updateInstallModeUI() {
  const parts = selectedBoardParts();
  const boardReady = Boolean(resolvedBoard() && parts?.fullPart);
  const appUnavailable = chipErased || !releaseAllowsAppInstall();
  els.installModeFull.disabled = busy;
  els.installModeApp.disabled = busy || appUnavailable;
  els.installModeAppLabel.classList.toggle(
    "disabled",
    busy || appUnavailable,
  );
  // Only fall back to Full when app-only is genuinely unavailable; a busy flash
  // must keep showing the mode the user picked.
  if (appUnavailable) {
    els.installModeFull.checked = true;
  }
  els.flashLatestBtn.disabled = busy || port === null || !boardReady;
  updateBoardStatus();
  updateReleaseMeta();
}

function setBusy(value) {
  busy = value;
  els.connectBtn.disabled = value || port !== null;
  els.disconnectBtn.disabled = value || port === null;
  els.flashLatestBtn.disabled = value || port === null || !selectedBoardParts()?.fullPart;
  els.eraseBtn.disabled = value || port === null;
  els.releaseSelect.disabled = value || releaseChoices.length <= 1;
  els.boardSelect.disabled = value;
  if (value) {
    setStatus("Working…");
    els.status.className = "";
  } else if (port) {
    setStatus("Connected");
    els.status.className = "ok";
  } else {
    setStatus("Not connected");
    els.status.className = "";
  }
  updateInstallModeUI();
}

function setProgress(pct, label) {
  els.progressWrap.classList.add("active");
  els.progress.value = pct;
  els.progressLabel.textContent = label;
}

function clearProgress() {
  els.progressWrap.classList.remove("active");
  els.progress.value = 0;
  els.progressLabel.textContent = "";
}

function manifestPart(build, mode) {
  const parts = build?.parts ?? [];
  if (mode === "app") {
    return (
      parts.find((part) => part.role === "app") ??
      parts.find((part) => /app/i.test(part.path ?? "")) ??
      null
    );
  }
  return (
    parts.find((part) => part.role === "full") ??
    parts.find((part) => /merged/i.test(part.path ?? "")) ??
    parts[0] ??
    null
  );
}

async function loadFirmwareManifest() {
  if (bundledManifestPromise === null) {
    bundledManifestPromise = (async () => {
      const resp = await fetch(MANIFEST_URL, { cache: "no-store" });
      if (!resp.ok) {
        throw new Error(`Manifest unavailable (HTTP ${resp.status})`);
      }
      return resp.json();
    })();
  }
  return bundledManifestPromise;
}

function buildBundledRelease(manifest) {
  const manifestBoards = manifest.boards ?? {};
  // Backward compatibility with the old single-board manifest.
  if (!manifestBoards[BOARD_TENCODER] && manifest.builds?.[0]) {
    manifestBoards[BOARD_TENCODER] = manifest.builds[0];
  }
  const boards = {};
  for (const board of [BOARD_TENCODER, BOARD_WAVESHARE]) {
    const build =
      manifestBoards[board] ??
      manifest.builds?.find((candidate) => candidate.board === board);
    const fullPart = manifestPart(build, "full");
    const appPart = manifestPart(build, "app");
    if (!fullPart?.path) {
      continue;
    }
    boards[board] = {
      allowAppOnly: Boolean(appPart?.path),
      fullPart: {
        url: `${FIRMWARE_BASE}/${fullPart.path}`,
        offset: fullPart.offset ?? FULL_FLASH_OFFSET,
        size: fullPart.size ?? manifest.size ?? null,
        label: fullPart.path,
      },
      appPart: appPart?.path
        ? {
            url: `${FIRMWARE_BASE}/${appPart.path}`,
            offset: appPart.offset ?? APP_FLASH_OFFSET,
            size: appPart.size ?? null,
            label: appPart.path,
          }
        : null,
    };
  }
  return {
    id: BUNDLED_RELEASE_ID,
    source: "bundled",
    name: manifest.name || manifest.version || "Latest release",
    version: manifest.version || "",
    publishedAt: null,
    boards,
  };
}

/** Historical builds must be same-origin; GitHub release CDN blocks browser CORS. */
function buildArchivedRelease(entry) {
  if (!entry?.version) {
    return null;
  }
  const archivedBoards = entry.boards ?? {};
  // Backward compatibility with old archive-index.json.
  if (!archivedBoards[BOARD_TENCODER] && entry.merged) {
    archivedBoards[BOARD_TENCODER] = {
      merged: entry.merged,
      merged_size: entry.merged_size,
    };
  }
  const boards = {};
  for (const board of [BOARD_TENCODER, BOARD_WAVESHARE]) {
    const archived = archivedBoards[board];
    if (!archived?.merged) {
      continue;
    }
    boards[board] = {
      allowAppOnly: false,
      fullPart: {
        url: `${FIRMWARE_BASE}/${archived.merged}`,
        offset: FULL_FLASH_OFFSET,
        size: archived.merged_size ?? null,
        label: `FlightScnr-${board}-merged.bin`,
      },
      appPart: null,
    };
  }
  if (Object.keys(boards).length === 0) {
    return null;
  }
  return {
    id: `archive:${entry.version}`,
    source: "archive",
    name: entry.version,
    version: entry.version,
    publishedAt: entry.published_at || null,
    boards,
  };
}

async function loadArchiveIndex() {
  const resp = await fetch(ARCHIVE_INDEX_URL, { cache: "no-store" });
  if (!resp.ok) {
    throw new Error(`Archive index unavailable (HTTP ${resp.status})`);
  }
  const index = await resp.json();
  const releases = Array.isArray(index?.releases) ? index.releases : [];
  return releases.map(buildArchivedRelease).filter(Boolean);
}

function populateReleaseSelect() {
  const previous = els.releaseSelect.value;
  els.releaseSelect.innerHTML = "";
  for (const release of releaseChoices) {
    const option = document.createElement("option");
    option.value = release.id;
    if (release.source === "bundled" || release.source === "latest") {
      option.textContent = `Latest (${release.version || "current"})`;
    } else {
      const published = formatPublishedDate(release.publishedAt);
      option.textContent = published
        ? `${release.version} - ${published}`
        : release.version || release.name;
    }
    els.releaseSelect.appendChild(option);
  }
  const defaultId = releaseChoices[0]?.id ?? BUNDLED_RELEASE_ID;
  const selected = releaseChoices.some((release) => release.id === previous)
    ? previous
    : defaultId;
  els.releaseSelect.value = selected;
  els.releaseSelect.disabled = busy || releaseChoices.length <= 1;
  updateInstallModeUI();
}

async function loadReleaseOptions() {
  let bundled = null;
  let archived = [];

  try {
    const manifest = await loadFirmwareManifest();
    bundled = buildBundledRelease(manifest);
  } catch (err) {
    console.warn("Bundled manifest unavailable:", err);
  }

  try {
    archived = await loadArchiveIndex();
    releaseLoadWarning = "";
  } catch (err) {
    releaseLoadWarning =
      "Older releases are unavailable until the WebFlasher site is redeployed.";
    console.warn("Firmware archive index unavailable:", err);
  }

  const choices = [];
  if (bundled) {
    choices.push(bundled);
  }
  for (const release of archived) {
    if (bundled && release.version === bundled.version) {
      continue;
    }
    choices.push(release);
  }

  if (choices.length === 0) {
    releaseChoices = [];
    els.releaseSelect.innerHTML = "<option>No releases available</option>";
    els.releaseSelect.disabled = true;
    els.releaseMeta.textContent =
      "No firmware available (run Release workflow, then redeploy Pages).";
    els.releaseHelp.textContent = "";
    return;
  }

  releaseChoices = choices;
  populateReleaseSelect();
}

async function fetchFirmwareForInstall(mode) {
  const release = selectedRelease();
  const board = resolvedBoard();
  if (!release) {
    throw new Error("No firmware release is selected");
  }
  if (!board) {
    throw new Error("Auto could not identify the board; select it manually");
  }
  const boardParts = selectedBoardParts(release);
  const part = mode === "app" ? boardParts?.appPart : boardParts?.fullPart;
  if (!part?.url) {
    throw new Error(
      `Selected release does not have the requested ${BOARD_LABELS[board]} installer`,
    );
  }

  const releaseLabel = release.version || release.name || part.label;
  log(
    `Downloading ${releaseLabel} for ${BOARD_LABELS[board]} (${mode === "app" ? "app-only" : "full install"})…`,
  );
  const resp = await fetch(part.url, { cache: "no-store" });
  if (!resp.ok) {
    throw new Error(`Download failed (HTTP ${resp.status})`);
  }
  const buf = await resp.arrayBuffer();
  if (buf.byteLength === 0) {
    throw new Error("Downloaded file is empty");
  }
  log(`Downloaded ${(buf.byteLength / (1024 * 1024)).toFixed(2)} MB`);
  return {
    data: new Uint8Array(buf),
    offset: part.offset ?? (mode === "app" ? APP_FLASH_OFFSET : FULL_FLASH_OFFSET),
    label: part.label,
  };
}

async function connect() {
  if (!("serial" in navigator)) {
    log("Web Serial is not supported. Use Chrome or Edge on desktop.");
    alert("Web Serial is not supported in this browser. Use Chrome or Edge.");
    return;
  }

  setBusy(true);
  try {
    log("Requesting serial port…");
    port = await navigator.serial.requestPort();
    detectedPortInfo =
      typeof port.getInfo === "function" ? port.getInfo() : {};
    detectedBoard = detectBoardFromPortInfo(detectedPortInfo);
    const vid = formatUsbId(detectedPortInfo?.usbVendorId);
    const pid = formatUsbId(detectedPortInfo?.usbProductId);
    if (detectedBoard) {
      log(
        `USB ${vid}:${pid} suggests ${BOARD_LABELS[detectedBoard]}. Confirm the board before Install.`,
      );
    } else {
      log(
        `USB ${vid}:${pid} is ambiguous. Select T-Encoder Pro or Waveshare manually.`,
      );
    }
    updateBoardStatus();
    transport = new Transport(port, true);
    esploader = new ESPLoader({
      transport,
      baudrate: 115200,
      romBaudrate: 115200,
      terminal: {
        clean: () => {},
        writeLine: (msg) => log(String(msg)),
        write: (msg) => log(String(msg)),
      },
    });

    log("Connecting…");
    await esploader.main();
    log("Chip detected - ready to flash after board confirmation.");
    els.status.className = "ok";
    setStatus("Connected");
  } catch (err) {
    log(`Connect failed: ${err.message || err}`);
    log("T-Encoder: hold BOOT and tap RESET. Waveshare: confirm the CH343 serial port, then try Connect again.");
    await disconnect();
  } finally {
    setBusy(false);
  }
}

async function disconnect() {
  try {
    if (transport) {
      await transport.disconnect();
    } else if (port) {
      await port.close();
    }
  } catch (err) {
    console.warn(err);
  }
  port = null;
  transport = null;
  esploader = null;
  detectedBoard = null;
  detectedPortInfo = null;
  els.status.className = "";
  setStatus("Not connected");
  setBusy(false);
  updateBoardStatus();
  log("Disconnected.");
}

async function flashBinary(data, label, address = FULL_FLASH_OFFSET) {
  if (!esploader) {
    throw new Error("Not connected");
  }

  setProgress(0, `Preparing ${label}…`);
  log(
    `Flashing ${label} at 0x${address.toString(16)} (${data.byteLength} bytes)…`,
  );
  if (address === FULL_FLASH_OFFSET) {
    log("Full factory image - bootloader, partitions, and app. Clears Wi‑Fi and saved settings.");
  } else {
    log("App-only image - requires an existing FlightScnr bootloader and partition table.");
  }

  // esptool-js 0.5.x expects a binary string, not Uint8Array (uses charCodeAt internally).
  const image =
    data instanceof Uint8Array ? esploader.ui8ToBstr(data) : data;

  await esploader.writeFlash({
    fileArray: [{ data: image, address }],
    flashSize: "16MB",
    flashMode: "qio",
    flashFreq: "80m",
    eraseAll: false,
    compress: true,
    reportProgress: (_fileIndex, written, total) => {
      const pct = total > 0 ? Math.round((written / total) * 100) : 0;
      setProgress(pct, `Flashing… ${pct}%`);
    },
  });

  log("Hard reset…");
  await esploader.after("hard_reset");
  setProgress(100, "Done");
  log("Flash complete. Unplug USB and reconnect to restart FlightScnr.");

  if (address === FULL_FLASH_OFFSET && chipErased) {
    chipErased = false;
    updateInstallModeUI();
    log("Full install complete - app-only updates are available again.");
  }
}

async function runFlash(getData, label) {
  setBusy(true);
  try {
    const payload = await getData();
    if (payload && typeof payload === "object" && payload.data) {
      await flashBinary(payload.data, payload.label || label, payload.offset);
    } else {
      await flashBinary(payload, label);
    }
  } catch (err) {
    log(`Flash failed: ${err.message || err}`);
    log("Confirm the selected board. T-Encoder: hold BOOT and tap RESET. Waveshare: confirm the CH343 port, then retry.");
    clearProgress();
  } finally {
    setBusy(false);
  }
}

async function eraseChipFlash() {
  if (!esploader) {
    throw new Error("Not connected");
  }

  setProgress(0, "Erasing entire flash…");
  log("Erasing entire 16 MB flash chip (this may take a few minutes)…");

  await esploader.eraseFlash();

  log("Hard reset…");
  await esploader.after("hard_reset");
  setProgress(100, "Erase complete");
  chipErased = true;
  updateInstallModeUI();
  log(
    "Chip erase complete. Flash is blank - choose Full install (app-only is disabled until then).",
  );
}

async function runErase() {
  setBusy(true);
  try {
    await eraseChipFlash();
  } catch (err) {
    log(`Erase failed: ${err.message || err}`);
    clearProgress();
  } finally {
    setBusy(false);
  }
}

els.connectBtn.addEventListener("click", connect);
els.disconnectBtn.addEventListener("click", disconnect);
els.releaseSelect.addEventListener("change", updateInstallModeUI);
els.boardSelect.addEventListener("change", () => {
  const selected = els.boardSelect.value;
  if (selected === BOARD_AUTO) {
    localStorage.removeItem(BOARD_STORAGE_KEY);
  } else {
    localStorage.setItem(BOARD_STORAGE_KEY, selected);
    log(`Board override: ${BOARD_LABELS[selected]}.`);
  }
  updateInstallModeUI();
});

els.flashLatestBtn.addEventListener("click", () => {
  const mode = getInstallMode();
  const board = resolvedBoard();
  const label = board
    ? `FlightScnr-${board}-${mode === "app" ? "app" : "merged"}.bin`
    : "FlightScnr firmware";
  runFlash(() => fetchFirmwareForInstall(mode), label);
});

els.eraseBtn.addEventListener("click", () => {
  if (!esploader || busy) {
    return;
  }
  els.eraseDialog.showModal();
});

els.eraseCancelBtn.addEventListener("click", () => {
  els.eraseDialog.close();
});

els.eraseDialog.addEventListener("click", (event) => {
  if (event.target === els.eraseDialog) {
    els.eraseDialog.close();
  }
});

els.eraseConfirmBtn.addEventListener("click", () => {
  els.eraseDialog.close();
  runErase();
});

navigator.serial?.addEventListener("disconnect", () => {
  log("Serial device disconnected.");
  port = null;
  transport = null;
  esploader = null;
  detectedBoard = null;
  detectedPortInfo = null;
  els.status.className = "";
  setStatus("Not connected");
  els.connectBtn.disabled = false;
  els.disconnectBtn.disabled = true;
  els.flashLatestBtn.disabled = true;
  els.eraseBtn.disabled = true;
  updateBoardStatus();
});

const savedBoard = localStorage.getItem(BOARD_STORAGE_KEY);
if (savedBoard === BOARD_TENCODER || savedBoard === BOARD_WAVESHARE) {
  els.boardSelect.value = savedBoard;
}
updateInstallModeUI();
loadReleaseOptions();
log("Ready. Use Chrome or Edge on desktop.");
log("Auto detection uses USB VID/PID hints; always confirm the board before Install.");
log("T-Encoder: if the port is missing, hold BOOT and tap RESET. Waveshare: select the CH343 port.");
