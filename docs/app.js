// Charmera-S3 — camera interface over Web Bluetooth. No build step, no dependencies.
'use strict';

const uuid = (n) => `c4a3000${n}-7b6e-4f0a-9d2e-5a1c8e3f6b20`;
const SERVICE = uuid(0);
const STATUS = uuid(1);
const COMMAND = uuid(2);
const INFO = uuid(3);
const STREAM = uuid(4);

const $ = (id) => document.getElementById(id);
const enc = new TextEncoder();
const dec = new TextDecoder();

let device = null;
let chars = {};
let status = null;
let sending = Promise.resolve(); // GATT allows one operation at a time
let ready = false; // setup finished; controls may be used
let info = null;
let timeSyncTimer = null;

function sendTimeSync() {
  if (device && device.gatt.connected && chars.command) {
    send(`time ${Math.floor(Date.now() / 1000)} ${-new Date().getTimezoneOffset()}`);
  }
}

// ---------- tiny localStorage helpers ----------
const store = {
  get(k, d) { try { const v = localStorage.getItem(k); return v === null ? d : JSON.parse(v); } catch { return d; } },
  set(k, v) { try { localStorage.setItem(k, JSON.stringify(v)); } catch { /* private mode */ } },
};

// ---------- stream reassembler ----------
// Frames from the camera: [u8 kind][payload]; 0 = meta JSON, 1 = data, 2 = end (u32 total).
let stream = null; // {buf, meta, done, resolve, reject, timer}

function onStream(event) {
  if (!stream) return;
  const v = event.target.value; // DataView
  if (v.byteLength < 1) return;
  const kind = v.getUint8(0);
  // Copy: Chrome may reuse the notification's buffer for the next one.
  const body = new Uint8Array(v.buffer.slice(v.byteOffset + 1, v.byteOffset + v.byteLength));
  if (kind === 0) {
    try { stream.meta = JSON.parse(dec.decode(body)); } catch { stream.meta = null; }
  } else if (kind === 1) {
    stream.chunks.push(body);
    stream.got += body.length;
    if (stream.progress) stream.progress(stream.got, stream.meta && stream.meta.size);
  } else if (kind === 2) {
    const total = v.byteLength >= 5 ? v.getUint32(1) : stream.got;
    finishStream(stream.got === total ? null : new Error(`truncated (${stream.got}/${total})`));
  } else if (kind === 3) {
    finishStream(new Error(dec.decode(body) || 'refused'));
  }
}

function finishStream(err) {
  const s = stream;
  if (!s) return;
  stream = null;
  clearTimeout(s.timer);
  if (err) s.reject(err);
  else s.resolve(new Blob(s.chunks, { type: 'image/jpeg' }));
}

/* Runs list/thumb/get; resolves to a Blob of the streamed bytes. The camera streams one
   file at a time, so every caller goes through this queue. */
let fileQueue = Promise.resolve();
function fetchFile(command, progress) {
  const run = fileQueue.catch(() => {}).then(() => fetchFileNow(command, progress));
  fileQueue = run;
  return run;
}

/* A rejection on the camera shows up in Status.error, but Status may still hold a
   *previous* rejection, so only a new error (or 15 s of silence) fails the call. */
function fetchFileNow(command, progress) {
  return new Promise((resolve, reject) => {
    if (stream) { reject(new Error('a download is already running')); return; }
    stream = { chunks: [], got: 0, meta: null, progress, resolve, reject, timer: null };
    sending
      .then(() => chars.command.writeValueWithResponse(enc.encode(command)))
      .catch((e) => finishStream(e));
    const t0 = Date.now();
    const errBefore = status && status.error;
    const deadline = t0 + 300000; // full-res over a small MTU can crawl
    const poll = setInterval(async () => {
      if (!stream) { clearInterval(poll); return; }
      if (stream.got || stream.meta) { clearInterval(poll); return; } // camera answered: frames rule
      if (Date.now() > deadline) { clearInterval(poll); finishStream(new Error('timed out')); return; }
      if (!device.gatt.connected) { clearInterval(poll); finishStream(new Error('disconnected')); return; }
      // A refusal shows in Status.error within a moment; listing a big card can take
      // several seconds before its first frame, so only give up on silence after 15 s.
      const elapsed = Date.now() - t0;
      if ((elapsed > 1500 && status && status.error && status.error !== errBefore) || elapsed > 15000) {
        clearInterval(poll);
        finishStream(new Error((status && status.error) || 'no response'));
      }
    }, 150);
  });
}

