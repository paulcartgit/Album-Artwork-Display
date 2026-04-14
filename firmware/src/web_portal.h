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
input[type=text],input[type=number]{width:100%;padding:8px 10px;margin-top:4px;
     background:#1a1a1a;border:1px solid #333;border-radius:6px;color:#eee;font-size:.9rem}
input:focus{outline:none;border-color:#666}
button{padding:10px 20px;background:#2a6;border:none;border-radius:6px;color:#fff;
       font-size:.9rem;cursor:pointer;margin-top:16px}
button:active{background:#185}
button.danger{background:#a33}
button.danger:active{background:#722}
.listen-btn{display:block;width:100%;padding:14px;background:#2980b9;border:none;border-radius:8px;
            color:#fff;font-size:1rem;cursor:pointer;margin:12px 0;text-align:center}
.listen-btn:active{background:#1a5276}
.listen-btn:disabled{background:#333;color:#666;cursor:default}
.debug-btn{padding:8px 14px;background:#333;border:1px solid #444;border-radius:6px;color:#aaa;
           font-size:.85rem;cursor:pointer;margin-right:8px;margin-top:8px}
.debug-btn:active{background:#444}
.gallery-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(130px,1fr));gap:8px;margin-top:8px}
.section-header{font-size:.75rem;font-weight:600;color:#aaa;text-transform:uppercase;letter-spacing:.07em;padding:10px 0 4px;border-bottom:1px solid #333;margin-bottom:4px;display:flex;align-items:center;gap:6px}
.section-header+.gallery-grid{margin-top:8px}
.gallery-item{background:#1a1a1a;border:1px solid #333;border-radius:6px;
              text-align:center;font-size:.75rem;color:#aaa;word-break:break-all;position:relative;overflow:hidden}
.msg{padding:8px 12px;border-radius:6px;margin-top:8px;font-size:.85rem;display:none}
.msg.ok{display:block;background:#1a3a1a;color:#6d6}
.msg.err{display:block;background:#3a1a1a;color:#d66}
</style>
</head>
<body>
<h1>&#127926; Vinyl Display</h1>

<div class="tabs">
  <div class="tab active" onclick="showTab('status')">Now Playing</div>
  <div class="tab" onclick="showTab('settings')">Settings</div>
  <div class="tab" onclick="showTab('history')">History</div>
  <div class="tab" onclick="showTab('debug')">Debug</div>
</div>

<!-- NOW PLAYING -->
<div id="status" class="panel active">
  <div class="status-card" style="text-align:center">
    <div class="status-label" id="sStateLabel">Idle</div>
    <img id="sArt" style="display:none;max-width:100%;border-radius:6px;margin:12px auto 8px">
    <div class="status-value" id="sTrack" style="font-size:1.2rem">Nothing playing</div>
    <div id="sAlbum" style="font-size:.9rem;color:#999;margin-top:2px"></div>
  </div>
  <div class="status-card" id="scheduleCard" style="display:none">
    <div class="status-label">Next check</div>
    <div class="status-value" id="sSchedule" style="font-size:.9rem;color:#bbb"></div>
  </div>
  <div style="display:flex;gap:8px;margin:12px 0">
    <button class="listen-btn" style="flex:1;margin:0" onclick="forceListen()">&#127911; Listen</button>
    <button class="listen-btn" style="flex:1;margin:0;background:#2a6" onclick="forceRefresh()">&#x1f504; Check Sonos</button>
  </div>
  <div style="display:flex;gap:8px;text-align:center;font-size:.7rem;color:#666;margin-top:-4px;margin-bottom:12px">
    <div style="flex:1">Identify what's playing in the room</div>
    <div style="flex:1">Re-check Sonos for track info</div>
  </div>
  <div class="status-card" style="margin-top:12px">
    <div class="status-label">Activity Log</div>
    <div id="sLog" style="font-family:monospace;font-size:.75rem;color:#aaa;max-height:300px;overflow-y:auto;margin-top:8px;line-height:1.6"></div>
  </div>
</div>

<!-- SETTINGS -->
<div id="settings" class="panel">
  <h2>Sonos</h2>
  <label>Speaker IP<input type="text" id="fSonosIp" placeholder="192.168.1.x"></label>

  <h2>Shazam (RapidAPI)</h2>
  <label>API Key<input type="text" id="fShazamKey" placeholder="your-rapidapi-key"></label>

  <h2>Timing</h2>
  <label>Sonos check interval
    <div style="display:flex;align-items:center;gap:8px">
      <input type="range" id="fSonosPoll" min="5" max="60" style="flex:1">
      <span id="fSonosPollVal" style="min-width:3em;color:#eee"></span>
    </div>
  </label>
  <label>Vinyl re-identify interval
    <div style="display:flex;align-items:center;gap:8px">
      <input type="range" id="fVinylRecheck" min="1" max="30" style="flex:1">
      <span id="fVinylRecheckVal" style="min-width:3em;color:#eee"></span>
    </div>
  </label>
  <label>No-match cooldown
    <div style="display:flex;align-items:center;gap:8px">
      <input type="range" id="fCooldown" min="1" max="15" style="flex:1">
      <span id="fCooldownVal" style="min-width:3em;color:#eee"></span>
    </div>
  </label>
  <label>Idle gallery rotation
    <div style="display:flex;align-items:center;gap:8px">
      <input type="range" id="fIdleGallery" min="1" max="30" style="flex:1">
      <span id="fIdleGalleryVal" style="min-width:3em;color:#eee"></span>
    </div>
  </label>

  <h2>Display</h2>
  <label style="margin-top:8px;display:flex;align-items:center;gap:8px">
    <input type="checkbox" id="fShowTrackInfo" style="width:auto;margin:0">
    Show track info overlay on display
  </label>
  <label style="margin-top:8px">Background fill
    <select id="fBgMode">
      <option value="2">Auto (smart detection)</option>
      <option value="1">Always blurred</option>
      <option value="0">Always solid colour</option>
    </select>
  </label>

  <button onclick="saveSettings()">Save Settings</button>
  <div id="settingsMsg" class="msg"></div>
</div>

<!-- HISTORY -->
<div id="history" class="panel">
  <p style="font-size:.8rem;color:#888;margin-bottom:12px">Album covers are saved automatically as you play music. Toggle covers on/off to control what shows when idle. Pin &#128204; a cover to keep it in rotation permanently — pinned covers are never removed when the history reaches 100 entries.</p>
  <div id="pinnedSection" style="display:none">
    <div class="section-header">&#128204; Pinned</div>
    <div class="gallery-grid" id="pinnedGrid"></div>
  </div>
  <div id="historySection" style="display:none">
    <div class="section-header" id="historyHeader">History</div>
    <div class="gallery-grid" id="historyGrid"></div>
  </div>
  <div id="historyEmpty" style="text-align:center;color:#666;padding:32px;display:none">No album art yet — play some music!</div>
</div>

<!-- DEBUG -->
<div id="debug" class="panel">
  <div class="status-card">
    <div class="status-label">Device Info</div>
    <div class="status-value" style="font-size:.9rem">
      <span id="dIP">—</span> &middot; Uptime: <span id="dUptime">—</span>
    </div>
  </div>
  <button class="debug-btn" onclick="forceRefresh()">Force Display Refresh</button>
  <button class="debug-btn" onclick="testColors()">Test Color Pattern</button>
  <a href="/api/last-audio" download="recording.wav"><button type="button" class="debug-btn">Download Last Audio</button></a>
  <p style="color:#666;font-size:.75rem;margin-top:16px">
    <b>Force Display Refresh</b> — re-fetches artwork and redraws the e-ink display.<br>
    <b>Test Color Pattern</b> — shows 6 color bands to verify all e-ink pigments.<br>
    <b>Download Last Audio</b> — saves the most recent recording as a WAV file.
  </p>
</div>

<script>
const STATES = ['BOOT','IDLE','DIGITAL','VINYL','ERROR'];
function fmtSec(s) {
  if (s==null) return null;
  if (s < 60) return s+'s';
  const m = Math.floor(s/60), rs = s%60;
  return m+'m '+rs+'s';
}

function showTab(name) {
  document.querySelectorAll('.tab').forEach(t => {
    const tabName = t.textContent.toLowerCase().replace(' ','');
    const map = {'nowplaying':'status','settings':'settings','history':'history','debug':'debug'};
    t.classList.toggle('active', map[tabName]===name);
  });
  document.querySelectorAll('.panel').forEach(p => p.classList.toggle('active', p.id===name));
  if (name==='status') loadStatus();
  if (name==='settings') loadSettings();
  if (name==='history') loadHistory();
  if (name==='debug') loadDebug();
}

async function loadStatus() {
  try {
    const r = await fetch('/api/status');
    const d = await r.json();
    const state = d.state_name || STATES[d.state] || 'Unknown';
    const hasTrack = d.artist && d.title;
    const labels = {IDLE:'Idle',VINYL:'Listening to Vinyl',DIGITAL:'Playing Digital',BOOT:'Starting up',ERROR:'Error'};
    document.getElementById('sStateLabel').textContent = labels[state] || state;
    document.getElementById('sTrack').textContent = hasTrack ? d.artist+' — '+d.title : 'Nothing playing';
    document.getElementById('sAlbum').textContent = d.album || '';
    // Artwork
    const artEl = document.getElementById('sArt');
    if (d.art_url) { artEl.src = d.art_url; artEl.style.display = ''; }
    else { artEl.style.display = 'none'; artEl.removeAttribute('src'); }
    let sched = [];
    if (d.next_poll_sec != null) sched.push('Sonos check in '+fmtSec(d.next_poll_sec));
    if (d.next_vinyl_check_sec != null) sched.push('Vinyl re-identify in '+fmtSec(d.next_vinyl_check_sec));
    if (d.retry_in_sec != null) sched.push('&#x1f504; Retry '+d.no_match_retries+'/3 in '+fmtSec(d.retry_in_sec));
    if (d.cooldown_remaining_sec != null) sched.push('&#x23f8; Paused — retrying in '+fmtSec(d.cooldown_remaining_sec));
    const schedEl = document.getElementById('scheduleCard');
    if (sched.length) { schedEl.style.display=''; document.getElementById('sSchedule').innerHTML=sched.join('<br>'); }
    else { schedEl.style.display='none'; }
  } catch(e) { console.error(e); }
  try {
    const r2 = await fetch('/api/log');
    const logs = await r2.json();
    const el = document.getElementById('sLog');
    el.innerHTML = logs.map(l => {
      const mm = Math.floor(l.t/60), hh = Math.floor(mm/60);
      const ts = hh+'h'+String(mm%60).padStart(2,'0')+'m'+String(l.t%60).padStart(2,'0')+'s';
      return '<div><span style="color:#666">'+ts+'</span> '+l.m+'</div>';
    }).join('');
  } catch(e) { console.error(e); }
}

async function loadDebug() {
  try {
    const r = await fetch('/api/status');
    const d = await r.json();
    document.getElementById('dIP').textContent = d.ip;
    const m = Math.floor(d.uptime/60), h = Math.floor(m/60);
    document.getElementById('dUptime').textContent = h+'h '+m%60+'m';
  } catch(e) { console.error(e); }
}

async function forceRefresh() {
  try {
    await fetch('/api/refresh', {method:'POST'});
    loadStatus();
  } catch(e) { alert('Failed: '+e.message); }
}

async function forceListen() {
  try {
    await fetch('/api/listen', {method:'POST'});
    loadStatus();
  } catch(e) { alert('Failed: '+e.message); }
}

async function testColors() {
  try {
    await fetch('/api/test-colors', {method:'POST'});
    alert('Test pattern sent — 6 bands: Black, White, Green, Blue, Red, Yellow');
  } catch(e) { alert('Failed: '+e.message); }
}

function bindSlider(id, valId, suffix) {
  const sl = document.getElementById(id);
  const vl = document.getElementById(valId);
  sl.oninput = () => { vl.textContent = sl.value + suffix; };
}
bindSlider('fSonosPoll','fSonosPollVal','s');
bindSlider('fVinylRecheck','fVinylRecheckVal',' min');
bindSlider('fCooldown','fCooldownVal',' min');
bindSlider('fIdleGallery','fIdleGalleryVal',' min');

async function loadSettings() {
  try {
    const r = await fetch('/api/settings');
    const d = await r.json();
    document.getElementById('fSonosIp').value = d.sonos_ip||'';
    document.getElementById('fShazamKey').value = '';
    document.getElementById('fShazamKey').placeholder = d.shazam_api_key_set ? '(set — leave blank to keep)' : '';
    // Timing sliders (API gives ms, sliders use seconds/minutes)
    const sp = document.getElementById('fSonosPoll');
    sp.value = Math.round((d.sonos_poll_ms||10000)/1000); sp.oninput();
    const vr = document.getElementById('fVinylRecheck');
    vr.value = Math.round((d.vinyl_recheck_ms||600000)/60000); vr.oninput();
    const cd = document.getElementById('fCooldown');
    cd.value = Math.round((d.no_match_cooldown_ms||300000)/60000); cd.oninput();
    const ig = document.getElementById('fIdleGallery');
    ig.value = Math.round((d.idle_gallery_ms||300000)/60000); ig.oninput();
    document.getElementById('fShowTrackInfo').checked = !!d.show_track_info;
    document.getElementById('fBgMode').value = (d.bg_mode !== undefined) ? d.bg_mode : 2;
  } catch(e) { console.error(e); }
}

async function saveSettings() {
  const body = {
    sonos_ip: document.getElementById('fSonosIp').value,
    sonos_poll_ms: parseInt(document.getElementById('fSonosPoll').value)*1000,
    vinyl_recheck_ms: parseInt(document.getElementById('fVinylRecheck').value)*60000,
    no_match_cooldown_ms: parseInt(document.getElementById('fCooldown').value)*60000,
    idle_gallery_ms: parseInt(document.getElementById('fIdleGallery').value)*60000,
    show_track_info: document.getElementById('fShowTrackInfo').checked,
    bg_mode: parseInt(document.getElementById('fBgMode').value)
  };
  const shz = document.getElementById('fShazamKey').value;
  if (shz) body.shazam_api_key = shz;
  const el = document.getElementById('settingsMsg');
  try {
    const r = await fetch('/api/settings', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(body)});
    el.className = r.ok ? 'msg ok' : 'msg err';
    el.textContent = r.ok ? 'Saved!' : 'Error saving';
  } catch(e) { el.className='msg err'; el.textContent=e.message; }
  setTimeout(()=>el.style.display='none',3000);
}

async function loadHistory() {
  const pinnedSection = document.getElementById('pinnedSection');
  const pinnedGrid = document.getElementById('pinnedGrid');
  const historySection = document.getElementById('historySection');
  const historyGrid = document.getElementById('historyGrid');
  const empty = document.getElementById('historyEmpty');
  try {
    const r = await fetch('/api/history');
    const items = await r.json();
    pinnedGrid.innerHTML = '';
    historyGrid.innerHTML = '';
    if (!items.length) { empty.style.display=''; pinnedSection.style.display='none'; historySection.style.display='none'; return; }
    empty.style.display='none';
    const reversed = items.slice().reverse();
    const pinned = reversed.filter(h => !!h.pin);
    const unpinned = reversed.filter(h => !h.pin);
    function buildCard(h) {
      const div = document.createElement('div');
      div.className = 'gallery-item';
      div.style.cssText = 'padding:0;overflow:hidden;position:relative;cursor:pointer';
      const on = h.on !== false;
      const pin = !!h.pin;
      div.innerHTML =
        '<img src="/api/history/image?f='+h.f+'" style="width:100%;aspect-ratio:1;object-fit:cover;display:block;'+(on?'':'opacity:.35;')+'">'
        + '<div style="padding:6px 8px;font-size:.7rem;line-height:1.3;'+(on?'':'opacity:.5;')+'">'
        + '<div style="color:#eee;white-space:nowrap;overflow:hidden;text-overflow:ellipsis">'+esc(h.a)+'</div>'
        + '<div style="color:#888;white-space:nowrap;overflow:hidden;text-overflow:ellipsis">'+esc(h.al||h.t)+'</div></div>'
        + '<div style="position:absolute;top:6px;right:6px;width:28px;height:28px;border-radius:50%;'
        + 'background:'+(on?'#2a6':'#444')+';display:flex;align-items:center;justify-content:center;'
        + 'font-size:.75rem;color:#fff;border:2px solid '+(on?'#2a6':'#666')+'">'
        + (on?'&#10003;':'')+'</div>'
        + '<div title="'+(pin?'Unpin':'Pin')+'" style="position:absolute;top:6px;left:6px;width:28px;height:28px;border-radius:50%;'
        + 'background:'+(pin?'#c8860a':'#333')+';display:flex;align-items:center;justify-content:center;'
        + 'font-size:.8rem;border:2px solid '+(pin?'#e6a020':'#555')+';cursor:pointer">'
        + '&#128204;</div>';
      div.onclick = () => toggleHistory(h.f, !on);
      div.lastElementChild.onclick = (e) => { e.stopPropagation(); pinHistory(h.f, !pin); };
      return div;
    }
    if (pinned.length) { pinnedSection.style.display=''; pinned.forEach(h => pinnedGrid.appendChild(buildCard(h))); }
    else { pinnedSection.style.display='none'; }
    if (unpinned.length) { historySection.style.display=''; unpinned.forEach(h => historyGrid.appendChild(buildCard(h))); }
    else { historySection.style.display='none'; }
  } catch(e) { console.error(e); }
}
function esc(s) { const d=document.createElement('div');d.textContent=s||'';return d.innerHTML; }
async function toggleHistory(f, on) {
  await fetch('/api/history/toggle', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'f='+encodeURIComponent(f)+'&on='+(on?'1':'0')});
  loadHistory();
}
async function pinHistory(f, pin) {
  await fetch('/api/history/pin', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'f='+encodeURIComponent(f)+'&pin='+(pin?'1':'0')});
  loadHistory();
}

loadStatus();
setInterval(()=>{ if(document.getElementById('status').classList.contains('active')) loadStatus(); }, 3000);
</script>
</body>
</html>
)rawliteral";
