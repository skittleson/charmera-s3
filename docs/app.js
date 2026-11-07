// Charmera-S3 Web Bluetooth remote. No build step, no dependencies.
'use strict';

const uuid = (n) => `c4a3000${n}-7b6e-4f0a-9d2e-5a1c8e3f6b20`;
const SERVICE = uuid(0);
const STATUS = uuid(1);
const COMMAND = uuid(2);
const INFO = uuid(3);

const $ = (id) => document.getElementById(id);
const enc = new TextEncoder();
const dec = new TextDecoder();

let device = null;
let chars = {};
let status = null;
let sending = Promise.resolve(); // GATT allows one operation at a time

function setConnState(text) {
  $('conn-state').textContent = text;
}

function fill(select, names) {
  select.innerHTML = '';
  names.forEach((name, i) => select.add(new Option(name, i)));
}

function render() {
  const connected = !!(device && device.gatt.connected);
  $('panel').classList.toggle('disabled', !connected);
  $('connect').textContent = connected ? 'Disconnect' : (device ? 'Reconnect' : 'Connect to camera');
  if (!status) return;
  const s = status;
  $('photos').textContent = s.photos;
  $('left').textContent = s.card === 'none' ? 'No card' : s.card === 'full' ? 'Full' : s.left;
  $('battery').textContent = s.usb ? '🔌 USB' : `${s.battery}%`;
  $('camera').textContent = s.camera;
  $('look').value = s.look;
  $('interval').value = s.interval;
  $('mode-single').setAttribute('aria-checked', s.mode === 'single');
  $('mode-lapse').setAttribute('aria-checked', s.mode === 'lapse');

  $('lapse-card').hidden = !s.running;
  if (s.running) {
    const m = Math.floor(s.elapsed / 60), sec = String(s.elapsed % 60).padStart(2, '0');
    $('lapse').textContent = `● ${s.frames} frames · ${m}:${sec} · ${s.folder}`;
  }
  $('error').hidden = !s.error;
  $('error').textContent = s.error ? `Camera said: ${s.error}` : '';
  $('last').textContent = s.last ? `Last saved: ${s.last}` : '';

  // During a run only Stop works; while saving, nothing new can start.
  const locked = s.running || s.busy;
  for (const id of ['look', 'interval', 'mode-single', 'mode-lapse', 'shoot']) $(id).disabled = locked;
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
    // A notification cut short by a small MTU: read the whole value instead.
    sending = sending.then(readStatus).catch(() => {});
  }
}

function send(text) {
  sending = sending
    .then(() => chars.command.writeValueWithResponse(enc.encode(text)))
    .catch((e) => setConnState(`Send failed: ${e.message}`));
  return sending;
}

async function setup() {
  setConnState('Connecting…');
  const server = await device.gatt.connect();
  const svc = await server.getPrimaryService(SERVICE);
  chars.status = await svc.getCharacteristic(STATUS);
  chars.command = await svc.getCharacteristic(COMMAND);
  chars.info = await svc.getCharacteristic(INFO);
  const info = JSON.parse(dec.decode(await chars.info.readValue()));
  fill($('look'), info.looks);
  fill($('interval'), info.intervals);
  chars.status.addEventListener('characteristicvaluechanged', onStatus);
  await chars.status.startNotifications();
  await readStatus();
  setConnState(`Connected to ${device.name || 'camera'} · firmware ${info.version}`);
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
        setConnState('Disconnected. Press Reconnect when the camera is on.');
        render();
      });
    }
    await setup();
  } catch (e) {
    setConnState(e.name === 'NotFoundError' ? 'No camera chosen' : `Couldn't connect: ${e.message}`);
    render();
  }
}

function init() {
  if (!navigator.bluetooth) {
    $('unsupported').hidden = false;
    $('connect').disabled = true;
    return;
  }
  $('connect').addEventListener('click', connect);
  $('look').addEventListener('change', (e) => send(`look ${e.target.value}`));
  $('interval').addEventListener('change', (e) => send(`interval ${e.target.value}`));
  $('mode-single').addEventListener('click', () => send('mode single'));
  $('mode-lapse').addEventListener('click', () => send(`mode lapse ${$('interval').value || ''}`.trim()));
  $('shoot').addEventListener('click', () => send('shoot'));
  $('lapse-start').addEventListener('click', () => send('lapse start'));
  $('lapse-stop').addEventListener('click', () => {
    if (confirm('Stop the time-lapse?')) send('lapse stop');
  });
  render();
}

init();