// ---------- status ----------

function render() {
  const connected = !!(device && device.gatt.connected) && ready;
  $('app').classList.toggle('disabled', !connected);
  $('conn-chip').textContent = connected ? 'Connected' : 'Not connected';
  $('conn-chip').title = connected ? 'Tap to disconnect' : '';
  $('conn-chip').classList.toggle('on', connected);
  if (!status) return;
  const s = status;
  $('photos').textContent = s.photos;
  $('left').textContent = s.card === 'none' ? 'No card' : s.card === 'full' ? 'Full' : s.left;
  $('battery').textContent = s.usb ? 'Charging' : `${s.battery}%`;
  $('interval').value = s.interval;
  if (s.orientationSetting) $('orientation').value = s.orientationSetting;
  $('mode-single').setAttribute('aria-checked', s.mode === 'single');
  $('mode-lapse').setAttribute('aria-checked', s.mode === 'lapse');
  // One shutter vocabulary: single mode shows the shutter+preview, lapse shows Start/Stop.
  $('shutter-row').hidden = s.mode !== 'single';
  $('preview-box').hidden = s.mode !== 'single';
  $('lapse-row').hidden = s.mode !== 'lapse';
  $('interval').hidden = s.mode !== 'lapse';
  paintLooks();

  $('lapse-line').hidden = !s.running;
  if (s.running) {
    const m = Math.floor(s.elapsed / 60), sec = String(s.elapsed % 60).padStart(2, '0');
    $('lapse-line').textContent = `● time-lapse: ${s.frames} frames · ${m}:${sec} · ${s.folder}`;
  }
  $('error').hidden = !s.error;
  $('error').textContent = s.error ? `Camera: ${s.error}` : '';
  $('last').textContent = s.last ? `Last saved: ${s.last}` : '';

  const locked = s.running || s.busy;
  for (const id of ['interval', 'orientation', 'mode-single', 'mode-lapse', 'shoot']) $(id).disabled = locked;
  $('lapse-start').disabled = locked;
  $('lapse-stop').disabled = !s.running;
  $('shoot').classList.toggle('busy', s.busy);
}

async function readStatus() {
  const v = await chars.status.readValue();
  status = JSON.parse(dec.decode(v));
  render();
}

function onStatus(event) {
  try {
    status = JSON.parse(dec.decode(event.target.value));
    render();
  } catch {
    sending = sending.then(readStatus).catch(() => {});
  }
}

function send(text) {
  sending = sending
    .then(() => chars.command.writeValueWithResponse(enc.encode(text)))
    .catch(() => {});
  return sending;
}

// ---------- looks ----------

function buildLooks() {
  const sel = $('looks');
  sel.innerHTML = '';
  (info ? info.looks : []).forEach((name, i) => sel.add(new Option(name, i)));
}

function paintLooks() {
  if (!status) return;
  const sel = $('looks');
  if (+sel.value !== status.look) sel.value = status.look;
  sel.disabled = status.running || status.busy;
}

// ---------- shoot + preview ----------

async function shoot() {
  const before = status ? status.photos : -1;
  await send('shoot');
  if (before < 0) return;
  // Wait for the save: busy is published before the shutter fires, count after.
  const t0 = Date.now();
  while (Date.now() - t0 < 60000) {
    await new Promise((r) => setTimeout(r, 250));
    const s = status;
    if (!s) break;
    if (s.photos === before + 1 && !s.busy) {
      if (store.get('preview', true)) refreshPreview(s.last);
      return;
    }
    if (!s.busy && s.photos === before && Date.now() - t0 > 4000 && s.error) return; // refused
  }
}

async function refreshPreview(name) {
  if (!name || !/IMG_(\d{4})$/.test(name)) return;
  const n = name.match(/IMG_(\d{4})$/)[1];
  $('preview-progress').hidden = false;
  $('preview-text').hidden = true;
  try {
    const blob = await fetchFile(`thumb ${n}`);
    const url = URL.createObjectURL(blob);
    const img = $('preview');
    img.src = url;
    img.hidden = false;
    $('preview-hint').hidden = true;
    $('preview').dataset.n = n;
  } catch (e) {
    $('preview-progress').hidden = true;
    $('preview-text').hidden = false;
    $('preview-text').textContent = e.message === 'no preview'
      ? 'No preview for this shot (taken before this firmware).'
      : `Preview failed: ${e.message}`;
  }
}

