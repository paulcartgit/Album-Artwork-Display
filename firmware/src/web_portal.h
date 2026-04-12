#pragma once
#include <pgmspace.h>

static const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Vinyl Display</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
     background:#111;color:#eee;max-width:600px;margin:0 auto;padding:16px}
h1{font-size:1.3rem;margin-bottom:12px;color:#fff}
h2{font-size:1.1rem;margin:16px 0 8px;color:#ccc}
.tabs{display:flex;gap:4px;margin-bottom:16px}
.tab{padding:8px 16px;background:#222;border:1px solid #333;border-radius:6px;
     color:#aaa;cursor:pointer;font-size:.9rem}
.tab.active{background:#333;color:#fff;border-color:#555}
.panel{display:none}
.panel.active{display:block}
.status-card{background:#1a1a1a;border:1px solid #333;border-radius:8px;padding:16px;margin-bottom:12px}
.status-label{font-size:.75rem;color:#888;text-transform:uppercase;letter-spacing:.05em}
.status-value{font-size:1.1rem;margin-top:2px}
label{display:block;font-size:.85rem;color:#aaa;margin-top:12px}
input[type=text],input[type=number],input[type=password]{width:100%;padding:8px 10px;margin-top:4px;
     background:#1a1a1a;border:1px solid #333;border-radius:6px;color:#eee;font-size:.9rem}
input:focus{outline:none;border-color:#666}
button{padding:10px 20px;background:#2a6;border:none;border-radius:6px;color:#fff;
       font-size:.9rem;cursor:pointer;margin-top:16px}
button:active{background:#185}
button.danger{background:#a33}
button.danger:active{background:#722}
.gallery-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(120px,1fr));gap:8px;margin-top:12px}
.gallery-item{background:#1a1a1a;border:1px solid #333;border-radius:6px;padding:8px;
              text-align:center;font-size:.75rem;color:#aaa;word-break:break-all;position:relative}
.gallery-item .del{position:absolute;top:4px;right:4px;background:#a33;color:#fff;
                   border:none;border-radius:50%;width:22px;height:22px;cursor:pointer;font-size:.7rem}
.upload-area{border:2px dashed #333;border-radius:8px;padding:24px;text-align:center;
             color:#666;margin-top:12px;cursor:pointer}
.upload-area.hover{border-color:#2a6;color:#2a6}
#uploadInput{display:none}
.msg{padding:8px 12px;border-radius:6px;margin-top:8px;font-size:.85rem;display:none}
.msg.ok{display:block;background:#1a3a1a;color:#6d6}
.msg.err{display:block;background:#3a1a1a;color:#d66}
.scan-item{background:#1a1a1a;border:1px solid #333;border-radius:6px;padding:8px 12px;
           cursor:pointer;margin-top:4px;display:flex;justify-content:space-between;align-items:center}
.scan-item:hover{border-color:#555;background:#222}
</style>
</head>
<body>
<h1>&#127926; Vinyl Display</h1>

<div class="tabs">
  <div class="tab active" onclick="showTab('status')">Status</div>
  <div class="tab" onclick="showTab('settings')">Settings</div>
  <div class="tab" onclick="showTab('wifi')">WiFi</div>
  <div class="tab" onclick="showTab('gallery')">Gallery</div>
</div>

<!-- STATUS -->
<div id="status" class="panel active">
  <div class="status-card">
    <div class="status-label">State</div>
    <div class="status-value" id="sState">—</div>
  </div>
  <div class="status-card">
    <div class="status-label">Now Playing</div>
    <div class="status-value" id="sTrack">—</div>
  </div>
  <div class="status-card">
    <div class="status-label">IP Address</div>
    <div class="status-value" id="sIP">—</div>
  </div>
  <div class="status-card">
    <div class="status-label">Uptime</div>
    <div class="status-value" id="sUptime">—</div>
  </div>
  <button onclick="loadStatus()">Refresh</button>
</div>

<!-- SETTINGS -->
<div id="settings" class="panel">
  <h2>Sonos</h2>
  <label>Speaker IP<input type="text" id="fSonosIp" placeholder="192.168.1.x"></label>
  <button onclick="scanSonos()" id="btnSonosScan" style="margin-top:8px">Scan for Speakers</button>
  <div id="sonosList" style="margin-top:4px;display:none"></div>

  <h2>ACRCloud</h2>
  <label>Host<input type="text" id="fAcrHost" placeholder="identify-eu-west-1.acrcloud.com"></label>
  <label>Access Key<input type="text" id="fAcrKey"></label>
  <label>Access Secret<input type="text" id="fAcrSecret" placeholder="leave blank to keep current"></label>

  <h2>Spotify</h2>
  <label>Client ID<input type="text" id="fSpotifyId"></label>
  <label>Client Secret<input type="text" id="fSpotifySecret" placeholder="leave blank to keep current"></label>

  <h2>Google Photos</h2>
  <label>Bridge URL<input type="text" id="fPhotosUrl" placeholder="https://script.google.com/..."></label>

  <h2>Polling</h2>
  <label>Interval (ms)<input type="number" id="fPollMs" min="10000" step="1000"></label>

  <button onclick="saveSettings()">Save Settings</button>
  <div id="settingsMsg" class="msg"></div>
</div>

<!-- GALLERY -->
<div id="gallery" class="panel">
  <div class="upload-area" id="dropZone" onclick="document.getElementById('uploadInput').click()">
    Tap or drop an image here to upload
  </div>
  <input type="file" id="uploadInput" accept="image/jpeg,image/png">
  <div id="uploadMsg" class="msg"></div>
  <div class="gallery-grid" id="galleryGrid"></div>
</div>

<!-- WIFI -->
<div id="wifi" class="panel">
  <div class="status-card">
    <div class="status-label">Connected Network</div>
    <div class="status-value" id="wSSID">—</div>
  </div>
  <h2>Change Network</h2>
  <label>Network Name (SSID)<input type="text" id="fWifiSsid" placeholder="Enter SSID or scan below"></label>
  <label>Password<input type="password" id="fWifiPass" placeholder="WiFi password"></label>
  <div style="display:flex;gap:8px;flex-wrap:wrap">
    <button onclick="scanWifi()" id="btnWifiScan">Scan Networks</button>
    <button onclick="saveWifi()">Save &amp; Reconnect</button>
  </div>
  <div id="wifiNetworks" style="margin-top:8px;display:none">
    <div style="font-size:.8rem;color:#888;margin-bottom:4px">Tap a network to select:</div>
    <div id="wifiList"></div>
  </div>
  <div id="wifiMsg" class="msg"></div>
  <p style="font-size:.75rem;color:#555;margin-top:12px">After saving, the device will reconnect. You may need to wait a moment then access <strong>vinyl.local</strong> again.</p>
</div>

<script>
const STATES = ['BOOT','IDLE','DIGITAL','VINYL','ERROR'];

function showTab(name) {
  document.querySelectorAll('.tab').forEach((t,i) => t.classList.toggle('active', t.textContent.toLowerCase()===name));
  document.querySelectorAll('.panel').forEach(p => p.classList.toggle('active', p.id===name));
  if (name==='status') loadStatus();
  if (name==='settings') loadSettings();
  if (name==='gallery') loadGallery();
  if (name==='wifi') loadWifi();
}

async function loadStatus() {
  try {
    const r = await fetch('/api/status');
    const d = await r.json();
    document.getElementById('sState').textContent = STATES[d.state] || d.state;
    document.getElementById('sTrack').textContent = (d.artist && d.title) ? d.artist+' — '+d.title : 'Nothing';
    document.getElementById('sIP').textContent = d.ip;
    const m = Math.floor(d.uptime/60), h = Math.floor(m/60);
    document.getElementById('sUptime').textContent = h+'h '+m%60+'m';
  } catch(e) { console.error(e); }
}

async function loadSettings() {
  try {
    const r = await fetch('/api/settings');
    const d = await r.json();
    document.getElementById('fSonosIp').value = d.sonos_ip||'';
    document.getElementById('fAcrHost').value = d.acrcloud_host||'';
    document.getElementById('fAcrKey').value = d.acrcloud_key||'';
    document.getElementById('fAcrSecret').value = '';
    document.getElementById('fAcrSecret').placeholder = d.acrcloud_secret_set ? '(set — leave blank to keep)' : '';
    document.getElementById('fSpotifyId').value = d.spotify_client_id||'';
    document.getElementById('fSpotifySecret').value = '';
    document.getElementById('fSpotifySecret').placeholder = d.spotify_client_secret_set ? '(set — leave blank to keep)' : '';
    document.getElementById('fPhotosUrl').value = d.google_photos_url||'';
    document.getElementById('fPollMs').value = d.poll_interval_ms||45000;
  } catch(e) { console.error(e); }
}

async function saveSettings() {
  const body = {
    sonos_ip: document.getElementById('fSonosIp').value,
    acrcloud_host: document.getElementById('fAcrHost').value,
    acrcloud_key: document.getElementById('fAcrKey').value,
    spotify_client_id: document.getElementById('fSpotifyId').value,
    google_photos_url: document.getElementById('fPhotosUrl').value,
    poll_interval_ms: parseInt(document.getElementById('fPollMs').value)||45000
  };
  const sec = document.getElementById('fAcrSecret').value;
  if (sec) body.acrcloud_secret = sec;
  const sps = document.getElementById('fSpotifySecret').value;
  if (sps) body.spotify_client_secret = sps;

  const el = document.getElementById('settingsMsg');
  try {
    const r = await fetch('/api/settings', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(body)});
    el.className = r.ok ? 'msg ok' : 'msg err';
    el.textContent = r.ok ? 'Saved!' : 'Error saving';
  } catch(e) { el.className='msg err'; el.textContent=e.message; }
  setTimeout(()=>el.style.display='none',3000);
}

async function loadGallery() {
  const grid = document.getElementById('galleryGrid');
  try {
    const r = await fetch('/api/gallery');
    const files = await r.json();
    grid.innerHTML = '';
    files.forEach(f => {
      const div = document.createElement('div');
      div.className = 'gallery-item';
      div.innerHTML = '<button class="del" onclick="delImg(\''+f.name+'\')">&times;</button>'
                    + '<div>'+f.name+'</div><div style="color:#555">'+Math.round(f.size/1024)+'KB</div>';
      grid.appendChild(div);
    });
  } catch(e) { console.error(e); }
}

async function delImg(name) {
  if (!confirm('Delete '+name+'?')) return;
  await fetch('/api/gallery/delete', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'name='+encodeURIComponent(name)});
  loadGallery();
}

// Upload
const dropZone = document.getElementById('dropZone');
const uploadInput = document.getElementById('uploadInput');
dropZone.addEventListener('dragover', e=>{e.preventDefault();dropZone.classList.add('hover');});
dropZone.addEventListener('dragleave', ()=>dropZone.classList.remove('hover'));
dropZone.addEventListener('drop', e=>{e.preventDefault();dropZone.classList.remove('hover');if(e.dataTransfer.files.length)uploadFile(e.dataTransfer.files[0]);});
uploadInput.addEventListener('change', ()=>{if(uploadInput.files.length)uploadFile(uploadInput.files[0]);});

async function uploadFile(file) {
  const el = document.getElementById('uploadMsg');
  el.className='msg ok'; el.textContent='Uploading...';
  try {
    const fd = new FormData();
    fd.append('file', file, file.name);
    const r = await fetch('/api/upload', {method:'POST', body:fd});
    el.className = r.ok ? 'msg ok' : 'msg err';
    el.textContent = r.ok ? 'Uploaded!' : 'Upload failed';
    loadGallery();
  } catch(e) { el.className='msg err'; el.textContent=e.message; }
}

function escHtml(s) {
  return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}

async function loadWifi() {
  try {
    const r = await fetch('/api/wifi');
    const d = await r.json();
    document.getElementById('wSSID').textContent = d.ssid || '—';
    document.getElementById('fWifiSsid').value = d.ssid || '';
  } catch(e) { console.error(e); }
}

async function scanWifi() {
  const btn = document.getElementById('btnWifiScan');
  const list = document.getElementById('wifiList');
  const container = document.getElementById('wifiNetworks');
  btn.textContent = 'Scanning...';
  btn.disabled = true;
  container.style.display = 'block';
  list.innerHTML = '<div style="color:#888;font-size:.8rem;padding:8px">Scanning...</div>';
  try {
    const r = await fetch('/api/wifi/scan');
    const nets = await r.json();
    list.innerHTML = '';
    if (!nets.length) {
      list.innerHTML = '<div style="color:#888;font-size:.8rem;padding:8px">No networks found</div>';
    } else {
      nets.sort((a,b) => b.rssi - a.rssi);
      nets.forEach(n => {
        const div = document.createElement('div');
        div.className = 'scan-item';
        const sig = n.rssi > -60 ? '▊▊▊' : n.rssi > -75 ? '▊▊░' : '▊░░';
        div.innerHTML = '<span>' + escHtml(n.ssid) + '</span>'
                      + '<span style="color:#888;font-size:.8rem">' + sig + (n.secure ? ' 🔒' : '') + '</span>';
        div.onclick = () => {
          document.getElementById('fWifiSsid').value = n.ssid;
          document.getElementById('fWifiPass').focus();
          document.querySelectorAll('#wifiList .scan-item').forEach(el => el.style.borderColor = '');
          div.style.borderColor = '#2a6';
        };
        list.appendChild(div);
      });
    }
  } catch(e) {
    list.innerHTML = '<div style="color:#d66;font-size:.8rem;padding:8px">Scan failed: ' + escHtml(e.message) + '</div>';
  }
  btn.textContent = 'Scan Networks';
  btn.disabled = false;
}

async function saveWifi() {
  const ssid = document.getElementById('fWifiSsid').value.trim();
  const pass = document.getElementById('fWifiPass').value;
  const el = document.getElementById('wifiMsg');
  if (!ssid) { el.className = 'msg err'; el.textContent = 'SSID is required'; return; }
  try {
    const r = await fetch('/api/wifi', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({ssid, password:pass})});
    el.className = r.ok ? 'msg ok' : 'msg err';
    el.textContent = r.ok ? 'Saved! Device is reconnecting...' : 'Error saving';
  } catch(e) { el.className='msg err'; el.textContent=e.message; }
}

async function scanSonos() {
  const btn = document.getElementById('btnSonosScan');
  const list = document.getElementById('sonosList');
  btn.textContent = 'Scanning...';
  btn.disabled = true;
  list.style.display = 'block';
  list.innerHTML = '<div style="color:#888;font-size:.8rem;padding:8px">Scanning for Sonos speakers...</div>';
  try {
    const r = await fetch('/api/sonos/scan');
    const devices = await r.json();
    list.innerHTML = '';
    if (!devices.length) {
      list.innerHTML = '<div style="color:#888;font-size:.8rem;padding:8px">No Sonos speakers found</div>';
    } else {
      devices.forEach(d => {
        const div = document.createElement('div');
        div.className = 'scan-item';
        div.innerHTML = '<span>' + escHtml(d.name) + '</span>'
                      + '<span style="color:#888;font-size:.8rem">' + escHtml(d.ip) + '</span>';
        div.onclick = () => {
          document.getElementById('fSonosIp').value = d.ip;
          document.querySelectorAll('#sonosList .scan-item').forEach(el => el.style.borderColor = '');
          div.style.borderColor = '#2a6';
        };
        list.appendChild(div);
      });
    }
  } catch(e) {
    list.innerHTML = '<div style="color:#d66;font-size:.8rem;padding:8px">Scan failed: ' + escHtml(e.message) + '</div>';
  }
  btn.textContent = 'Scan for Speakers';
  btn.disabled = false;
}

loadStatus();
</script>
</body>
</html>
)rawliteral";
