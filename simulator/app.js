const $ = (id) => document.getElementById(id);
let busy = false;
let frame = null;
let live = false,
  previewPending = null,
  previewTimer = null;
const previewGap = 25;
let lastPreviewFrame = 0,
  previewIntervals = [];

async function request(path, payload = null) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), 120000);
  try {
    const response = await fetch(path, {
      method: 'POST',
      signal: controller.signal,
      ...(payload ? { headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) } : {}),
    });
    const result = await response.json();
    if (!response.ok) throw new Error(result.error || 'Device request failed.');
    return result;
  } finally {
    clearTimeout(timer);
  }
}
function controls() {
  document.querySelectorAll('button').forEach((button) => (button.disabled = busy));
  $('removeKey').disabled = busy || !frame?.keys?.[keyTarget()];
  $('prev').disabled = busy || !frame || frame.view !== 'answer' || frame.page <= 1;
  $('next').disabled = busy || !frame || frame.view !== 'answer' || frame.page >= frame.pages;
}
async function operation(action) {
  if (busy) return;
  busy = true;
  lastPreviewFrame = 0;
  previewIntervals = [];
  controls();
  if (live) $('liveInfo').textContent = 'Preview paused while the device works...';
  $('error').hidden = true;
  $('transport').textContent = 'Waiting for device...';
  try {
    if (previewPending) await previewPending;
    await action();
    $('transport').textContent = 'Device frame verified';
  } catch (error) {
    $('error').hidden = false;
    $('error').textContent = error.message;
    $('transport').textContent = 'Device request failed';
  } finally {
    busy = false;
    controls();
    if (live) schedulePreview();
  }
}
async function display(result) {
  const image = new Image();
  await new Promise((resolve, reject) => {
    image.onload = resolve;
    image.onerror = () => reject(new Error('Screen image could not be decoded.'));
    image.src = result.image;
  });
  // Display the board's pixels unchanged: no local text layout or UI state machine.
  const canvas = $('screen'),
    context = canvas.getContext('2d');
  canvas.width = image.naturalWidth;
  canvas.height = image.naturalHeight;
  context.drawImage(image, 0, 0);
  frame = result;

  live = Boolean(result.live);
  $('liveToggle').textContent = live ? 'Stop preview' : 'Start preview';
  if (!live) clearTimeout(previewTimer);
  for (const [id, value] of [
    ['providerGemini', 'gemini'],
    ['providerGpt', 'gpt'],
  ])
    $(id).setAttribute('aria-pressed', String(result.provider === value));
  if (!keyTargetChosen && result.provider) $('keyProvider').value = result.provider;
  keyStatus();
  $('providerInfo').textContent = result.provider
    ? (result.provider === 'gemini' ? 'Gemini' : 'GPT') + ' / requests run on ESP32'
    : 'Update firmware to select an AI provider.';
  $('connection').textContent = 'ESP32 connected / ' + result.port;
  $('captureInfo').textContent =
    'Frame ' +
    result.sequence +
    ' / ' +
    image.naturalWidth +
    ' x ' +
    image.naturalHeight +
    ' / RGB565 / checksum ' +
    result.checksum;
  $('screenDescription').textContent =
    'Device: ' + result.view + (result.view === 'answer' ? ' / page ' + result.page + ' of ' + result.pages : '');
  canvas.setAttribute('aria-label', $('screenDescription').textContent + '. Pixels rendered on ESP32.');
}
function button(name) {
  return operation(async () => {
    const result = await request('/api/button/' + name);
    await display(result);
    if (name === 'capture') {
      $('originalImage').hidden = true;
      $('downloadPhoto').hidden = true;
      $('originalInfo').textContent = 'Open Original photo to inspect this new capture.';
    }
  });
}
$('capture').onclick = () => button('capture');
$('prev').onclick = () => button('up');
$('next').onclick = () => button('down');
$('home').onclick = () => button('home');
$('viewPhoto').onclick = () => button('photo');
$('demo').onclick = () => button('demo');
$('deviceStatus').onclick = () => button('status');
$('rotatePhoto').onclick = () => button('rotate');
$('refresh').onclick = () => operation(async () => display(await request('/api/screen')));
async function diagnostic(path) {
  const result = await request(path);
  $('log').textContent = result.log;
  $('connection').textContent = 'ESP32 connected / ' + result.port;
  await display(await request('/api/screen'));
}
$('status').onclick = () => operation(() => diagnostic('/api/status'));
$('campus').onclick = () => operation(() => diagnostic('/api/wifi/campus'));
$('hotspot').onclick = () => operation(() => diagnostic('/api/wifi/hotspot'));
$('nativeSize').onchange = () => {
  $('screen').classList.toggle('native-size', $('nativeSize').checked);
};
document.addEventListener('keydown', (event) => {
  if (['INPUT', 'TEXTAREA'].includes(document.activeElement.tagName) || busy) return;
  const keys = { ArrowUp: 'prev', ArrowDown: 'next', ' ': 'capture' };
  if (keys[event.key] && !$(keys[event.key]).disabled) {
    event.preventDefault();
    $(keys[event.key]).click();
  }
});
controls();
operation(async () => display(await request('/api/screen')));

