import { ESPLoader, Transport } from "./vendor/esptool-js.bundle.js";

const REPO = "yashmulgaonkar/FlightScnr";
const MERGED_ASSET = "FlightScnr-tencoder-pro-merged.bin";
const LATEST_URL = `https://github.com/${REPO}/releases/latest/download/${MERGED_ASSET}`;

const els = {
  connectBtn: document.getElementById("connect-btn"),
  disconnectBtn: document.getElementById("disconnect-btn"),
  flashLatestBtn: document.getElementById("flash-latest-btn"),
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

async function loadLatestReleaseMeta() {
  try {
    const resp = await fetch(`https://api.github.com/repos/${REPO}/releases/latest`);
    if (!resp.ok) {
      throw new Error(`HTTP ${resp.status}`);
    }
    const data = await resp.json();
    const asset = (data.assets || []).find((a) => a.name === MERGED_ASSET);
    const sizeMb = asset ? (asset.size / (1024 * 1024)).toFixed(2) : "?";
    els.releaseMeta.textContent = `Latest: ${data.name || data.tag_name} (${sizeMb} MB)`;
  } catch (err) {
    els.releaseMeta.textContent = "Latest release info unavailable (you can still upload a .bin file).";
    console.warn(err);
  }
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

async function fetchFirmware(url) {
  log(`Downloading ${url}`);
  const resp = await fetch(url);
  if (!resp.ok) {
    throw new Error(`Download failed (${resp.status})`);
  }
  const buf = await resp.arrayBuffer();
  log(`Downloaded ${(buf.byteLength / (1024 * 1024)).toFixed(2)} MB`);
  return new Uint8Array(buf);
}

async function flashBinary(data, label) {
  if (!esploader) {
    throw new Error("Not connected");
  }

  setProgress(0, `Preparing ${label}…`);
  log(`Flashing ${label} at 0x0 (${data.byteLength} bytes)…`);

  await esploader.writeFlash({
    fileArray: [{ data, address: 0 }],
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
    const data = await getData();
    await flashBinary(data, label);
  } catch (err) {
    log(`Flash failed: ${err.message || err}`);
    clearProgress();
  } finally {
    setBusy(false);
  }
}

els.connectBtn.addEventListener("click", connect);
els.disconnectBtn.addEventListener("click", disconnect);

els.flashLatestBtn.addEventListener("click", () => {
  runFlash(() => fetchFirmware(LATEST_URL), MERGED_ASSET);
});

els.fileInput.addEventListener("change", async () => {
  const file = els.fileInput.files?.[0];
  els.fileInput.value = "";
  if (!file) {
    return;
  }
  await runFlash(async () => {
    log(`Reading ${file.name}…`);
    return new Uint8Array(await file.arrayBuffer());
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
});

loadLatestReleaseMeta();
log("Ready. Use Chrome or Edge on desktop.");
log("Hold BOOT (not the knob) if the port does not appear.");