// ---------- gallery ----------

let gallery = []; // [{n, size}]

async function loadGallery() {
  $('gallery-info').textContent = 'Reading card…';
  try {
    const blob = await fetchFile('list');
    gallery = JSON.parse(await blob.text());
    gallery.sort((a, b) => b[0] - a[0]); // newest first
    $('gallery-info').textContent = `${gallery.length} photos`;
    buildList();
  } catch (e) {
    $('gallery-info').textContent = `Failed: ${e.message}`;
  }
}

/* A plain list: name, size, a download button. Loading 100+ thumbnails over Bluetooth
   would take forever, so images only load in the viewer for the photo you picked. */
function buildList() {
  const list = $('photo-list');
  list.innerHTML = '';
  for (const [n, size] of gallery) {
    const row = document.createElement('div');
    row.className = 'prow';
    const name = document.createElement('button');
    name.className = 'prow-open';
    name.textContent = `IMG_${String(n).padStart(4, '0')}.jpg`;
    name.addEventListener('click', () => openViewer(n));
    const kb = document.createElement('span');
    kb.className = 'prow-size';
    kb.textContent = `${Math.round(size / 1024)} kB`;
    const dl = document.createElement('button');
    dl.className = 'prow-dl ghost';
    dl.textContent = '⬇';
    dl.title = 'Download full size';
    dl.addEventListener('click', () => downloadRow(n, dl));
    row.append(name, kb, dl);
    list.appendChild(row);
  }
}

/* Download straight from the list; the button shows progress and re-enables when done. */
async function downloadRow(n, btn) {
  btn.disabled = true;
  try {
    const blob = await fetchFile(`get ${n}`, (got, total) => {
      btn.textContent = total ? `${Math.round((got / total) * 100)}%` : `${got} B`;
    });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = `IMG_${String(n).padStart(4, '0')}.jpg`;
    a.click();
    btn.textContent = '✓';
    setTimeout(() => { btn.textContent = '⬇'; }, 1500);
  } catch (e) {
    btn.textContent = '✕';
    setTimeout(() => { btn.textContent = '⬇'; }, 1500);
  } finally {
    btn.disabled = false;
  }
}

async function openViewer(n) {
  const v = $('viewer');
  v.hidden = false;
  $('viewer-title').textContent = `IMG_${String(n).padStart(4, '0')}`;
  $('viewer-img').removeAttribute('src');
  $('viewer-download').disabled = true;
  $('viewer-progress').textContent = 'Loading thumbnail…';
  try {
    const blob = await fetchFile(`thumb ${n}`, (got, total) => {
      $('viewer-progress').textContent = `Loading thumbnail… ${got} B`;
    });
    $('viewer-progress').textContent = '';
    $('viewer-img').src = URL.createObjectURL(blob);
    $('viewer-download').disabled = false;
  } catch (e) {
    $('viewer-download').disabled = false; // full size still works without a sidecar
    $('viewer-progress').textContent =
      e.message === 'no preview' ? 'No preview stored for this photo — full size still downloads.' : `Failed: ${e.message}`;
  }
}

async function downloadFull(n) {
  $('viewer-download').disabled = true;
  $('viewer-progress').textContent = 'Downloading full size…';
  try {
    const blob = await fetchFile(`get ${n}`, (got, total) => {
      $('viewer-progress').textContent = total
        ? `Downloading full size… ${Math.round((got / total) * 100)}%`
        : `Downloading full size… ${got} B`;
    });
    $('viewer-progress').textContent = '';
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = `IMG_${String(n).padStart(4, '0')}.jpg`;
    a.click();
  } catch (e) {
    $('viewer-progress').textContent = `Download failed: ${e.message}`;
  } finally {
    $('viewer-download').disabled = false;
  }
}

// ---------- connection ----------