$('original').onclick = () =>
  operation(async () => {
    const result = await request('/api/original');
    const image = $('originalImage');
    image.src = result.image;
    image.hidden = false;
    $('downloadPhoto').href = result.image;
    $('downloadPhoto').hidden = false;
    image.onload = () => {
      $('originalInfo').textContent =
        image.naturalWidth +
        ' x ' +
        image.naturalHeight +
        ' / ' +
        Math.round(result.bytes / 1024) +
        ' KB / original JPEG';
    };
  });

function schedulePreview() {
  clearTimeout(previewTimer);
  if (live && !busy && document.visibilityState !== 'hidden') previewTimer = setTimeout(pollPreview, previewGap);
}
async function pollPreview() {
  if (!live || busy || document.visibilityState === 'hidden') return;
  previewPending = (async () => {
    try {
      const result = await request('/api/preview');
      const image = new Image();
      await new Promise((resolve, reject) => {
        image.onload = resolve;
        image.onerror = () => reject(new Error('Preview image failed.'));
        image.src = result.image;
      });
      if (live && frame?.view === 'home') {
        // Display the raw camera stream in the device's preview rectangle.
        // UI state, hit regions, typography and capture remain on the ESP32.
        $('screen').getContext('2d').drawImage(image, 0, 32, 240, 180);
      }
      const arrived = performance.now();
      if (lastPreviewFrame) {
        previewIntervals.push(arrived - lastPreviewFrame);
        if (previewIntervals.length > 10) previewIntervals.shift();
      }
      lastPreviewFrame = arrived;
      const rate = previewIntervals.length
        ? ((1000 * previewIntervals.length) / previewIntervals.reduce((a, b) => a + b, 0)).toFixed(1) + ' fps'
        : 'warming up';
      $('liveInfo').textContent = 'Live / 320 x 240 / ' + rate + ' / Capture saves full resolution';
    } catch (error) {
      live = false;
      $('liveToggle').textContent = 'Start preview';
      $('liveToggle').setAttribute('aria-pressed', 'false');
      $('liveInfo').textContent = 'Preview paused: ' + error.message;
    }
  })();
  await previewPending;
  previewPending = null;
  schedulePreview();
}
$('liveToggle').onclick = () => operation(async () => display(await request('/api/touch', { x: 120, y: 140 })));

let touchStart = null;
$('screen').style.touchAction = 'none';
$('screen').addEventListener('pointerdown', (event) => {
  if (busy) return;
  touchStart = {
    x: event.clientX,
    y: event.clientY,
    screenY:
      ((event.clientY - $('screen').getBoundingClientRect().top) * 284) / $('screen').getBoundingClientRect().height,
  };
  $('screen').setPointerCapture(event.pointerId);
});
$('screen').addEventListener('pointercancel', () => {
  touchStart = null;
});
$('screen').addEventListener('pointerup', (event) => {
  if (!touchStart) return;
  const dy = event.clientY - touchStart.y,
    dx = event.clientX - touchStart.x,
    touchStartY = touchStart.screenY;
  touchStart = null;
  if (busy) return;
  if (Math.abs(dy) > 35 && Math.abs(dy) > Math.abs(dx)) {
    if (dy < 0 && touchStartY >= 250) button('home');
    else if (frame?.view === 'answer') button(dy < 0 ? 'down' : 'up');
    return;
  }
  const bounds = $('screen').getBoundingClientRect();
  const x = Math.max(0, Math.min(239, Math.floor(((event.clientX - bounds.left) * 240) / bounds.width)));
  const y = Math.max(0, Math.min(283, Math.floor(((event.clientY - bounds.top) * 284) / bounds.height)));
  operation(async () => {
    const result = await request('/api/touch', { x, y });
    await display(result);
    if (result.view === 'photo') {
      $('originalImage').hidden = true;
      $('downloadPhoto').hidden = true;
    }
  });
});

$('providerGemini').onclick = () => button('provider/gemini');
$('providerGpt').onclick = () => button('provider/gpt');

// Which key the form saves: chosen independently of the device's active provider, so
// adding a paid key never switches the device to it.
let keyTargetChosen = false;
function keyTarget() {
  return $('keyProvider').value;
}
function keyStatus() {
  const selected = keyTarget() === 'gpt' ? 'GPT' : 'Gemini';
  $('keyLabel').textContent = selected + ' API key';
  $('keyStatus').textContent = frame?.keys?.[keyTarget()]
    ? selected + ' key saved on device'
    : 'No key saved for ' + selected;
  $('removeKey').disabled = busy || !frame?.keys?.[keyTarget()];
}
$('keyProvider').onchange = () => {
  keyTargetChosen = true;
  keyStatus();
};
$('keyForm').onsubmit = (event) => {
  event.preventDefault();
  const provider = keyTarget(),
    key = $('apiKey').value.trim();
  if (!provider || !key) {
    $('error').hidden = false;
    $('error').textContent = 'Select a provider and paste its API key first.';
    return;
  }
  operation(async () => {
    await display(await request('/api/ai/key', { provider, key }));
    $('apiKey').value = '';
  });
};
$('removeKey').onclick = () => {
  const provider = keyTarget();
  if (provider)
    operation(async () => {
      await display(await request('/api/ai/key', { provider, key: '' }));
      $('apiKey').value = '';
    });
};

document.addEventListener('visibilitychange', () => {
  clearTimeout(previewTimer);
  if (document.visibilityState !== 'hidden') {
    lastPreviewFrame = 0;
    previewIntervals = [];

    if (frame) schedulePreview();
    else operation(async () => display(await request('/api/screen')));
  }
});
