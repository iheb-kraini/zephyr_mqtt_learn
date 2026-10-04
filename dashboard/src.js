const brokerUrl = 'ws://broker.hivemq.com:8000/mqtt';
const pubTopic = 'r_topic_place_holder';
const subTopic = 'r_topic_place_holder/data';

// Full-scale of the accel bars (change to match your MPU range, e.g. 2 for ±2g)
const ACCEL_MAX = 2;

const $ = (id) => document.getElementById(id);
const statusEl = $('status');
const ledSwitch = $('led-switch');
const ledLabel = $('led-label');
const btnSwitch = $('btn-switch');
const btnLabel = $('btn-label');

const client = mqtt.connect(brokerUrl, {
  clientId: 'js_client_' + Math.random().toString(16).substring(2, 10),
  clean: true,
  connectTimeout: 4000,
});

client.on('connect', () => {
  statusEl.textContent = 'Connected';
  statusEl.className = 'pill connected';
  client.subscribe(subTopic);
});

client.on('close', () => {
  statusEl.textContent = 'Disconnected';
  statusEl.className = 'pill disconnected';
});

client.on('error', (err) => console.error('MQTT error:', err));

// Treat common "pressed" values as ON
function isOn(v) {
  return v === true || v === 1 || ['1', 'on', 'true', 'pressed', 'down'].includes(String(v).toLowerCase());
}

function setAxis(axis, value) {
  const v = Number(value) || 0;
  $('ax-' + axis).textContent = v.toFixed(2);
  const pct = Math.min(Math.abs(v) / ACCEL_MAX, 1) * 50; // half the bar each way
  const bar = $('bar-' + axis);
  bar.style.width = pct + '%';
  bar.style.left = v >= 0 ? '50%' : 50 - pct + '%';
}

client.on('message', (topic, payload) => {
  let data;
  try {
    data = JSON.parse(payload.toString());
  } catch {
    return; // ignore non-JSON messages
  }

  if (data.button !== undefined) {
    const on = isOn(data.button);
    btnSwitch.checked = on;
    btnLabel.textContent = on ? 'Pressed' : 'Released';
  }

  if (data.mpu_accel) {
    setAxis('x', data.mpu_accel.x);
    setAxis('y', data.mpu_accel.y);
    setAxis('z', data.mpu_accel.z);
  }

  $('updated').textContent = new Date().toLocaleTimeString();
});

// LED switch -> publish on/off
ledSwitch.addEventListener('change', () => {
  const state = ledSwitch.checked ? 'on' : 'off';
  if (!client.connected) {
    ledSwitch.checked = !ledSwitch.checked; // revert
    return;
  }
  client.publish(pubTopic, JSON.stringify({ led: state }));
  ledLabel.textContent = ledSwitch.checked ? 'On' : 'Off';
});