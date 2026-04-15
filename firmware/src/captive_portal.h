#pragma once
#include <pgmspace.h>

static const char CAPTIVE_PORTAL_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Now Playing — Wi-Fi Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
     background:#111;color:#eee;max-width:480px;margin:0 auto;padding:24px 16px}
h1{font-size:1.3rem;margin-bottom:6px;color:#fff}
p.sub{font-size:.85rem;color:#888;margin-bottom:24px}
label{display:block;font-size:.85rem;color:#aaa;margin-top:16px}
select,input[type=password]{width:100%;padding:10px 12px;margin-top:6px;
     background:#1a1a1a;border:1px solid #333;border-radius:6px;color:#eee;font-size:.9rem}
select:focus,input:focus{outline:none;border-color:#666}
button{width:100%;padding:12px;background:#2a6;border:none;border-radius:6px;
       color:#fff;font-size:1rem;cursor:pointer;margin-top:24px}
button:disabled{background:#333;color:#666;cursor:default}
.msg{padding:10px 14px;border-radius:6px;margin-top:16px;font-size:.85rem;display:none}
.msg.ok{display:block;background:#1a3a1a;color:#6d6}
.msg.err{display:block;background:#3a1a1a;color:#d66}
.spinner{display:inline-block;width:14px;height:14px;border:2px solid #666;
         border-top-color:#eee;border-radius:50%;animation:spin .7s linear infinite;
         vertical-align:middle;margin-right:6px}
@keyframes spin{to{transform:rotate(360deg)}}
.rssi{font-size:.75rem;color:#666;margin-left:4px}
</style>
</head>
<body>
<h1>&#127926; Now Playing Setup</h1>
<p class="sub">Connect to your home Wi-Fi network to get started.</p>

<label>Wi-Fi Network
  <select id="ssidSel" onchange="onNetSelect()">
    <option value="">— scanning… —</option>
  </select>
</label>

<label id="manualLabel" style="display:none">Or enter network name manually
  <input type="text" id="ssidManual" placeholder="Network name (SSID)" autocomplete="off" autocorrect="off" autocapitalize="none">
</label>

<label>Password
  <input type="password" id="pwd" placeholder="Wi-Fi password" autocomplete="current-password">
</label>

<button id="btn" onclick="doConnect()" disabled>Connect</button>
<div id="msg" class="msg"></div>

<script>
let networks = [];
let scanRetries = 0;

async function scanNetworks() {
  try {
    const r = await fetch('/api/wifi/scan');
    networks = await r.json();
    if (networks.length === 0 && scanRetries < 5) {
      // Scan still running or no results yet — retry after a short delay
      scanRetries++;
      document.getElementById('ssidSel').innerHTML = '<option value="">— scanning\u2026 (' + scanRetries + ') —</option>';
      setTimeout(scanNetworks, 2000);
      return;
    }
    const sel = document.getElementById('ssidSel');
    sel.innerHTML = '<option value="">— select a network —</option>'
      + networks.map(n =>
          '<option value="'+esc(n.ssid)+'">'
          + esc(n.ssid)
          + (n.open?'<span class="rssi"> (open)</span>':'')
          + '</option>'
        ).join('')
      + '<option value="__manual__">Other (enter manually)…</option>';
    document.getElementById('btn').disabled = false;
  } catch(e) {
    document.getElementById('ssidSel').innerHTML = '<option value="__manual__">Enter manually…</option>';
    document.getElementById('manualLabel').style.display = '';
    document.getElementById('btn').disabled = false;
  }
}

function onNetSelect() {
  const v = document.getElementById('ssidSel').value;
  const ml = document.getElementById('manualLabel');
  if (v === '__manual__') {
    ml.style.display = '';
    document.getElementById('ssidManual').focus();
  } else {
    ml.style.display = 'none';
  }
  // Pre-clear password for new selection
  document.getElementById('pwd').value = '';
}

async function doConnect() {
  const sel = document.getElementById('ssidSel').value;
  const ssid = (sel === '__manual__' || sel === '')
    ? document.getElementById('ssidManual').value.trim()
    : sel;
  const pwd = document.getElementById('pwd').value;
  const btn = document.getElementById('btn');
  const msg = document.getElementById('msg');

  if (!ssid) {
    msg.className = 'msg err';
    msg.textContent = 'Please select or enter a network name.';
    return;
  }

  btn.disabled = true;
  btn.innerHTML = '<span class="spinner"></span>Connecting…';
  msg.className = 'msg';

  try {
    const r = await fetch('/api/wifi/save', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({ssid, password: pwd})
    });
    const d = await r.json();
    if (r.ok && d.ok) {
      msg.className = 'msg ok';
      msg.textContent = 'Credentials saved! The device is restarting and will join "' + ssid + '". You can close this page.';
      btn.textContent = 'Done';
    } else {
      throw new Error(d.error || 'Save failed');
    }
  } catch(e) {
    msg.className = 'msg err';
    msg.textContent = 'Error: ' + e.message;
    btn.disabled = false;
    btn.textContent = 'Connect';
  }
}

function esc(s) {
  const d = document.createElement('div');
  d.textContent = s || '';
  return d.innerHTML;
}

scanNetworks();
</script>
</body>
</html>
)rawliteral";
