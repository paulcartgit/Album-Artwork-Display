#pragma once
#include <pgmspace.h>

// ─── First-run setup page ───
// Served on the NowPlaying-Setup access point, and shown again if the saved
// network stops working. This is the first thing anyone sees, and it is shown
// on a phone that has just been pulled off the home Wi-Fi — so it has to be
// completely self-contained and it has to explain what is happening.

static const char CAPTIVE_PORTAL_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="color-scheme" content="dark light">
<title>Set up Now Playing</title>
<style>
:root{
  --bg:#0a0a0b; --surface:#161618; --surface-2:#1f1f22; --surface-3:#2a2a2e;
  --line:rgba(255,255,255,.09); --line-strong:rgba(255,255,255,.16);
  --text:#f4f4f5; --dim:#a1a1aa; --faint:#8a8a93;
  --accent:#fafafa; --on-accent:#0a0a0b; --live:#4ade80; --danger:#f87171;
  --r:14px; --ease:cubic-bezier(.4,0,.2,1);
}
@media (prefers-color-scheme: light){
  :root{
    --bg:#f7f7f8; --surface:#fff; --surface-2:#f0f0f2; --surface-3:#e5e5e8;
    --line:rgba(0,0,0,.08); --line-strong:rgba(0,0,0,.14);
    --text:#18181b; --dim:#63636b; --faint:#6e6e77;
    --accent:#18181b; --on-accent:#fff; --live:#15803d; --danger:#dc2626;
  }
}
@media (prefers-reduced-motion: reduce){
  *,*::before,*::after{animation-duration:.01ms!important;transition-duration:.01ms!important}
}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{
  margin:0;background:var(--bg);color:var(--text);
  font:400 15px/1.55 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;
  -webkit-font-smoothing:antialiased;
  padding:max(32px,env(safe-area-inset-top)) 20px calc(32px + env(safe-area-inset-bottom));
}
button,input,select{font:inherit;color:inherit}
button{border:0;cursor:pointer}
:focus-visible{outline:2px solid var(--accent);outline-offset:2px;border-radius:6px}
.wrap{max-width:400px;margin:0 auto}

.mark{width:44px;height:44px;border-radius:12px;background:var(--surface-2);
  border:1px solid var(--line);display:grid;place-items:center;margin-bottom:20px}
.mark svg{width:24px;height:24px;color:var(--text)}
h1{font-size:26px;font-weight:600;letter-spacing:-.022em;margin:0 0 8px;line-height:1.2}
.lede{color:var(--dim);margin:0 0 28px;font-size:15px}

.label{font-size:11px;font-weight:600;letter-spacing:.07em;text-transform:uppercase;
  color:var(--faint);margin:0 0 9px}
.card{background:var(--surface);border:1px solid var(--line);border-radius:var(--r);
  padding:16px;margin-bottom:14px}
.field{margin-bottom:14px}
.field:last-child{margin-bottom:0}
input,select{width:100%;height:46px;padding:0 13px;border-radius:10px;
  background:var(--surface-2);border:1px solid var(--line);outline:none;
  transition:border-color .15s var(--ease)}
input:focus,select:focus{border-color:var(--line-strong)}
select{appearance:none;padding-right:36px;
  background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='12' height='8' viewBox='0 0 12 8'%3E%3Cpath d='M1 1l5 5 5-5' stroke='%238a8a93' stroke-width='1.6' fill='none' stroke-linecap='round'/%3E%3C/svg%3E");
  background-repeat:no-repeat;background-position:right 13px center}

.btn{display:flex;align-items:center;justify-content:center;gap:8px;width:100%;height:48px;
  border-radius:10px;background:var(--surface-2);border:1px solid var(--line);
  font-size:15px;font-weight:500;transition:background .15s var(--ease),transform .1s var(--ease)}
.btn:active{transform:scale(.985)}
.btn.primary{background:var(--accent);color:var(--on-accent);border-color:transparent}
.btn[disabled]{opacity:.45;pointer-events:none}
.btn.busy{color:transparent;position:relative}
.btn.busy::after{content:"";position:absolute;width:17px;height:17px;border-radius:50%;
  border:2px solid var(--text);border-top-color:transparent;animation:spin .7s linear infinite}
.btn.primary.busy::after{border-color:var(--on-accent);border-top-color:transparent}
@keyframes spin{to{transform:rotate(360deg)}}

.msg{margin-top:14px;padding:12px 14px;border-radius:10px;font-size:14px;display:none;
  border:1px solid var(--line);background:var(--surface-2)}
