#pragma once
#include <pgmspace.h>

// ─── Web portal ───
// Single embedded page. No external requests: the device may have no route to
// the internet, and a portal that waits on a CDN is a portal that hangs.
// Everything is system fonts, inline SVG and vanilla JS.

static const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="color-scheme" content="dark light">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<title>Now Playing</title>
<style>
:root{
  --bg:#0a0a0b; --surface:#161618; --surface-2:#1f1f22; --surface-3:#2a2a2e;
  --line:rgba(255,255,255,.09); --line-strong:rgba(255,255,255,.16);
  --text:#f4f4f5; --dim:#a1a1aa; --faint:#6b6b73;
  --accent:#fafafa; --on-accent:#0a0a0b;
  --live:#4ade80; --warn:#fbbf24; --danger:#f87171;
  --r:14px; --r-sm:10px;
  --shadow:0 1px 2px rgba(0,0,0,.4),0 8px 24px rgba(0,0,0,.35);
  --ease:cubic-bezier(.4,0,.2,1);
  --nav-h:calc(56px + env(safe-area-inset-bottom));
}
@media (prefers-color-scheme: light){
  :root{
    --bg:#f7f7f8; --surface:#fff; --surface-2:#f0f0f2; --surface-3:#e5e5e8;
    --line:rgba(0,0,0,.08); --line-strong:rgba(0,0,0,.14);
    --text:#18181b; --dim:#63636b; --faint:#9a9aa2;
    --accent:#18181b; --on-accent:#fff;
    --live:#16a34a; --warn:#b45309; --danger:#dc2626;
    --shadow:0 1px 2px rgba(0,0,0,.06),0 8px 24px rgba(0,0,0,.08);
  }
}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{margin:0;padding:0}
body{
  background:var(--bg);color:var(--text);
  font:400 15px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;
  -webkit-font-smoothing:antialiased;text-rendering:optimizeLegibility;
  padding-bottom:calc(var(--nav-h) + 16px);
  overscroll-behavior-y:contain;
}
button,input,select{font:inherit;color:inherit}
button{background:none;border:0;padding:0;cursor:pointer}
a{color:inherit}

/* ── Shell ── */
.wrap{max-width:640px;margin:0 auto;padding:0 20px}
header{
  position:sticky;top:0;z-index:30;background:color-mix(in srgb,var(--bg) 88%,transparent);
  backdrop-filter:saturate(180%) blur(20px);-webkit-backdrop-filter:saturate(180%) blur(20px);
  border-bottom:1px solid transparent;transition:border-color .2s var(--ease);
  padding-top:env(safe-area-inset-top);
}
header.scrolled{border-bottom-color:var(--line)}
.head{display:flex;align-items:center;justify-content:space-between;height:56px}
.brand{font-size:16px;font-weight:600;letter-spacing:-.01em}
.pill{
  display:inline-flex;align-items:center;gap:6px;height:26px;padding:0 10px;
  border-radius:999px;background:var(--surface-2);border:1px solid var(--line);
  font-size:12px;font-weight:500;color:var(--dim);white-space:nowrap
}
.dot{width:6px;height:6px;border-radius:50%;background:var(--faint);flex:none}
.dot.on{background:var(--live);box-shadow:0 0 0 3px color-mix(in srgb,var(--live) 22%,transparent)}
.dot.busy{background:var(--warn);box-shadow:0 0 0 3px color-mix(in srgb,var(--warn) 22%,transparent)}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.35}}
.dot.on,.dot.busy{animation:pulse 2.4s var(--ease) infinite}

/* ── Sections ── */
.view{display:none;padding-top:20px;animation:fade .22s var(--ease)}
.view.active{display:block}
@keyframes fade{from{opacity:0;transform:translateY(4px)}to{opacity:1;transform:none}}
.label{
  font-size:11px;font-weight:600;letter-spacing:.07em;text-transform:uppercase;
  color:var(--faint);margin:28px 0 10px
}
.label:first-child{margin-top:0}

/* ── Now Playing ── */
.art-wrap{position:relative;width:100%;aspect-ratio:1;margin:4px 0 24px}
.art{
  width:100%;height:100%;object-fit:cover;border-radius:var(--r);
  background:var(--surface-2);box-shadow:var(--shadow);display:block
}
.art.placeholder{display:grid;place-items:center;color:var(--faint)}
@keyframes shimmer{to{background-position:200% 0}}
.skeleton{
  background:linear-gradient(90deg,var(--surface-2) 25%,var(--surface-3) 50%,var(--surface-2) 75%);
  background-size:200% 100%;animation:shimmer 1.4s linear infinite
}
.track{text-align:center;margin-bottom:24px;min-height:76px}
.track h1{font-size:22px;font-weight:600;letter-spacing:-.02em;margin:0 0 4px;line-height:1.25}
.track p{margin:0;color:var(--dim);font-size:15px}
.track .album{color:var(--faint);font-size:13px;margin-top:3px}
.meta{text-align:center;color:var(--faint);font-size:12.5px;margin-top:-14px;margin-bottom:22px;min-height:18px}

