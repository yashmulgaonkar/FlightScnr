import { ESPLoader, Transport } from "./vendor/esptool-js.bundle.js";

const MERGED_ASSET = "FlightScnr-tencoder-pro-merged.bin";
const APP_ASSET = "FlightScnr-tencoder-pro-app.bin";
const FULL_FLASH_OFFSET = 0;
const APP_FLASH_OFFSET = 0x10000;
const FIRMWARE_BASE = "./firmware";
const MANIFEST_URL = `${FIRMWARE_BASE}/manifest.json`;
const RELEASES_API_URL =
  "https://api.github.com/repos/yashmulgaonkar/FlightScnr/releases?per_page=20";
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

function releaseAllowsAppInstall(release = selectedRelease()) {
  return Boolean(release?.allowAppOnly && release?.appPart);
}

function updateReleaseMeta() {
  const release = selectedRelease();
  if (!release) {
    els.releaseMeta.textContent = "Loading firmware releases...";
    els.releaseHelp.textContent = "";
    return;
  }

  const prefix = release.source === "bundled" ? "Latest bundled" : "Selected";
  const details = [
    release.name || release.version,
    formatPublishedDate(release.publishedAt),
    `${formatSizeMb(release.fullPart?.size)} full image`,
  ].filter(Boolean);
  els.releaseMeta.textContent = `${prefix}: ${details.join(" | ")}`;

  const notes = [];
  if (chipErased) {
    notes.push("Chip erase in this session requires Full install.");
  }
  if (release.source === "bundled") {
    notes.push("Latest release supports both Full install and Update app only.");
  } else if (release.appPart) {
    notes.push("Historical releases use Full install only for safety.");
  } else {
    notes.push("This historical release only includes a full-image installer.");
  }
  if (releaseLoadWarning) {
    notes.push(releaseLoadWarning);
  }
  els.releaseHelp.textContent = notes.join(" ");
}

function getInstallMode() {
  if (chipErased || els.installModeApp.disabled) {
    return "full";
  }
  return els.installModeApp.checked ? "app" : "full";
}

function updateInstallModeUI() {
  const appDisabled = busy || chipErased || !releaseAllowsAppInstall();
  els.installModeFull.disabled = busy;
  els.installModeApp.disabled = appDisabled;
  els.installModeAppLabel.classList.toggle("disabled", appDisabled);
  if (appDisabled) {
    els.installModeFull.checked = true;
  }
  updateReleaseMeta();
}

