import { ESPLoader, Transport } from "./vendor/esptool-js.bundle.js";

const APP_ASSET = "FlightScnr-tencoder-pro-app.bin";
const APP_FLASH_OFFSET = 0x10000;
const FIRMWARE_BASE = "./firmware";
const MANIFEST_URL = `${FIRMWARE_BASE}/manifest.json`;

const els = {
  connectBtn: document.getElementById("connect-btn"),
  disconnectBtn: document.getElementById("disconnect-btn"),
  flashLatestBtn: document.getElementById("flash-latest-btn"),
  eraseBtn: document.getElementById("erase-btn"),
  eraseDialog: document.getElementById("erase-dialog"),
  eraseCancelBtn: document.getElementById("erase-cancel-btn"),
  eraseConfirmBtn: document.getElementById("erase-confirm-btn"),
  fileInput: document.getElementById("file-input"),
  status: document.getElementById("status"),
  releaseMeta: document.getElementById("release-meta"),
  progressWrap: document.getElementById("progress-wrap"),
  progress: document.getElementById("progress"),
  progressLabel: document.getElementById("progress-label"),
  log: document.getElementById("log"),
};

let port = null;
let transport = null;
let esploader = null;
let busy = false;

function log(line) {
  const ts = new Date().toLocaleTimeString();
  els.log.textContent += `[${ts}] ${line}\n`;
  els.log.scrollTop = els.log.scrollHeight;
}

function setStatus(text) {
  els.status.textContent = text;
}

function setBusy(value) {
  busy = value;
  els.connectBtn.disabled = value || port !== null;
  els.disconnectBtn.disabled = value || port === null;
  els.flashLatestBtn.disabled = value || port === null;
  els.eraseBtn.disabled = value || port === null;
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

async function loadFirmwareManifest() {
  const resp = await fetch(MANIFEST_URL, { cache: "no-store" });
  if (!resp.ok) {
    throw new Error(`Manifest unavailable (HTTP ${resp.status})`);
  }
  return resp.json();
}

async function loadLatestReleaseMeta() {
  try {
    const manifest = await loadFirmwareManifest();
    const sizeMb = manifest.size
      ? (manifest.size / (1024 * 1024)).toFixed(2)
      : "?";
    els.releaseMeta.textContent = `Latest: ${manifest.name || manifest.version} (${sizeMb} MB)`;
  } catch (err) {
    els.releaseMeta.textContent =
      "Firmware not bundled yet (run Release workflow, then redeploy Pages). You can still upload a .bin file.";
    console.warn(err);
  }
}

async function fetchLatestFirmware() {
  const manifest = await loadFirmwareManifest();
  const part = manifest.builds?.[0]?.parts?.[0];
  if (!part?.path) {
    throw new Error("Firmware manifest has no image path");
  }
  const url = `${FIRMWARE_BASE}/${part.path}`;
  const offset = part.offset ?? APP_FLASH_OFFSET;

  log(`Downloading ${manifest.name || manifest.version || part.path}…`);
  const resp = await fetch(url, { cache: "no-store" });
  if (!resp.ok) {
    throw new Error(`Download failed (HTTP ${resp.status})`);
  }
  const buf = await resp.arrayBuffer();
  if (buf.byteLength === 0) {
    throw new Error("Downloaded file is empty");
  }
  log(`Downloaded ${(buf.byteLength / (1024 * 1024)).toFixed(2)} MB`);
  return { data: new Uint8Array(buf), offset, label: part.path };
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
    log("Chip detected — ready to flash.");
    els.status.className = "ok";
    setStatus("Connected");
  } catch (err) {
    log(`Connect failed: ${err.message || err}`);
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

async function flashBinary(data, label, address = APP_FLASH_OFFSET) {
  if (!esploader) {
    throw new Error("Not connected");
  }

  setProgress(0, `Preparing ${label}…`);
  log(
    `Flashing ${label} at 0x${address.toString(16)} (${data.byteLength} bytes, no full-chip erase)…`,
  );

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
  log("Flash complete. FlightScnr should boot shortly.");
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
  log(
    "Chip erase complete. Flash is blank — use Install or upload a .bin to flash firmware.",
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

els.flashLatestBtn.addEventListener("click", () => {
  runFlash(() => fetchLatestFirmware(), APP_ASSET);
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

els.fileInput.addEventListener("change", async () => {
  const file = els.fileInput.files?.[0];
  els.fileInput.value = "";
  if (!file) {
    return;
  }
  const offset = /merged/i.test(file.name) ? 0 : APP_FLASH_OFFSET;
  if (offset === 0) {
    log("Warning: merged images at 0x0 replace the full flash and erase saved settings.");
  }
  await runFlash(async () => {
    log(`Reading ${file.name}…`);
    return {
      data: new Uint8Array(await file.arrayBuffer()),
      offset,
    };
  }, file.name);
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

loadLatestReleaseMeta();
log("Ready. Use Chrome or Edge on desktop.");
log("Hold BOOT (not the knob) if the port does not appear.");
