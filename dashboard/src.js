// Broker details (Mosquitto Public Broker WebSocket Port)
const brokerUrl = 'wss://test.mosquitto.org:8081';

// Topics
const pubTopic = 'r_topic_place_holder';
const subTopic = 'r_topic_place_holder/data';

// DOM elements
const statusEl = document.getElementById('status');
const logEl = document.getElementById('log');
const ledOnBtn = document.getElementById('led-on-btn');
const ledOffBtn = document.getElementById('led-off-btn');

function log(msg) {
  const entry = document.createElement('div');
  entry.textContent = `[${new Date().toLocaleTimeString()}] ${msg}`;
  logEl.appendChild(entry);
  logEl.scrollTop = logEl.scrollHeight;
}

log('Connecting to broker...');

// Initialize MQTT Client
const client = mqtt.connect(brokerUrl, {
  clientId: 'js_client_' + Math.random().toString(16).substring(2, 10),
  clean: true,
  connectTimeout: 4000,
});

// MQTT Event Handlers
client.on('connect', () => {
  statusEl.textContent = 'Connected';
  statusEl.className = 'status connected';
  log('Connected to ' + brokerUrl);

  // Subscribe to telemetry incoming data topic
  client.subscribe(subTopic, (err) => {
    if (!err) {
      log(`Subscribed to topic: "${subTopic}"`);
    } else {
      log(`Subscription error: ${err}`);
    }
  });
});

client.on('message', (topic, payload) => {
  try {
    const data = JSON.parse(payload.toString());
    log(`Received on [${topic}]: Button=${data.button || 'N/A'}, Accel=X:${data.mpu_accel?.x ?? 0}, Y:${data.mpu_accel?.y ?? 0}, Z:${data.mpu_accel?.z ?? 0}`);
  } catch (e) {
    // Fallback if message isn't valid JSON
    log(`Received on [${topic}] (raw): ${payload.toString()}`);
  }
});

client.on('error', (err) => {
  log('Connection error: ' + err);
});

client.on('close', () => {
  statusEl.textContent = 'Disconnected';
  statusEl.className = 'status disconnected';
});

// Helper function to send LED commands
function sendLedCommand(state) {
  if (client.connected) {
    const payload = JSON.stringify({ led: state });
    client.publish(pubTopic, payload);
    log(`Published to [${pubTopic}]: ${payload}`);
  } else {
    alert('MQTT client is not connected!');
  }
}

// Event Listeners for LED Buttons
ledOnBtn.addEventListener('click', () => sendLedCommand('on'));
ledOffBtn.addEventListener('click', () => sendLedCommand('off'));