.msg.show{display:block;animation:fade .2s var(--ease)}
.msg.err{color:var(--danger)}
.msg.ok{color:var(--live)}
@keyframes fade{from{opacity:0;transform:translateY(3px)}to{opacity:1;transform:none}}

.steps{list-style:none;margin:22px 0 0;padding:0;counter-reset:s}
.steps li{counter-increment:s;display:flex;gap:12px;color:var(--faint);font-size:13.5px;
  line-height:1.5;padding:7px 0}
.steps li::before{content:counter(s);flex:none;width:20px;height:20px;border-radius:50%;
  background:var(--surface-2);border:1px solid var(--line);color:var(--dim);
  font-size:11px;font-weight:600;display:grid;place-items:center;margin-top:1px}
.foot{color:var(--faint);font-size:12.5px;text-align:center;margin-top:26px;line-height:1.5}
.sr{position:absolute;width:1px;height:1px;overflow:hidden;clip:rect(0 0 0 0)}
</style>
</head>
<body>
<div class="wrap">

  <div class="mark">
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5">
      <circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="2.5"/>
    </svg>
  </div>

  <h1>Set up your display</h1>
  <p class="lede">Choose the Wi-Fi network the frame should join. It will restart and
  reconnect automatically.</p>

  <div class="card">
    <div class="field">
      <div class="label"><label for="ssid">Network</label></div>
      <select id="ssid"><option value="">Scanning…</option></select>
    </div>
    <div class="field">
      <div class="label"><label for="pw">Password</label></div>
      <input type="password" id="pw" placeholder="Leave blank if open" autocomplete="off">
    </div>
    <button class="btn primary" id="save" onclick="save()">Connect</button>
    <div class="msg" id="msg" role="status" aria-live="polite"></div>
  </div>

  <button class="btn" id="rescan" onclick="scan(true)">Scan again</button>

  <ol class="steps">
    <li>The frame restarts and joins your network.</li>
    <li>This page will stop responding — that is expected. Rejoin your usual Wi-Fi.</li>
    <li>Open <b>nowplaying.local</b> to finish setup and choose your Sonos speaker.</li>
  </ol>

  <p class="foot">If the password is wrong, the frame reopens this
  <b>NowPlaying-Setup</b> network so you can try again.</p>
</div>

<script>
"use strict";
const $ = s => document.querySelector(s);
function say(text, kind){
  const m = $('#msg');
  m.textContent = text;
  m.className = 'msg show' + (kind ? ' ' + kind : '');
}

async function scan(manual){
  const btn = $('#rescan');
  if(manual){ btn.classList.add('busy'); btn.disabled = true; }
  try {
    // The device scans asynchronously and answers 202 until results are ready.
    let r = await fetch('/api/wifi/scan');
    for(let i = 0; i < 12 && r.status === 202; i++){
      await new Promise(res => setTimeout(res, 700));
      r = await fetch('/api/wifi/scan');
    }
    const nets = await r.json();
    const sel = $('#ssid');
    if(!nets.length){
      sel.innerHTML = '<option value="">No networks found</option>';
      if(manual) say('No networks found. Move the frame closer to your router and scan again.', 'err');
      return;
    }
    nets.sort((a,b) => b.rssi - a.rssi);
    const prev = sel.value;
    sel.innerHTML = '<option value="">Select a network…</option>' + nets.map(n =>
      '<option value="' + esc(n.ssid) + '">' + esc(n.ssid) + (n.open ? '  (open)' : '') +
      '</option>').join('');
    if(prev) sel.value = prev;
    if(manual) say(nets.length + ' networks found', 'ok');
  } catch(e){
    say('Could not scan for networks. Reload this page and try again.', 'err');
  } finally {
    if(manual){ btn.classList.remove('busy'); btn.disabled = false; }
  }
}

function esc(s){ return String(s).replace(/[&<>"]/g, c =>
  ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c])); }

async function save(){
  const ssid = $('#ssid').value;
  if(!ssid){ say('Choose a network first.', 'err'); return; }
  const btn = $('#save');
  btn.classList.add('busy'); btn.disabled = true;
  try {
    const r = await fetch('/api/wifi/save', {
      method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify({ssid, password: $('#pw').value})
    });
    if(!r.ok) throw new Error('HTTP ' + r.status);
    say('Saved. The frame is restarting and joining ' + ssid +
        '. You can close this page and rejoin your usual Wi-Fi.', 'ok');
    btn.textContent = 'Restarting…';
  } catch(e){
    say('Could not save: ' + e.message, 'err');
    btn.classList.remove('busy'); btn.disabled = false;
  }
}

scan(false);
</script>
</body>
</html>
)rawliteral";