function setBusy(value) {
  busy = value;
  els.connectBtn.disabled = value || port !== null;
  els.disconnectBtn.disabled = value || port === null;
  els.flashLatestBtn.disabled = value || port === null;
  els.eraseBtn.disabled = value || port === null;
  els.releaseSelect.disabled = value || releaseChoices.length <= 1;
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

function manifestPart(manifest, mode) {
  const parts = manifest.builds?.[0]?.parts ?? [];
  if (mode === "app") {
    return (
      parts.find((part) => part.role === "app") ??
      parts.find((part) => /app/i.test(part.path ?? "")) ??
      { path: APP_ASSET, offset: APP_FLASH_OFFSET }
    );
  }
  return (
    parts.find((part) => part.role === "full") ??
    parts.find((part) => /merged/i.test(part.path ?? "")) ??
    parts[0] ?? { path: MERGED_ASSET, offset: FULL_FLASH_OFFSET }
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
  const fullPart = manifestPart(manifest, "full");
  const appPart = manifestPart(manifest, "app");
  return {
    id: BUNDLED_RELEASE_ID,
    source: "bundled",
    name: manifest.name || manifest.version || "Latest release",
    version: manifest.version || "",
    publishedAt: null,
    allowAppOnly: Boolean(appPart?.path),
    fullPart: {
      url: `${FIRMWARE_BASE}/${fullPart.path}`,
      offset: fullPart.offset ?? FULL_FLASH_OFFSET,
      size: fullPart.size ?? manifest.size ?? null,
      label: fullPart.path,
      headers: null,
    },
    appPart: appPart?.path
      ? {
          url: `${FIRMWARE_BASE}/${appPart.path}`,
          offset: appPart.offset ?? APP_FLASH_OFFSET,
          size: appPart.size ?? null,
          label: appPart.path,
          headers: null,
        }
      : null,
  };
}

function buildGitHubRelease(release) {
  const assets = Array.isArray(release.assets) ? release.assets : [];
  const fullAsset =
    assets.find((asset) => asset.name === MERGED_ASSET) ??
    assets.find((asset) => /merged/i.test(asset.name ?? ""));
  if (!fullAsset) {
    return null;
  }
  const appAsset =
    assets.find((asset) => asset.name === APP_ASSET) ??
    assets.find((asset) => /app/i.test(asset.name ?? ""));
  return {
    id: `release:${release.tag_name}`,
    source: "github",
    name: release.name || release.tag_name,
    version: release.tag_name,
    publishedAt: release.published_at || release.created_at || null,
    allowAppOnly: false,
    fullPart: {
      url: fullAsset.url,
      offset: FULL_FLASH_OFFSET,
      size: fullAsset.size ?? null,
      label: fullAsset.name,
      headers: { Accept: "application/octet-stream" },
    },
    appPart: appAsset
      ? {
          url: appAsset.url,
          offset: APP_FLASH_OFFSET,
          size: appAsset.size ?? null,
          label: appAsset.name,
          headers: { Accept: "application/octet-stream" },
        }
      : null,
  };
}

async function loadGitHubReleases() {
  const resp = await fetch(RELEASES_API_URL, {
    cache: "no-store",
    headers: { Accept: "application/vnd.github+json" },
  });
  if (!resp.ok) {
    throw new Error(`Release list unavailable (HTTP ${resp.status})`);
  }
  const releases = await resp.json();
  if (!Array.isArray(releases)) {
    throw new Error("Release list response was not an array");
  }
  return releases
    .filter((release) => !release.draft && !release.prerelease)
    .map(buildGitHubRelease)
    .filter(Boolean);
}

function populateReleaseSelect() {
  const previous = els.releaseSelect.value;
  els.releaseSelect.innerHTML = "";
  for (const release of releaseChoices) {
    const option = document.createElement("option");
    option.value = release.id;
    if (release.source === "bundled") {
      option.textContent = `Latest (${release.version || "current"})`;
    } else {
      const published = formatPublishedDate(release.publishedAt);
      option.textContent = published
        ? `${release.version} - ${published}`
        : release.version || release.name;
    }
    els.releaseSelect.appendChild(option);
  }
  const selected = releaseChoices.some((release) => release.id === previous)
    ? previous
    : BUNDLED_RELEASE_ID;
  els.releaseSelect.value = selected;
  els.releaseSelect.disabled = busy || releaseChoices.length <= 1;
  updateInstallModeUI();
}

async function loadReleaseOptions() {
  const choices = [];

  try {
    const manifest = await loadFirmwareManifest();
    choices.push(buildBundledRelease(manifest));
  } catch (err) {
    console.warn("Bundled manifest unavailable:", err);
  }

  try {
    const historical = await loadGitHubReleases();
    const bundledVersion = choices[0]?.version ?? "";
    for (const release of historical) {
      if (release.version === bundledVersion) {
        continue;
      }
      choices.push(release);
    }
    releaseLoadWarning = "";
  } catch (err) {
    releaseLoadWarning =
      "Older GitHub releases are unavailable right now.";
    console.warn("GitHub releases unavailable:", err);
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
  if (!release) {
    throw new Error("No firmware release is selected");
  }
  const part = mode === "app" ? release.appPart : release.fullPart;
  if (!part?.url) {
    throw new Error("Selected release does not have the requested installer");
  }

  const releaseLabel = release.version || release.name || part.label;
  log(
    `Downloading ${releaseLabel} (${mode === "app" ? "app-only" : "full install"})…`,
  );
  const resp = await fetch(part.url, {
    cache: "no-store",
    headers: part.headers ?? undefined,
  });
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
    log("Chip detected - ready to flash.");
    els.status.className = "ok";
    setStatus("Connected");
  } catch (err) {
    log(`Connect failed: ${err.message || err}`);
    log("Push the screen down (BOOT) and hold, tap RESET on the back of the board, then try Connect again.");
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
  els.status.className = "";
  setStatus("Not connected");
  setBusy(false);
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
      await flashBinary(payload.data, label, payload.offset);
    } else {
      await flashBinary(payload, label);
    }
  } catch (err) {
    log(`Flash failed: ${err.message || err}`);
    log("Push the screen down (BOOT) and hold, tap RESET on the back of the board, then try Install again.");
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

els.flashLatestBtn.addEventListener("click", () => {
  const mode = getInstallMode();
  const label = mode === "app" ? APP_ASSET : MERGED_ASSET;
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
  els.status.className = "";
  setStatus("Not connected");
  els.connectBtn.disabled = false;
  els.disconnectBtn.disabled = true;
  els.flashLatestBtn.disabled = true;
  els.eraseBtn.disabled = true;
});

updateInstallModeUI();
loadReleaseOptions();
log("Ready. Use Chrome or Edge on desktop.");
log("If the port is missing: push the screen down (BOOT) and hold, tap RESET on the back of the board, then Connect.");
log("If Connect or Install fails: hold the screen in (BOOT) and retry until flashing starts.");
