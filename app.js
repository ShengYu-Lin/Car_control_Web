// ── Camera buttons ──────────────────────────────────────────
document.addEventListener('selectstart', (event) => {
  event.preventDefault();
});

document.addEventListener('contextmenu', (event) => {
  event.preventDefault();
});

function camPress(direction) {
  console.log(`camera: ${direction}`);
}

function camRelease(btnId) {
  // visual feedback reset handled by :active; nothing extra needed
}

// ── Joystick ─────────────────────────────────────────────────
const ring = document.getElementById('joystick-ring');
const knob = document.getElementById('joystick-knob');

let joystickActive = false;
let currentLeft = 0;
let currentRight = 0;
let signalInterval = null;
let motorSocket = null;
let reconnectTimer = null;
let pageUnloading = false;

const SIGNAL_RATE_MS = 100; // emit signal every 100 ms while held
const RECONNECT_DELAY_MS = 1000;

const connectionDot = document.getElementById('connection-dot');
const connectionStatus = document.getElementById('connection-status');

function setConnectionStatus(connected, label) {
  connectionDot.classList.toggle('dot-offline', !connected);
  connectionStatus.classList.toggle('status-offline', !connected);
  connectionStatus.textContent = label;
}

function motorWebSocketUrl() {
  const scheme = window.location.protocol === 'https:' ? 'wss' : 'ws';
  return `${scheme}://${window.location.hostname}:8080/ws`;
}

function sendMotor(left, right) {
  const message = { type: 'motor', left, right };

  if (motorSocket && motorSocket.readyState === WebSocket.OPEN) {
    motorSocket.send(JSON.stringify(message));
  }

  console.log('motor', message);
}

function connectMotorWebSocket() {
  if (pageUnloading ||
      (motorSocket && (motorSocket.readyState === WebSocket.OPEN ||
                       motorSocket.readyState === WebSocket.CONNECTING))) {
    return;
  }

  setConnectionStatus(false, '連線中');
  motorSocket = new WebSocket(motorWebSocketUrl());

  motorSocket.addEventListener('open', () => {
    setConnectionStatus(true, '已連接');
    sendMotor(0, 0);
  });

motorSocket.addEventListener('message', (event) => {
  try {
    const data = JSON.parse(event.data);

    if (data.type === 'mq135') {
      updateAirQuality(data.quality);
    }
  } catch (error) {
    console.error('Invalid WebSocket data:', error);
  }
});

  motorSocket.addEventListener('close', () => {
    setConnectionStatus(false, '未連接');
    motorSocket = null;
    if (!pageUnloading && !reconnectTimer) {
      reconnectTimer = setTimeout(() => {
        reconnectTimer = null;
        connectMotorWebSocket();
      }, RECONNECT_DELAY_MS);
    }
  });

  motorSocket.addEventListener('error', () => {
    setConnectionStatus(false, '連線錯誤');
  });
}

function clamp(value, min, max) {
  return Math.max(min, Math.min(max, value));
}

function xyToWheelSpeed(x, y) {
  // x: -100 (left) to 100 (right)
  // y: -100 (backward) to 100 (forward)
  
  // Convert joystick position to differential drive
  // left wheel = forward/backward + turn adjustment
  // right wheel = forward/backward - turn adjustment
  
  let left = y + x;
  let right = y - x;
  
  // Clamp to -100 to 100 range
  left = clamp(left, -100, 100);
  right = clamp(right, -100, 100);
  
  return { left: Math.round(left), right: Math.round(right) };
}

function quantizeJoystick(dx, dy, maxR) {
  // Calculate normalized distance (-1 to 1 range)
  const normalizedX = dx / maxR;
  const normalizedY = dy / maxR;
  
  // Scale to -100 to 100 range
  let x = Math.round(normalizedX * 100);
  let y = Math.round(-normalizedY * 100); // Invert Y so up is positive
  
  // Clamp values
  x = clamp(x, -100, 100);
  y = clamp(y, -100, 100);
  
  // Dead zone threshold (about 12%)
  const threshold = 12;
  if (Math.abs(x) < threshold && Math.abs(y) < threshold) {
    return { x: 0, y: 0 };
  }
  
  return { x, y };
}

function startSignal(left, right) {
  if (left === currentLeft && right === currentRight) return;
  
  currentLeft = left;
  currentRight = right;
  
  sendMotor(left, right);
  
  clearInterval(signalInterval);
  if (left !== 0 || right !== 0) {
    signalInterval = setInterval(() => {
      sendMotor(left, right);
    }, SIGNAL_RATE_MS);
  }
  
  // Update visual feedback based on direction
  updateDirectionArrows(left, right);
}