async function setup() {
  ready = false;
  const server = await device.gatt.connect();
  const svc = await server.getPrimaryService(SERVICE);
  chars.status = await svc.getCharacteristic(STATUS);
  chars.command = await svc.getCharacteristic(COMMAND);
  chars.info = await svc.getCharacteristic(INFO);
  chars.stream = await svc.getCharacteristic(STREAM);
  info = JSON.parse(dec.decode(await chars.info.readValue()));
  buildLooks();
  $('interval').innerHTML = '';
  info.intervals.forEach((name, i) => $('interval').add(new Option(name, i)));
  const now = `time ${Math.floor(Date.now() / 1000)} ${-new Date().getTimezoneOffset()}`;
  await chars.command.writeValueWithResponse(enc.encode(now));
  // Keep the camera's clock true while connected — it has no battery-backed RTC, and
  // every photo's EXIF carries it. Re-sync every 5 minutes; the page may stay open for days.
  clearInterval(timeSyncTimer);
  timeSyncTimer = setInterval(() => sendTimeSync(), 5 * 60 * 1000);
  chars.status.addEventListener('characteristicvaluechanged', onStatus);
  await chars.status.startNotifications();
  chars.stream.addEventListener('characteristicvaluechanged', onStream);
  await chars.stream.startNotifications();
  await readStatus();
  ready = true;
  render();
}

async function connect() {
  if (device && device.gatt.connected) {
    device.gatt.disconnect();
    return;
  }
  try {
    if (!device) {
      device = await navigator.bluetooth.requestDevice({
        filters: [{ services: [SERVICE] }],
        optionalServices: ['battery_service'],
      });
      device.addEventListener('gattserverdisconnected', () => {
        ready = false;
        clearInterval(timeSyncTimer);
        finishStream(new Error('disconnected'));
        render();
        $('connect-overlay').hidden = false; // back to the big Connect screen
        $('connect-hint').textContent = 'Disconnected — tap to reconnect';
      });
    }
    sending = sending.catch(() => {}).then(setup);
    await sending;
    $('connect-overlay').hidden = true;
  } catch (e) {
    sending = Promise.resolve();
    $('connect-hint').textContent =
      e.name === 'NotFoundError' ? 'No camera chosen.' : `Couldn't connect: ${e.message}`;
    render();
  }
}

// ---------- PWA ----------

function setupPwa() {
  if ('serviceWorker' in navigator) {
    navigator.serviceWorker.register('sw.js').catch((e) => console.warn('service worker:', e));
  }
  let prompt = null;
  window.addEventListener('beforeinstallprompt', (e) => {
    e.preventDefault();
    prompt = e;
    $('install').hidden = false;
  });
  $('install').addEventListener('click', async () => {
    if (!prompt) return;
    prompt.prompt();
    await prompt.userChoice;
    prompt = null;
    $('install').hidden = true;
  });
  window.addEventListener('appinstalled', () => { $('install').hidden = true; });
}

// ---------- tabs ----------

function showTab(which) {
  $('view-camera').hidden = which !== 'camera';
  $('view-gallery').hidden = which !== 'gallery';
  $('tab-camera').classList.toggle('active', which === 'camera');
  $('tab-gallery').classList.toggle('active', which === 'gallery');
  if (which === 'gallery' && !gallery.length) loadGallery();
}

// ---------- init ----------

function init() {
  setupPwa();
  if (!navigator.bluetooth) {
    $('unsupported').hidden = false;
    $('connect').disabled = true;
  }
  $('preview-toggle').checked = store.get('preview', true);
  $('preview-toggle').addEventListener('change', (e) => store.set('preview', e.target.checked));
  $('connect').addEventListener('click', connect);
  $('conn-chip').addEventListener('click', () => { if (device && device.gatt.connected) connect(); });
  $('looks').addEventListener('change', (e) => send(`look ${e.target.value}`));
  $('mode-single').addEventListener('click', () => send('mode single'));
  $('mode-lapse').addEventListener('click', () => send(`mode lapse ${$('interval').value || ''}`.trim()));
  $('interval').addEventListener('change', (e) => send(`interval ${e.target.value}`));
  $('orientation').addEventListener('change', (e) => send(`orientation ${e.target.value}`));
  $('shoot').addEventListener('click', shoot);
  $('lapse-start').addEventListener('click', () => send('lapse start'));
  $('lapse-stop').addEventListener('click', () => {
    if (confirm('Stop the time-lapse?')) send('lapse stop');
  });
  $('tab-camera').addEventListener('click', () => showTab('camera'));
  $('tab-gallery').addEventListener('click', () => showTab('gallery'));
  $('gallery-refresh').addEventListener('click', loadGallery);
  $('viewer-close').addEventListener('click', () => { $('viewer').hidden = true; });
  $('viewer-download').addEventListener('click', () => downloadFull(+$('viewer-title').textContent.replace(/\D/g, '')));
  render();
}

init();
