const $ = (id) => document.getElementById(id);
const statusEl = $('status');
const ledSwitch = $('led-switch');
const ledLabel = $('led-label');
const btnSwitch = $('btn-switch');
const btnLabel = $('btn-label');

// Treat common "pressed" values as ON
function isOn(v) {
  return v === true || v === 1 || ['1', 'on', 'true', 'pressed', 'down'].includes(String(v).toLowerCase());
}

function setAxis(axis, value, accelMax) {
  const v = Number(value) || 0;
  $('ax-' + axis).textContent = v.toFixed(2);
  const pct = Math.min(Math.abs(v) / accelMax, 1) * 50; // half the bar each way
  const bar = $('bar-' + axis);
  bar.style.width = pct + '%';
  bar.style.left = v >= 0 ? '50%' : 50 - pct + '%';
}

async function initApp() {
  let config;
  
  try {
    const response = await fetch('config.json');
    if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
    config = await response.json();
  } catch (err) {
    console.error('Failed to load configuration:', err);
    statusEl.textContent = 'Config Error';
    statusEl.className = 'pill disconnected';
    return;
  }

  const { brokerUrl, pubTopic, subTopic, accelMax = 2 } = config;

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
      setAxis('x', data.mpu_accel.x, accelMax);
      setAxis('y', data.mpu_accel.y, accelMax);
      setAxis('z', data.mpu_accel.z, accelMax);
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
}

// Verification step: Ensure DOM is loaded before starting initialization
document.addEventListener('DOMContentLoaded', initApp);