/* ── Buttons ── */
.actions{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.btn{
  display:flex;align-items:center;justify-content:center;gap:8px;height:46px;
  border-radius:var(--r-sm);background:var(--surface-2);border:1px solid var(--line);
  font-size:15px;font-weight:500;transition:background .15s var(--ease),transform .1s var(--ease);
  width:100%
}
.btn:active{transform:scale(.98)}
.btn:hover{background:var(--surface-3)}
.btn.primary{background:var(--accent);color:var(--on-accent);border-color:transparent}
.btn.primary:hover{opacity:.9;background:var(--accent)}
.btn.danger{color:var(--danger)}
.btn[disabled]{opacity:.45;pointer-events:none}
.btn svg{width:17px;height:17px;flex:none}
.btn.busy{color:transparent;position:relative}
.btn.busy::after{
  content:"";position:absolute;width:16px;height:16px;border-radius:50%;
  border:2px solid currentColor;border-top-color:transparent;
  color:var(--text);animation:spin .7s linear infinite
}
.btn.primary.busy::after{color:var(--on-accent)}
@keyframes spin{to{transform:rotate(360deg)}}

/* ── List rows ── */
.card{background:var(--surface);border:1px solid var(--line);border-radius:var(--r);overflow:hidden}
.row{display:flex;align-items:center;gap:14px;padding:13px 16px;border-bottom:1px solid var(--line);min-height:52px}
.row:last-child{border-bottom:0}
.row .k{flex:none;color:var(--dim);font-size:14px}
.row .v{margin-left:auto;text-align:right;color:var(--text);font-size:14px;min-width:0;
  overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.row.tap{cursor:pointer;transition:background .15s var(--ease)}
.row.tap:hover{background:var(--surface-2)}
.row.col{flex-direction:column;align-items:stretch;gap:9px}
.hint{color:var(--faint);font-size:12.5px;line-height:1.45;margin:8px 2px 0}

/* ── Form controls ── */
input[type=text],input[type=password],select{
  width:100%;height:42px;padding:0 12px;border-radius:var(--r-sm);
  background:var(--surface-2);border:1px solid var(--line);outline:none;
  transition:border-color .15s var(--ease)
}
input:focus,select:focus{border-color:var(--line-strong)}
select{appearance:none;
  background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='12' height='8' viewBox='0 0 12 8'%3E%3Cpath d='M1 1l5 5 5-5' stroke='%23a1a1aa' stroke-width='1.6' fill='none' stroke-linecap='round'/%3E%3C/svg%3E");
  background-repeat:no-repeat;background-position:right 13px center;padding-right:34px}
.sw{position:relative;width:46px;height:28px;flex:none;margin-left:auto}
.sw input{position:absolute;opacity:0;width:100%;height:100%;margin:0;cursor:pointer;z-index:1}
.sw span{position:absolute;inset:0;border-radius:999px;background:var(--surface-3);
  transition:background .2s var(--ease)}
.sw span::after{content:"";position:absolute;top:3px;left:3px;width:22px;height:22px;
  border-radius:50%;background:#fff;box-shadow:0 1px 3px rgba(0,0,0,.3);
  transition:transform .2s var(--ease)}
.sw input:checked + span{background:var(--live)}
.sw input:checked + span::after{transform:translateX(18px)}
.slider{display:flex;align-items:center;gap:12px}
input[type=range]{flex:1;appearance:none;height:4px;border-radius:2px;background:var(--surface-3);outline:none}
input[type=range]::-webkit-slider-thumb{appearance:none;width:20px;height:20px;border-radius:50%;
  background:var(--accent);cursor:pointer;box-shadow:0 1px 3px rgba(0,0,0,.3)}
input[type=range]::-moz-range-thumb{width:20px;height:20px;border:0;border-radius:50%;
  background:var(--accent);cursor:pointer}
.slider .val{min-width:62px;text-align:right;color:var(--dim);font-size:13px;
  font-variant-numeric:tabular-nums}

/* ── Library grid ── */
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}
@media(min-width:520px){.grid{grid-template-columns:repeat(4,1fr)}}
.tile{position:relative;aspect-ratio:1;border-radius:var(--r-sm);overflow:hidden;
  background:var(--surface-2);cursor:pointer;transition:transform .12s var(--ease)}
.tile:active{transform:scale(.96)}
.tile img{width:100%;height:100%;object-fit:cover;display:block;opacity:0;
  transition:opacity .3s var(--ease)}
.tile img.loaded{opacity:1}
.tile.off img{opacity:.28}
.tile .badge{position:absolute;top:5px;right:5px;width:20px;height:20px;border-radius:50%;
  background:rgba(0,0,0,.62);display:grid;place-items:center;backdrop-filter:blur(6px)}
.tile .badge svg{width:11px;height:11px;color:#fff}
.empty{text-align:center;color:var(--faint);padding:56px 20px;font-size:14px}

/* ── Bottom nav ── */
nav{
  position:fixed;left:0;right:0;bottom:0;z-index:40;height:var(--nav-h);
  padding-bottom:env(safe-area-inset-bottom);
  background:color-mix(in srgb,var(--bg) 86%,transparent);
  backdrop-filter:saturate(180%) blur(20px);-webkit-backdrop-filter:saturate(180%) blur(20px);
  border-top:1px solid var(--line);display:flex
}
nav button{flex:1;display:flex;flex-direction:column;align-items:center;justify-content:center;
  gap:3px;color:var(--faint);font-size:10.5px;font-weight:500;transition:color .15s var(--ease)}
nav button svg{width:22px;height:22px}
nav button.active{color:var(--text)}

/* ── Toast ── */
#toasts{position:fixed;left:0;right:0;bottom:calc(var(--nav-h) + 12px);z-index:60;
  display:flex;flex-direction:column;align-items:center;gap:8px;pointer-events:none;padding:0 20px}
.toast{
  background:var(--surface-3);color:var(--text);border:1px solid var(--line-strong);
  padding:11px 16px;border-radius:999px;font-size:14px;box-shadow:var(--shadow);
  max-width:100%;animation:rise .25s var(--ease);display:flex;align-items:center;gap:9px
}
.toast.err{color:var(--danger)}
.toast.ok{color:var(--live)}
@keyframes rise{from{opacity:0;transform:translateY(10px)}to{opacity:1;transform:none}}
.toast.out{opacity:0;transform:translateY(6px);transition:all .25s var(--ease)}

/* ── Sheet ── */
#scrim{position:fixed;inset:0;background:rgba(0,0,0,.55);z-index:50;opacity:0;
  pointer-events:none;transition:opacity .25s var(--ease);backdrop-filter:blur(2px)}