function updateDirectionArrows(left, right) {
  ring.classList.remove('dir-up', 'dir-down', 'dir-left', 'dir-right');
  
  const threshold = 20;
  const avg = (left + right) / 2;
  const diff = left - right;
  
  if (Math.abs(avg) > Math.abs(diff)) {
    // Primarily forward/backward
    if (avg > threshold) ring.classList.add('dir-up');
    else if (avg < -threshold) ring.classList.add('dir-down');
  } else {
    // Primarily turning
    if (diff > threshold) ring.classList.add('dir-left');
    else if (diff < -threshold) ring.classList.add('dir-right');
  }
}

function stopSignal() {
  clearInterval(signalInterval);
  signalInterval = null;
  currentLeft = 0;
  currentRight = 0;
  sendMotor(0, 0);
  ring.classList.remove('dir-up', 'dir-down', 'dir-left', 'dir-right');
  knob.classList.remove('active');
  knob.style.transform = '';
}

function handleMove(clientX, clientY) {
  const rect = ring.getBoundingClientRect();
  const cx = rect.left + rect.width / 2;
  const cy = rect.top  + rect.height / 2;
  const dx = clientX - cx;
  const dy = clientY - cy;

  // clamp knob inside ring
  const maxR = rect.width / 2 - knob.offsetWidth / 2 - 4;
  const dist = Math.sqrt(dx * dx + dy * dy);
  const clampedDist = Math.min(dist, maxR);
  const angle = Math.atan2(dy, dx);
  const kx = Math.cos(angle) * clampedDist;
  const ky = Math.sin(angle) * clampedDist;
  knob.style.transform = `translate(${kx}px, ${ky}px)`;

  const coords = quantizeJoystick(kx, ky, maxR);
  const wheels = xyToWheelSpeed(coords.x, coords.y);
  startSignal(wheels.left, wheels.right);
}

// Touch events
knob.addEventListener('touchstart', (e) => {
  e.preventDefault();
  joystickActive = true;
  knob.classList.add('active');
}, { passive: false });

document.addEventListener('touchmove', (e) => {
  if (!joystickActive) return;
  e.preventDefault();
  const t = e.touches[0];
  handleMove(t.clientX, t.clientY);
}, { passive: false });

document.addEventListener('touchend', (e) => {
  if (!joystickActive) return;
  joystickActive = false;
  stopSignal();
});

document.addEventListener('touchcancel', () => {
  if (!joystickActive) return;
  joystickActive = false;
  stopSignal();
});

// Mouse events (for desktop testing)
knob.addEventListener('mousedown', (e) => {
  joystickActive = true;
  knob.classList.add('active');
});

document.addEventListener('mousemove', (e) => {
  if (!joystickActive) return;
  handleMove(e.clientX, e.clientY);
});

document.addEventListener('mouseup', (e) => {
  if (!joystickActive) return;
  joystickActive = false;
  stopSignal();
});

document.addEventListener('visibilitychange', () => {
  if (document.hidden)
    stopSignal();
});

window.addEventListener('pagehide', () => {
  pageUnloading = true;
  stopSignal();
  if (motorSocket && motorSocket.readyState === WebSocket.OPEN)
    motorSocket.close();
});

connectMotorWebSocket();

// ── Force landscape orientation ──────────────────────────────
if (screen.orientation && screen.orientation.lock) {
  screen.orientation.lock('landscape').catch((err) => {
    console.warn('Screen orientation lock not supported:', err);
  });
}

// ── Air Quality Control ──────────────────────────────────────
// Example function to update air quality
// Call updateAirQuality('good'), updateAirQuality('moderate'), or updateAirQuality('poor')
function updateAirQuality(level) {
  const elem = document.getElementById('air-quality');

  elem.classList.remove(
    'good',
    'normal',
    'bad',
    'unknown'
  );

  switch (level) {
    case 'GOOD':
      elem.classList.add('good');
      elem.textContent = '優良';
      break;

    case 'NORMAL':
      elem.classList.add('normal');
      elem.textContent = '普通';
      break;

    case 'BAD':
      elem.classList.add('bad');
      elem.textContent = '不佳';
      break;

    default:
      elem.classList.add('unknown');
      elem.textContent = '等待資料';
      break;
  }
}
// Example function to update light level
// Call updateLightLevel(500) with lux value
function updateLightLevel(lux) {
  const elem = document.getElementById('light');
  elem.textContent = `${lux} lux`;
}