#scrim.show{opacity:1;pointer-events:auto}
.sheet{
  position:fixed;left:0;right:0;bottom:0;z-index:55;background:var(--surface);
  border-radius:20px 20px 0 0;border-top:1px solid var(--line);
  padding:8px 16px calc(20px + env(safe-area-inset-bottom));
  transform:translateY(101%);transition:transform .3s var(--ease);box-shadow:var(--shadow)
}
.sheet.show{transform:none}
.sheet .grip{width:36px;height:4px;border-radius:2px;background:var(--surface-3);margin:8px auto 14px}
.sheet h3{margin:0 0 4px;font-size:16px;font-weight:600;text-align:center;
  overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.sheet .sub{margin:0 0 16px;text-align:center;color:var(--faint);font-size:13px;
  overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.sheet .opts{display:flex;flex-direction:column;gap:8px}
.sheet .opt{display:flex;align-items:center;gap:12px;padding:14px 16px;border-radius:var(--r-sm);
  background:var(--surface-2);font-size:15px;text-align:left;width:100%}
.sheet .opt:active{background:var(--surface-3)}
.sheet .opt svg{width:18px;height:18px;color:var(--dim);flex:none}
.sheet .opt.danger{color:var(--danger)}
.sheet .opt.danger svg{color:var(--danger)}

/* ── Log ── */
.log{font-size:12.5px;line-height:1.7;max-height:260px;overflow-y:auto;
  font-variant-numeric:tabular-nums;-webkit-overflow-scrolling:touch}
.log div{display:flex;gap:10px;padding:2px 0}
.log time{color:var(--faint);flex:none;font-size:11.5px;padding-top:1px}
.log span{color:var(--dim);word-break:break-word}
details summary{cursor:pointer;list-style:none;display:flex;align-items:center;
  justify-content:space-between;padding:13px 16px;font-size:14px;color:var(--dim)}
details summary::-webkit-details-marker{display:none}
details summary::after{content:"";width:7px;height:7px;border-right:1.6px solid var(--faint);
  border-bottom:1.6px solid var(--faint);transform:rotate(45deg);transition:transform .2s var(--ease)}
details[open] summary::after{transform:rotate(-135deg)}
details .body{padding:0 16px 14px}

/* ── Progress ── */
.bar{height:4px;border-radius:2px;background:var(--surface-3);overflow:hidden;margin-top:10px}
.bar i{display:block;height:100%;width:0;background:var(--live);transition:width .2s var(--ease)}
</style>
</head>
<body>

<header>
  <div class="wrap head">
    <div class="brand">Now Playing</div>
    <div class="pill"><span class="dot" id="dot"></span><span id="stateTxt">Connecting</span></div>
  </div>
</header>

<main class="wrap">

<!-- ══ NOW PLAYING ══ -->
<section class="view active" id="v-now">
  <div class="art-wrap">
    <img class="art skeleton" id="art" alt="" hidden>
    <div class="art placeholder skeleton" id="artPlaceholder">
      <svg width="48" height="48" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.2">
        <circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="2.5"/>
      </svg>
    </div>
  </div>
  <div class="track">
    <h1 id="npTitle">&nbsp;</h1>
    <p id="npArtist">&nbsp;</p>
    <p class="album" id="npAlbum">&nbsp;</p>
  </div>
  <div class="meta" id="npMeta"></div>
  <div class="actions">
    <button class="btn" id="btnRefresh" onclick="act(this,'/api/refresh','Display refreshing')">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round">
        <path d="M21 12a9 9 0 11-2.6-6.4M21 3v6h-6"/></svg>Refresh
    </button>
    <button class="btn" id="btnListen" onclick="act(this,'/api/listen','Listening…')">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round">
        <path d="M12 2a3 3 0 013 3v6a3 3 0 01-6 0V5a3 3 0 013-3zM19 10v1a7 7 0 01-14 0v-1M12 19v3"/></svg>Listen
    </button>
  </div>

  <div class="label">Activity</div>
  <div class="card">
    <details>
      <summary><span id="logSummary">Recent events</span></summary>
      <div class="body"><div class="log" id="log"></div></div>
    </details>
  </div>
</section>

<!-- ══ LIBRARY ══ -->
<section class="view" id="v-lib">
  <div id="libPinned" hidden>
    <div class="label">Pinned</div>
    <div class="grid" id="gridPinned"></div>
  </div>
  <div id="libAll" hidden>
    <div class="label"><span id="libCount">Library</span></div>
    <div class="grid" id="gridAll"></div>
  </div>
  <div class="empty" id="libEmpty" hidden>
    No artwork yet.<br>Play something and it will appear here.
  </div>
  <p class="hint">Covers are saved automatically as they are displayed. Dimmed covers are
  excluded from the idle rotation. Pinned covers are never removed when the library is full.</p>
</section>

<!-- ══ SETTINGS ══ -->
<section class="view" id="v-set">
  <div class="label">Speaker</div>
  <div class="card">
    <div class="row col">
      <select id="fSpeaker"></select>
      <button class="btn" id="btnScanSonos" onclick="scanSonos()">Scan for speakers</button>
    </div>
  </div>

  <div class="label">Wi-Fi</div>
  <div class="card">
    <div class="row"><span class="k">Network</span><span class="v" id="wifiCurrent">—</span></div>
    <div class="row tap" onclick="showWifi()"><span class="k">Change network</span>
      <span class="v" style="color:var(--faint)">›</span></div>
  </div>
  <div class="card" id="wifiPanel" hidden style="margin-top:10px">
    <div class="row col">
      <select id="fSsid"><option value="">Select a network…</option></select>
      <input type="password" id="fWifiPw" placeholder="Password" autocomplete="off">
      <div class="actions">
        <button class="btn" onclick="scanWifi()" id="btnScanWifi">Scan</button>
        <button class="btn primary" onclick="saveWifi()" id="btnSaveWifi">Connect</button>
      </div>
    </div>
  </div>
  <p class="hint" id="wifiHint" hidden>Saving new Wi-Fi details restarts the device.
  If it cannot connect it will reopen the <b>NowPlaying-Setup</b> network.</p>

  <div class="label">Vinyl identification</div>
  <div class="card">
    <div class="row col">
      <span class="k">Shazam API key</span>
      <input type="password" id="fShazam" placeholder="Not set" autocomplete="off">
    </div>
  </div>
  <p class="hint">A RapidAPI Shazam key lets the device identify records playing through
  the turntable input. Digital playback does not need one.</p>

  <div class="label">Timing</div>
  <div class="card">
    <div class="row col"><span class="k">Check Sonos every</span>
      <div class="slider"><input type="range" id="tPoll" min="5" max="60" step="5">
        <span class="val" id="tPollV"></span></div></div>
    <div class="row col"><span class="k">Re-identify vinyl every</span>
      <div class="slider"><input type="range" id="tVinyl" min="1" max="30">
        <span class="val" id="tVinylV"></span></div></div>
    <div class="row col"><span class="k">Pause after failed matches</span>
      <div class="slider"><input type="range" id="tCool" min="1" max="15">
        <span class="val" id="tCoolV"></span></div></div>
    <div class="row col"><span class="k">Rotate artwork when idle</span>
      <div class="slider"><input type="range" id="tIdle" min="1" max="30">
        <span class="val" id="tIdleV"></span></div></div>
  </div>

  <div class="label">Display</div>
  <div class="card">
    <div class="row"><span class="k">Show artist and album</span>
      <label class="sw"><input type="checkbox" id="fTrackInfo"><span></span></label></div>
    <div class="row col"><span class="k">Background</span>
      <select id="fBgMode">
        <option value="2">Automatic</option><option value="1">Blurred artwork</option>
        <option value="0">Solid colour</option></select></div>
    <div class="row col"><span class="k">Background tone</span>
      <select id="fBgStyle"><option value="0">Darken</option><option value="1">Lighten</option></select></div>
    <div class="row col"><span class="k">Render profile</span><select id="fProfile"></select></div>
  </div>
  <p class="hint">The render profile controls sharpening, contrast and how aggressively colour
  is dithered. <b>Punchy</b> suits bold graphic sleeves, <b>Soft</b> suits photographic ones.
  Changes apply to the next artwork.</p>

  <div class="label">Security</div>
  <div class="card">
    <div class="row col"><span class="k">Portal password</span>
      <input type="password" id="fPortalPw" placeholder="None" autocomplete="off"></div>
  </div>
  <p class="hint">Sets a password on this portal, with username <b>admin</b>. Without one,
  anyone on your network can change these settings.</p>

  <div style="margin:24px 0 8px">
    <button class="btn primary" id="btnSave" onclick="saveSettings()">Save changes</button>
  </div>
</section>

<!-- ══ SYSTEM ══ -->
<section class="view" id="v-sys">
  <div class="label">Device</div>
  <div class="card">
    <div class="row"><span class="k">Address</span><span class="v" id="dIp">—</span></div>
    <div class="row"><span class="k">Uptime</span><span class="v" id="dUp">—</span></div>
    <div class="row"><span class="k">State</span><span class="v" id="dState">—</span></div>
  </div>

  <div class="label">Diagnostics</div>
  <div class="card">
    <div class="row tap" onclick="act(this,'/api/test-colors','Colour bars sent')">
      <span class="k">Colour bars</span><span class="v" style="color:var(--faint)">›</span></div>
    <div class="row tap" onclick="act(this,'/api/test-dither','Dither test sent')">
      <span class="k">Dither test pattern</span><span class="v" style="color:var(--faint)">›</span></div>
    <div class="row tap" onclick="act(this,'/api/test-calibration','Calibration card sent')">
      <span class="k">Palette calibration card</span><span class="v" style="color:var(--faint)">›</span></div>
    <div class="row tap" onclick="location.href='/api/last-audio'">
      <span class="k">Download last recording</span><span class="v" style="color:var(--faint)">›</span></div>
  </div>
  <p class="hint">Test patterns stay on screen for 30 minutes so they can be photographed.
  Use <b>Refresh</b> on the Now Playing tab to release the display early.</p>

  <div class="label">Firmware</div>
  <div class="card">
    <div class="row col">
      <input type="file" id="fw" accept=".bin">
      <button class="btn" onclick="upload()" id="btnUpload">Install update</button>
      <div id="fwProg" hidden><div class="bar"><i id="fwBar"></i></div>
        <div class="hint" id="fwMsg" style="margin-top:8px"></div></div>
    </div>
  </div>
  <p class="hint">Upload <code>firmware.bin</code> from the build output. The device verifies
  the image before switching to it and restarts when finished — if the upload fails, the
  current firmware keeps running. Do not remove power during the update.</p>
</section>

</main>

<nav>
  <button class="active" data-view="now" onclick="go('now')">
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round">
      <circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="2.5"/></svg>Now Playing</button>
  <button data-view="lib" onclick="go('lib')">
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linejoin="round">
      <rect x="3" y="3" width="7" height="7" rx="1.5"/><rect x="14" y="3" width="7" height="7" rx="1.5"/>
      <rect x="3" y="14" width="7" height="7" rx="1.5"/><rect x="14" y="14" width="7" height="7" rx="1.5"/></svg>Library</button>
  <button data-view="set" onclick="go('set')">
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7">
      <circle cx="12" cy="12" r="3"/>
      <path d="M19.4 15a1.7 1.7 0 00.3 1.8l.1.1a2 2 0 11-2.8 2.8l-.1-.1a1.7 1.7 0 00-2.9 1.2 2 2 0 11-4 0 1.7 1.7 0 00-2.9-1.2l-.1.1a2 2 0 11-2.8-2.8l.1-.1A1.7 1.7 0 004 15a2 2 0 110-4 1.7 1.7 0 001.2-2.9l-.1-.1a2 2 0 112.8-2.8l.1.1A1.7 1.7 0 0011 4a2 2 0 114 0 1.7 1.7 0 002.9 1.2l.1-.1a2 2 0 112.8 2.8l-.1.1A1.7 1.7 0 0020 11a2 2 0 110 4z"/></svg>Settings</button>
  <button data-view="sys" onclick="go('sys')">
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round">
      <rect x="4" y="4" width="16" height="16" rx="2.5"/><path d="M9 9h6v6H9z"/>
      <path d="M9 1v3M15 1v3M9 20v3M15 20v3M1 9h3M1 15h3M20 9h3M20 15h3"/></svg>System</button>
</nav>

<div id="scrim" onclick="closeSheet()"></div>
<div class="sheet" id="sheet">
  <div class="grip"></div>
  <h3 id="sheetTitle"></h3>
  <p class="sub" id="sheetSub"></p>
  <div class="opts" id="sheetOpts"></div>
</div>
<div id="toasts"></div>

<script>
"use strict";
const $ = s => document.querySelector(s);
const $$ = s => Array.from(document.querySelectorAll(s));

/* ── Toasts ─────────────────────────────────────────────── */
function toast(msg, kind){
  const el = document.createElement('div');
  el.className = 'toast' + (kind ? ' ' + kind : '');
  el.textContent = msg;
  $('#toasts').appendChild(el);
  setTimeout(() => { el.classList.add('out'); setTimeout(() => el.remove(), 260); }, 2600);
}

/* ── Fetch helpers ──────────────────────────────────────── */
async function api(path, opts){
  const r = await fetch(path, opts);
  if(!r.ok) throw new Error('HTTP ' + r.status);
  const t = r.headers.get('content-type') || '';
  return t.includes('json') ? r.json() : r.text();
}
async function post(path, body){
  return api(path, body
    ? {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body}
    : {method:'POST'});
}
async function act(btn, path, msg){
  const b = btn.classList.contains('btn') ? btn : null;
  if(b){ b.classList.add('busy'); b.disabled = true; }
  try { await post(path); toast(msg, 'ok'); }
  catch(e){ toast('Failed: ' + e.message, 'err'); }
  finally { if(b){ b.classList.remove('busy'); b.disabled = false; } }
}

/* ── Navigation ─────────────────────────────────────────── */
let view = 'now';
function go(v){
  view = v;
  $$('.view').forEach(s => s.classList.toggle('active', s.id === 'v-' + v));
  $$('nav button').forEach(b => b.classList.toggle('active', b.dataset.view === v));
  window.scrollTo(0, 0);
  if(v === 'lib') loadLibrary();
  if(v === 'set') loadSettings();
}
addEventListener('scroll', () => {
  $('header').classList.toggle('scrolled', window.scrollY > 4);
}, {passive:true});

/* ── Now Playing ────────────────────────────────────────── */
const STATE_TEXT = {BOOT:'Starting', IDLE:'Idle', DIGITAL:'Playing', VINYL:'Vinyl',
                    ERROR:'Error', SETUP:'Setup'};
let lastArt = null;

function fmtUptime(s){
  const d = Math.floor(s/86400), h = Math.floor(s%86400/3600), m = Math.floor(s%3600/60);
  if(d) return d + 'd ' + h + 'h';
  if(h) return h + 'h ' + m + 'm';
  return m + 'm ' + (s % 60) + 's';
}

async function tick(){
  let d;
  try { d = await api('/api/status'); }
  catch(e){ $('#stateTxt').textContent = 'Offline'; $('#dot').className = 'dot'; return; }

  const st = d.state_name || 'IDLE';
  $('#stateTxt').textContent = STATE_TEXT[st] || st;
  $('#dot').className = 'dot' + (st === 'DIGITAL' || st === 'VINYL' ? ' on'
                        : st === 'ERROR' ? ' busy' : '');

  $('#npTitle').textContent  = d.title  || (st === 'IDLE' ? 'Nothing playing' : ' ');
  $('#npArtist').textContent = d.artist || ' ';
  $('#npAlbum').textContent  = d.album  || ' ';

  if(d.art_url && d.art_url !== lastArt){
    lastArt = d.art_url;
    const img = $('#art');
    img.onload = () => { img.hidden = false; img.classList.remove('skeleton');
                         $('#artPlaceholder').hidden = true; };
    img.onerror = () => { img.hidden = true; $('#artPlaceholder').hidden = false; };
    img.src = d.art_url;
  } else if(!d.art_url){
    lastArt = null; $('#art').hidden = true; $('#artPlaceholder').hidden = false;
  }

  const bits = [];
  if(d.display_hold_sec > 0)
    bits.push('Test pattern held for ' + Math.ceil(d.display_hold_sec/60) + ' min');
  else if(d.cooldown_remaining_sec > 0)
    bits.push('Paused for ' + Math.ceil(d.cooldown_remaining_sec/60) + ' min after failed matches');
  else if(d.retry_in_sec > 0) bits.push('Retrying in ' + d.retry_in_sec + 's');
  else if(d.next_vinyl_check_sec > 0)
    bits.push('Next vinyl check in ' + Math.ceil(d.next_vinyl_check_sec/60) + ' min');
  else if(d.next_poll_sec > 0) bits.push('Next check in ' + d.next_poll_sec + 's');
  $('#npMeta').textContent = bits.join('');

  $('#dIp').textContent = d.ip || '—';
  $('#dUp').textContent = fmtUptime(d.uptime || 0);
  $('#dState').textContent = STATE_TEXT[st] || st;
}

async function loadLog(){
  try {
    const l = await api('/api/log');
    $('#logSummary').textContent = l.length ? l[0].m.slice(0, 40) : 'No activity yet';
    $('#log').innerHTML = l.map(e => {
      const t = e.t, hh = Math.floor(t/3600), mm = Math.floor(t%3600/60), ss = t % 60;
      const stamp = (hh ? hh + 'h' : '') + String(mm).padStart(2,'0') + 'm'
                  + String(ss).padStart(2,'0') + 's';
      return '<div><time>' + stamp + '</time><span>' + esc(e.m) + '</span></div>';
    }).join('');
  } catch(e){}
}
function esc(s){ return String(s).replace(/[&<>"]/g, c =>
  ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c])); }

/* ── Library ────────────────────────────────────────────── */
let libLoaded = false, libItems = [];
const io = new IntersectionObserver(es => es.forEach(e => {
  if(e.isIntersecting){
    const img = e.target;
    img.src = img.dataset.src;
    img.onload = () => img.classList.add('loaded');
    io.unobserve(img);
  }
}), {rootMargin:'200px'});

async function loadLibrary(force){
  if(libLoaded && !force) return;
  try { libItems = await api('/api/history'); } catch(e){ return; }
  libLoaded = true;
  const pinned = libItems.filter(i => i.pin);
  const rest   = libItems.filter(i => !i.pin);

  $('#libEmpty').hidden  = libItems.length > 0;
  $('#libPinned').hidden = pinned.length === 0;
  $('#libAll').hidden    = rest.length === 0;
  $('#libCount').textContent = rest.length + (rest.length === 1 ? ' cover' : ' covers');
  $('#gridPinned').innerHTML = pinned.map(tile).join('');
  $('#gridAll').innerHTML    = rest.map(tile).join('');
  $$('.tile img[data-src]').forEach(i => io.observe(i));
}

function tile(i){
  const on = i.on !== false;
  return '<div class="tile' + (on ? '' : ' off') + '" onclick="openSheet(\'' + i.f + '\')">'
       + '<img data-src="/api/history/image?f=' + encodeURIComponent(i.f) + '" alt="">'
       + (i.pin ? '<div class="badge"><svg viewBox="0 0 24 24" fill="currentColor">'
                + '<path d="M16 3v6l3 3v2h-6v7l-1 1-1-1v-7H5v-2l3-3V3z"/></svg></div>' : '')
       + '</div>';
}

let sheetFile = null;
function openSheet(f){
  const it = libItems.find(i => i.f === f);
  if(!it) return;
  sheetFile = f;
  $('#sheetTitle').textContent = it.a || 'Unknown artist';
  $('#sheetSub').textContent   = it.al || it.t || '';
  const on = it.on !== false;
  $('#sheetOpts').innerHTML = [
    opt('show',   'Show on display',
      '<path d="M4 5h16v11H4zM9 20h6M12 16v4"/>'),
    opt('toggle', on ? 'Exclude from rotation' : 'Include in rotation',
      on ? '<path d="M2 12s4-7 10-7 10 7 10 7-4 7-10 7S2 12 2 12z"/><path d="M3 3l18 18"/>'
         : '<path d="M2 12s4-7 10-7 10 7 10 7-4 7-10 7S2 12 2 12z"/><circle cx="12" cy="12" r="3"/>'),
    opt('pin',    it.pin ? 'Unpin' : 'Pin to keep',
      '<path d="M16 3v6l3 3v2h-6v7l-1 1-1-1v-7H5v-2l3-3V3z"/>'),
    opt('delete', 'Delete', '<path d="M4 7h16M9 7V5h6v2M6 7l1 13h10l1-13"/>', true)
  ].join('');
  $('#scrim').classList.add('show');
  $('#sheet').classList.add('show');
}
function opt(action, text, path, danger){
  return '<button class="opt' + (danger ? ' danger' : '') + '" onclick="libAct(\'' + action + '\')">'
       + '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" '
       + 'stroke-linecap="round" stroke-linejoin="round">' + path + '</svg>' + text + '</button>';
}
function closeSheet(){
  $('#scrim').classList.remove('show');
  $('#sheet').classList.remove('show');
}

async function libAct(action){
  const f = sheetFile, it = libItems.find(i => i.f === f);
  closeSheet();
  if(!it) return;
  try {
    if(action === 'show'){
      await post('/api/history/show', 'f=' + encodeURIComponent(f));
      toast('Sending to display', 'ok');
    } else if(action === 'toggle'){
      const on = it.on !== false;
      await post('/api/history/toggle', 'f=' + encodeURIComponent(f) + '&on=' + (on ? '0' : '1'));
      toast(on ? 'Excluded from rotation' : 'Included in rotation', 'ok');
      loadLibrary(true);
    } else if(action === 'pin'){
      await post('/api/history/pin', 'f=' + encodeURIComponent(f) + '&pin=' + (it.pin ? '0' : '1'));
      toast(it.pin ? 'Unpinned' : 'Pinned', 'ok');
      loadLibrary(true);
    } else if(action === 'delete'){
      await post('/api/history/delete', 'f=' + encodeURIComponent(f));
      toast('Deleted', 'ok');
      loadLibrary(true);
    }
  } catch(e){ toast('Failed: ' + e.message, 'err'); }
}

/* ── Settings ───────────────────────────────────────────── */
let setLoaded = false;
const SLIDERS = [['tPoll','s',1000],['tVinyl',' min',60000],
                 ['tCool',' min',60000],['tIdle',' min',60000]];
SLIDERS.forEach(([id, suffix]) => {
  const el = $('#' + id);
  el.addEventListener('input', () => { $('#' + id + 'V').textContent = el.value + suffix; });
});

async function loadSettings(force){
  if(setLoaded && !force) return;
  try {
    const d = await api('/api/settings');
    setLoaded = true;

    const sel = $('#fSpeaker');
    sel.innerHTML = '<option value="">No speaker selected</option>';
    if(d.sonos_name){
      const o = document.createElement('option');
      o.value = o.textContent = d.sonos_name; o.dataset.ip = d.sonos_ip || '';
      sel.appendChild(o); sel.value = d.sonos_name;
    }

    $('#fShazam').placeholder = d.shazam_api_key_set ? 'Set — leave blank to keep' : 'Not set';
    $('#fPortalPw').placeholder = d.portal_password_set ? 'Set — leave blank to keep' : 'None';
    $('#fShazam').value = ''; $('#fPortalPw').value = '';

    const set = (id, v, suffix) => { const el = $('#' + id); el.value = v;
      $('#' + id + 'V').textContent = v + suffix; };
    set('tPoll',  Math.round((d.sonos_poll_ms || 10000)/1000), 's');
    set('tVinyl', Math.round((d.vinyl_recheck_ms || 600000)/60000), ' min');
    set('tCool',  Math.round((d.no_match_cooldown_ms || 300000)/60000), ' min');
    set('tIdle',  Math.round((d.idle_gallery_ms || 300000)/60000), ' min');

    $('#fTrackInfo').checked = !!d.show_track_info;
    $('#fBgMode').value  = d.bg_mode !== undefined ? d.bg_mode : 2;
    $('#fBgStyle').value = d.bg_style !== undefined ? d.bg_style : 0;

    try {
      const profs = await api('/api/profiles');
      $('#fProfile').innerHTML = profs.map(p =>
        '<option value="' + p.id + '">' + esc(p.name) + '</option>').join('');
      $('#fProfile').value = d.render_profile !== undefined ? d.render_profile : 1;
    } catch(e){}

    try { const w = await api('/api/wifi');
      $('#wifiCurrent').textContent = w.ssid || 'Not configured'; } catch(e){}
  } catch(e){ toast('Could not load settings', 'err'); }
}

function showWifi(){
  const p = $('#wifiPanel');
  p.hidden = !p.hidden;
  $('#wifiHint').hidden = p.hidden;
  if(!p.hidden) scanWifi();
}

async function pollScan(url, btn, label){
  btn.classList.add('busy'); btn.disabled = true;
  try {
    for(let i = 0; i < 15; i++){
      const r = await fetch(url);
      if(r.status === 200) return await r.json();
      await new Promise(res => setTimeout(res, 700));
    }
    toast(label + ' timed out', 'err');
    return null;
  } catch(e){ toast('Scan failed', 'err'); return null; }
  finally { btn.classList.remove('busy'); btn.disabled = false; }
}

async function scanWifi(){
  const nets = await pollScan('/api/wifi/scan', $('#btnScanWifi'), 'Wi-Fi scan');
  if(!nets) return;
  nets.sort((a,b) => b.rssi - a.rssi);
  $('#fSsid').innerHTML = '<option value="">Select a network…</option>' +
    nets.map(n => '<option value="' + esc(n.ssid) + '">' + esc(n.ssid) +
      (n.open ? ' (open)' : '') + '</option>').join('');
  toast(nets.length + ' networks found');
}

async function saveWifi(){
  const ssid = $('#fSsid').value;
  if(!ssid){ toast('Choose a network first', 'err'); return; }
  const btn = $('#btnSaveWifi');
  btn.classList.add('busy'); btn.disabled = true;
  try {
    await api('/api/wifi', {method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify({ssid, password: $('#fWifiPw').value})});
    toast('Restarting to join ' + ssid, 'ok');
  } catch(e){ toast('Failed: ' + e.message, 'err'); }
  finally { btn.classList.remove('busy'); btn.disabled = false; }
}

async function scanSonos(){
  const found = await pollScan('/api/sonos/scan', $('#btnScanSonos'), 'Speaker scan');
  if(!found) return;
  const sel = $('#fSpeaker'), prev = sel.value;
  sel.innerHTML = '<option value="">No speaker selected</option>' +
    found.map(d => '<option value="' + esc(d.name) + '" data-ip="' + esc(d.ip) + '">' +
      esc(d.name) + '</option>').join('');
  if(prev) sel.value = prev;
  toast(found.length ? found.length + ' speakers found' : 'No speakers found',
        found.length ? 'ok' : 'err');
}

async function saveSettings(){
  const btn = $('#btnSave');
  btn.classList.add('busy'); btn.disabled = true;
  const sel = $('#fSpeaker'), opt = sel.options[sel.selectedIndex];
  const body = {
    sonos_name: sel.value,
    sonos_ip: (opt && opt.dataset.ip) || '',
    sonos_poll_ms: +$('#tPoll').value * 1000,
    vinyl_recheck_ms: +$('#tVinyl').value * 60000,
    no_match_cooldown_ms: +$('#tCool').value * 60000,
    idle_gallery_ms: +$('#tIdle').value * 60000,
    show_track_info: $('#fTrackInfo').checked,
    bg_mode: +$('#fBgMode').value,
    bg_style: +$('#fBgStyle').value,
    render_profile: +($('#fProfile').value || 1)
  };
  if($('#fShazam').value)   body.shazam_api_key  = $('#fShazam').value;
  if($('#fPortalPw').value) body.portal_password = $('#fPortalPw').value;
  try {
    await api('/api/settings', {method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify(body)});
    toast('Settings saved', 'ok');
    setLoaded = false; loadSettings(true);
  } catch(e){ toast('Failed: ' + e.message, 'err'); }
  finally { btn.classList.remove('busy'); btn.disabled = false; }
}

/* ── Firmware ───────────────────────────────────────────── */
function upload(){
  const f = $('#fw').files && $('#fw').files[0];
  if(!f){ toast('Choose a firmware file first', 'err'); return; }
  $('#fwProg').hidden = false;
  $('#fwMsg').textContent = 'Uploading ' + Math.round(f.size/1024) + ' KB…';
  $('#btnUpload').classList.add('busy'); $('#btnUpload').disabled = true;

  const fd = new FormData(); fd.append('firmware', f, f.name);
  const x = new XMLHttpRequest();
  x.open('POST', '/api/update');
  x.upload.onprogress = e => {
    if(e.lengthComputable) $('#fwBar').style.width = Math.round(e.loaded/e.total*100) + '%';
  };
  x.onload = () => {
    $('#btnUpload').classList.remove('busy'); $('#btnUpload').disabled = false;
    if(x.status === 200){
      $('#fwBar').style.width = '100%';
      $('#fwMsg').textContent = 'Installed. The device is restarting and will be back in about 20 seconds.';
      toast('Update installed', 'ok');
    } else {
      $('#fwMsg').textContent = 'Update failed (HTTP ' + x.status + '). The current firmware is unchanged.';
      toast('Update failed', 'err');
    }
  };
  x.onerror = () => {
    $('#btnUpload').classList.remove('busy'); $('#btnUpload').disabled = false;
    $('#fwMsg').textContent = 'Connection lost during upload.';
    toast('Upload failed', 'err');
  };
  x.send(fd);
}

/* ── Polling ────────────────────────────────────────────── */
let timer = null;
function startPolling(){
  stopPolling();
  tick(); loadLog();
  timer = setInterval(() => { tick(); if(view === 'now') loadLog(); }, 3000);
}
function stopPolling(){ if(timer) clearInterval(timer); timer = null; }
// Don't poll a device on battery while the tab is in the background.
document.addEventListener('visibilitychange',
  () => document.hidden ? stopPolling() : startPolling());
startPolling();
</script>
</body>
</html>
)rawliteral";
