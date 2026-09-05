#pragma once
#include <pgmspace.h>

// ─── Web portal ───
//
// One view matters: what is on the wall right now. Everything else is
// occasional. So Now Playing is the app, not a tab within it — History and
// Settings push in over the top and come back with a Done button, which is a
// hierarchy rather than four things claiming equal importance.
//
// The hero is the panel's actual contents, read back from the device, because
// that is the thing the whole project exists to produce. The source artwork is
// one tap away for comparison.
//
// No external requests: the device may have no route to the internet, and a
// portal that waits on a CDN is a portal that hangs. System fonts, inline SVG,
// vanilla JS.

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
  --text:#f4f4f5; --dim:#a1a1aa; --faint:#8a8a93;
  --accent:#fafafa; --on-accent:#0a0a0b;
  --live:#4ade80; --warn:#fbbf24; --danger:#f87171;
  --r:16px; --r-sm:11px;
  --ease:cubic-bezier(.32,.72,0,1);
  --top:calc(52px + env(safe-area-inset-top));
}
@media (prefers-color-scheme: light){
  :root{
    --bg:#f7f7f8; --surface:#fff; --surface-2:#f0f0f2; --surface-3:#e5e5e8;
    --line:rgba(0,0,0,.08); --line-strong:rgba(0,0,0,.14);
    --text:#18181b; --dim:#63636b; --faint:#6e6e77;
    --accent:#18181b; --on-accent:#fff;
    --live:#15803d; --warn:#b45309; --danger:#dc2626;
  }
}
@media (prefers-reduced-motion: reduce){
  *,*::before,*::after{animation-duration:.01ms!important;transition-duration:.01ms!important}
}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{margin:0;padding:0;height:100%}
body{
  background:var(--bg);color:var(--text);
  font:400 15px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;
  -webkit-font-smoothing:antialiased;overscroll-behavior-y:contain;
}
button,input,select{font:inherit;color:inherit}
button{background:none;border:0;padding:0;cursor:pointer}
:focus-visible{outline:2px solid var(--accent);outline-offset:2px;border-radius:6px}
.sr{position:absolute;width:1px;height:1px;overflow:hidden;clip:rect(0 0 0 0);white-space:nowrap}

/* ── Screens: root sits still, the rest slide over it ── */
.screen{position:fixed;inset:0;overflow-y:auto;-webkit-overflow-scrolling:touch;
  background:var(--bg);padding-bottom:calc(40px + env(safe-area-inset-bottom))}
.screen.sub{transform:translateX(100%);transition:transform .34s var(--ease);
  z-index:20;box-shadow:-12px 0 32px rgba(0,0,0,.28)}
.screen.sub.open{transform:none}
.wrap{max-width:600px;margin:0 auto;padding:0 20px}

/* ── Top bar ── */
.bar{position:sticky;top:0;z-index:10;height:var(--top);padding-top:env(safe-area-inset-top);
  display:flex;align-items:center;gap:10px;
  background:color-mix(in srgb,var(--bg) 88%,transparent);
  backdrop-filter:saturate(180%) blur(20px);-webkit-backdrop-filter:saturate(180%) blur(20px);
  border-bottom:1px solid transparent;transition:border-color .2s var(--ease)}
.bar.scrolled{border-bottom-color:var(--line)}
.bar h1{font-size:16px;font-weight:600;letter-spacing:-.01em;margin:0}
.bar .spacer{flex:1}
.iconbtn{width:36px;height:36px;border-radius:10px;display:grid;place-items:center;
  color:var(--dim);transition:background .15s var(--ease),color .15s var(--ease)}
.iconbtn:hover{background:var(--surface-2);color:var(--text)}
.iconbtn svg{width:20px;height:20px}
.done{font-size:15px;font-weight:500;color:var(--text);padding:6px 4px}

/* ── The frame: the hero ── */
.stage{padding:14px 0 0}
.frame{position:relative;display:block;width:100%;aspect-ratio:480/800;max-height:70vh;margin:0 auto;
  padding:0;border:0;cursor:pointer;
  border-radius:var(--r);overflow:hidden;background:var(--surface-2);
  box-shadow:0 2px 6px rgba(0,0,0,.35),0 18px 50px rgba(0,0,0,.4);
  display:block}
@media (prefers-color-scheme: light){
  .frame{box-shadow:0 2px 6px rgba(0,0,0,.09),0 18px 44px rgba(0,0,0,.13)}
}
.frame img{width:100%;height:100%;object-fit:contain;display:block;opacity:0;
  transition:opacity .35s var(--ease)}
.frame img.on{opacity:1}
.frame .ph{position:absolute;inset:0;display:grid;place-items:center;color:var(--faint)}
@keyframes shimmer{to{background-position:200% 0}}
.skeleton{background:linear-gradient(90deg,var(--surface-2) 25%,var(--surface-3) 50%,var(--surface-2) 75%);
  background-size:200% 100%;animation:shimmer 1.5s linear infinite}

.tag{position:absolute;top:10px;left:50%;transform:translateX(-50%);
  padding:4px 10px;border-radius:999px;font-size:11px;font-weight:500;letter-spacing:.03em;
  background:rgba(0,0,0,.55);color:#fff;backdrop-filter:blur(8px)}

/* ── Track ── */
.track{text-align:center;margin:22px 0 4px}
.track h2{font-size:23px;font-weight:600;letter-spacing:-.022em;margin:0 0 5px;line-height:1.24}
.track .artist{margin:0;color:var(--dim);font-size:15.5px}
.track .album{margin:3px 0 0;color:var(--faint);font-size:13px}
.track .release{margin:9px 0 0;color:var(--faint);font-size:12px;letter-spacing:.02em;min-height:16px}
.status{display:flex;align-items:center;justify-content:center;gap:7px;margin:0 0 20px;
  color:var(--faint);font-size:12.5px}
.status[hidden]{display:none}
.dot{width:6px;height:6px;border-radius:50%;background:var(--faint);flex:none}
.dot.on{background:var(--live);box-shadow:0 0 0 3px color-mix(in srgb,var(--live) 22%,transparent)}
.dot.warn{background:var(--warn);box-shadow:0 0 0 3px color-mix(in srgb,var(--warn) 22%,transparent)}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.4}}
.dot.on,.dot.warn{animation:pulse 2.4s var(--ease) infinite}

/* ── Buttons ── */
.actions{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.btn{display:flex;align-items:center;justify-content:center;gap:8px;height:46px;width:100%;
  border-radius:var(--r-sm);background:var(--surface-2);border:1px solid var(--line);
  font-size:15px;font-weight:500;transition:background .15s var(--ease),transform .1s var(--ease)}
.btn:active{transform:scale(.98)}
.btn:hover{background:var(--surface-3)}
.btn.primary{background:var(--accent);color:var(--on-accent);border-color:transparent}
.btn.danger{color:var(--danger)}
.btn[disabled]{opacity:.45;pointer-events:none}
.btn svg{width:17px;height:17px;flex:none}
.btn.busy{color:transparent;position:relative}
.btn.busy::after{content:"";position:absolute;width:16px;height:16px;border-radius:50%;
  border:2px solid var(--text);border-top-color:transparent;animation:spin .7s linear infinite}
.btn.primary.busy::after{border-color:var(--on-accent);border-top-color:transparent}
@keyframes spin{to{transform:rotate(360deg)}}

/* ── Lists ── */
.label{font-size:11px;font-weight:600;letter-spacing:.07em;text-transform:uppercase;
  color:var(--faint);margin:28px 0 10px}
.card{background:var(--surface);border:1px solid var(--line);border-radius:var(--r);overflow:hidden}
.row{display:flex;align-items:center;gap:14px;padding:13px 16px;
  border-bottom:1px solid var(--line);min-height:52px}
.row:last-child{border-bottom:0}
.row .k{color:var(--dim);font-size:14px}
.row .v{margin-left:auto;text-align:right;color:var(--text);font-size:14px;
  overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.row.tap{cursor:pointer;transition:background .15s var(--ease)}
.row.tap:hover{background:var(--surface-2)}
.row.col{flex-direction:column;align-items:stretch;gap:9px}
.chev{color:var(--faint);margin-left:auto}
.hint{color:var(--faint);font-size:12.5px;line-height:1.45;margin:8px 2px 0}

input[type=text],input[type=password],select{width:100%;height:42px;padding:0 12px;
  border-radius:var(--r-sm);background:var(--surface-2);border:1px solid var(--line);
  outline:none;transition:border-color .15s var(--ease)}
input:focus,select:focus{border-color:var(--line-strong)}
select{appearance:none;padding-right:34px;
  background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='12' height='8' viewBox='0 0 12 8'%3E%3Cpath d='M1 1l5 5 5-5' stroke='%238a8a93' stroke-width='1.6' fill='none' stroke-linecap='round'/%3E%3C/svg%3E");
  background-repeat:no-repeat;background-position:right 13px center}
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
input[type=range]{flex:1;appearance:none;height:4px;border-radius:2px;background:var(--surface-3)}
input[type=range]::-webkit-slider-thumb{appearance:none;width:20px;height:20px;border-radius:50%;
  background:var(--accent);box-shadow:0 1px 3px rgba(0,0,0,.3)}
input[type=range]::-moz-range-thumb{width:20px;height:20px;border:0;border-radius:50%;
  background:var(--accent)}
.slider .val{min-width:64px;text-align:right;color:var(--dim);font-size:13px;
  font-variant-numeric:tabular-nums}

/* ── History grid ── */
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
.empty{text-align:center;color:var(--faint);padding:60px 20px;font-size:14px;line-height:1.6}

/* ── Log ── */
details summary{cursor:pointer;list-style:none;display:flex;align-items:center;
  justify-content:space-between;padding:13px 16px;font-size:14px;color:var(--dim)}
details summary::-webkit-details-marker{display:none}
details summary::after{content:"";width:7px;height:7px;border-right:1.6px solid var(--faint);
  border-bottom:1.6px solid var(--faint);transform:rotate(45deg);transition:transform .2s var(--ease)}
details[open] summary::after{transform:rotate(-135deg)}
.log{font-size:12.5px;line-height:1.7;max-height:250px;overflow-y:auto;padding:0 16px 14px;
  font-variant-numeric:tabular-nums}
.log div{display:flex;gap:10px;padding:2px 0}
.log time{color:var(--faint);flex:none;font-size:11.5px;padding-top:1px}
.log span{color:var(--dim);word-break:break-word}

/* ── Toast / sheet ── */
#toasts{position:fixed;left:0;right:0;bottom:calc(24px + env(safe-area-inset-bottom));z-index:60;
  display:flex;flex-direction:column;align-items:center;gap:8px;pointer-events:none;padding:0 20px}
.toast{background:var(--surface-3);border:1px solid var(--line-strong);padding:11px 16px;
  border-radius:999px;font-size:14px;box-shadow:0 8px 24px rgba(0,0,0,.35);
  animation:rise .25s var(--ease)}
.toast.err{color:var(--danger)} .toast.ok{color:var(--live)}
@keyframes rise{from{opacity:0;transform:translateY(10px)}to{opacity:1;transform:none}}
.toast.out{opacity:0;transform:translateY(6px);transition:all .25s var(--ease)}
#scrim{position:fixed;inset:0;background:rgba(0,0,0,.55);z-index:50;opacity:0;
  pointer-events:none;transition:opacity .25s var(--ease)}
#scrim.show{opacity:1;pointer-events:auto}
.sheet{position:fixed;left:0;right:0;bottom:0;z-index:55;background:var(--surface);
  border-radius:20px 20px 0 0;border-top:1px solid var(--line);
  padding:8px 16px calc(20px + env(safe-area-inset-bottom));
  transform:translateY(101%);transition:transform .3s var(--ease)}
.sheet.show{transform:none}
.sheet .grip{width:36px;height:4px;border-radius:2px;background:var(--surface-3);margin:8px auto 14px}
.sheet h3{margin:0 0 4px;font-size:16px;font-weight:600;text-align:center;
  overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.sheet .sub{margin:0 0 16px;text-align:center;color:var(--faint);font-size:13px;
  overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.sheet .opts{display:flex;flex-direction:column;gap:8px}
.opt{display:flex;align-items:center;gap:12px;padding:14px 16px;border-radius:var(--r-sm);
  background:var(--surface-2);font-size:15px;text-align:left;width:100%}
.opt:active{background:var(--surface-3)}
.opt svg{width:18px;height:18px;color:var(--dim);flex:none}
.opt.danger,.opt.danger svg{color:var(--danger)}
.bar2{height:4px;border-radius:2px;background:var(--surface-3);overflow:hidden;margin-top:10px}
.bar2 i{display:block;height:100%;width:0;background:var(--live);transition:width .2s var(--ease)}
</style>
</head>
<body>

<!-- ═══ NOW PLAYING — the app ═══ -->
<div class="screen" id="root">
  <div class="bar wrap">
    <h1>Now Playing</h1>
    <div class="spacer"></div>
    <button class="iconbtn" onclick="openScreen('history')" aria-label="History">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linejoin="round">
        <rect x="3" y="3" width="7" height="7" rx="1.5"/><rect x="14" y="3" width="7" height="7" rx="1.5"/>
        <rect x="3" y="14" width="7" height="7" rx="1.5"/><rect x="14" y="14" width="7" height="7" rx="1.5"/></svg>
    </button>
    <button class="iconbtn" onclick="openScreen('settings')" aria-label="Settings">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7">
        <circle cx="12" cy="12" r="3"/>
        <path d="M19.4 15a1.7 1.7 0 00.3 1.8l.1.1a2 2 0 11-2.8 2.8l-.1-.1a1.7 1.7 0 00-2.9 1.2 2 2 0 11-4 0 1.7 1.7 0 00-2.9-1.2l-.1.1a2 2 0 11-2.8-2.8l.1-.1A1.7 1.7 0 004 15a2 2 0 110-4 1.7 1.7 0 001.2-2.9l-.1-.1a2 2 0 112.8-2.8l.1.1A1.7 1.7 0 0011 4a2 2 0 114 0 1.7 1.7 0 002.9 1.2l.1-.1a2 2 0 112.8 2.8l-.1.1A1.7 1.7 0 0020 11a2 2 0 110 4z"/></svg>
    </button>
  </div>

  <div class="wrap">
    <div class="stage">
      <button class="frame" id="frame" onclick="toggleView()"
              aria-label="Show the original artwork instead">
        <img id="art" alt="">
        <div class="ph skeleton" id="ph">
          <svg width="44" height="44" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.2">
            <circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="2.5"/></svg>
        </div>
        <span class="tag" id="viewTag" hidden>Original</span>
      </button>
      <span class="sr" id="artDesc" aria-live="polite"></span>
    </div>

    <div class="track">
      <h2 id="npTitle">&nbsp;</h2>
      <p class="artist" id="npArtist">&nbsp;</p>
      <p class="album" id="npAlbum">&nbsp;</p>
      <p class="release" id="npRelease"></p>
    </div>

    <div class="status" id="statusRow"><span class="dot" id="dot"></span>
      <span id="npStatus">Connecting</span></div>

    <div class="actions">
      <button class="btn" id="btnRefresh" onclick="act(this,'/api/refresh','Redrawing the frame')">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round">
          <path d="M21 12a9 9 0 11-2.6-6.4M21 3v6h-6"/></svg>Redraw
      </button>
      <button class="btn" id="btnListen" onclick="act(this,'/api/listen','Listening…')">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round">
          <path d="M12 2a3 3 0 013 3v6a3 3 0 01-6 0V5a3 3 0 013-3zM19 10v1a7 7 0 01-14 0v-1M12 19v3"/></svg>Listen
      </button>
    </div>

  </div>
</div>

<!-- ═══ HISTORY ═══ -->
<div class="screen sub" id="history">
  <div class="bar wrap">
    <h1>History</h1><div class="spacer"></div>
    <button class="done" onclick="closeScreen()">Done</button>
  </div>
  <div class="wrap">
    <div id="hPinned" hidden><div class="label">Pinned</div><div class="grid" id="gridPinned"></div></div>
    <div id="hAll" hidden><div class="label" id="hCount"></div><div class="grid" id="gridAll"></div></div>
    <div class="empty" id="hEmpty" hidden>Nothing here yet.<br>Covers are saved as they appear on the frame.</div>
    <p class="hint">Every cover the frame has shown is kept here. Dimmed ones are excluded from
    the idle rotation; pinned ones are never removed when the history fills up.</p>
  </div>
</div>

<!-- ═══ SETTINGS ═══ -->
<div class="screen sub" id="settings">
  <div class="bar wrap">
    <h1>Settings</h1><div class="spacer"></div>
    <button class="done" onclick="closeScreen()">Done</button>
  </div>
  <div class="wrap">
    <div class="label">Speaker</div>
    <div class="card"><div class="row col">
      <select id="fSpeaker" aria-label="Sonos speaker"></select>
      <button class="btn" id="btnScanSonos" onclick="scanSonos()">Scan for speakers</button>
    </div></div>

    <div class="label">Network</div>
    <div class="card">
      <div class="row"><span class="k">Wi-Fi</span><span class="v" id="wifiCurrent">—</span></div>
      <div class="row tap" onclick="toggleWifi()"><span class="k">Change network</span>
        <span class="chev">›</span></div>
    </div>
    <div class="card" id="wifiPanel" hidden style="margin-top:10px"><div class="row col">
      <select id="fSsid" aria-label="Wi-Fi network"><option value="">Select a network…</option></select>
      <input type="password" id="fWifiPw" aria-label="Wi-Fi password" placeholder="Password" autocomplete="off">
      <div class="actions">
        <button class="btn" id="btnScanWifi" onclick="scanWifi()">Scan</button>
        <button class="btn primary" id="btnSaveWifi" onclick="saveWifi()">Connect</button>
      </div>
    </div></div>
    <p class="hint" id="wifiHint" hidden>Saving restarts the frame. If it cannot connect it
    reopens the <b>NowPlaying-Setup</b> network.</p>

    <div class="label">The picture</div>
    <div class="card">
      <div class="row col"><span class="k">Fill the screen</span>
        <select id="fFill" aria-label="How artwork fills the screen">
          <option value="1">Adaptive</option><option value="2">Never crop</option>
          <option value="3">Always fill</option><option value="0">Centred square</option>
        </select></div>
      <div class="row col"><span class="k">Render profile</span>
        <select id="fProfile" aria-label="Render profile"></select></div>
      <div class="row"><span class="k">Show artist and album</span>
        <label class="sw"><input type="checkbox" id="fTrackInfo"
          aria-label="Show artist and album on the frame"><span></span></label></div>
      <div class="row col"><span class="k">Background</span>
        <select id="fBgMode" aria-label="Background">
          <option value="2">Automatic</option><option value="1">Blurred artwork</option>
          <option value="0">Solid colour</option></select></div>
      <div class="row col"><span class="k">Background tone</span>
        <select id="fBgStyle" aria-label="Background tone">
          <option value="0">Darken</option><option value="1">Lighten</option></select></div>
    </div>
    <p class="hint">Album art is square and the frame is not, so a centred square covers only
    60&#37; of it. <b>Adaptive</b> enlarges each sleeve as far as it can before the crop would cut
    into the artwork, then blends the rest out to the edges. The background options apply to the
    centred-square layout, which is also used whenever the artist and album overlay is on.</p>

    <div class="label">Panel care</div>
    <div class="card">
      <div class="row col"><span class="k">Minimum time between redraws</span>
        <div class="slider"><input type="range" id="tMinRef" min="0" max="180" step="15"
          aria-label="Minimum time between redraws"><span class="val" id="tMinRefV"></span></div></div>
      <div class="row col"><span class="k">Quiet hours</span>
        <div class="slider"><select id="fQuietStart" aria-label="Quiet hours start"></select>
          <select id="fQuietEnd" aria-label="Quiet hours end"></select></div></div>
      <div class="row col"><span class="k">Hours ahead of UTC</span>
        <div class="slider"><input type="range" id="tUtc" min="-12" max="14" step="1"
          aria-label="Hours ahead of UTC"><span class="val" id="tUtcV"></span></div></div>
    </div>
    <p class="hint">Each redraw takes 20&ndash;25 seconds and e-ink panels have a finite refresh
    life. During quiet hours the frame is left alone entirely &mdash; e-ink holds its image with
    no power. Set both hours the same to disable.</p>

    <div class="label">Timing</div>
    <div class="card">
      <div class="row col"><span class="k">Check Sonos every</span>
        <div class="slider"><input type="range" id="tPoll" min="5" max="60" step="5"
          aria-label="Sonos check interval"><span class="val" id="tPollV"></span></div></div>
      <div class="row col"><span class="k">Re-identify vinyl every</span>
        <div class="slider"><input type="range" id="tVinyl" min="1" max="30"
          aria-label="Vinyl re-identify interval"><span class="val" id="tVinylV"></span></div></div>
      <div class="row col"><span class="k">Pause after failed matches</span>
        <div class="slider"><input type="range" id="tCool" min="1" max="15"
          aria-label="Pause after failed matches"><span class="val" id="tCoolV"></span></div></div>
      <div class="row col"><span class="k">Rotate artwork when idle</span>
        <div class="slider"><input type="range" id="tIdle" min="1" max="30"
          aria-label="Idle rotation interval"><span class="val" id="tIdleV"></span></div></div>
    </div>
    <p class="hint">The frame subscribes to Sonos for instant updates; this poll is the fallback
    for when an event is missed.</p>

    <div class="label">Vinyl</div>
    <div class="card"><div class="row col"><span class="k">Shazam API key</span>
      <input type="password" id="fShazam" aria-label="Shazam API key" autocomplete="off"></div></div>
    <p class="hint">Lets the frame identify records playing through the turntable input.
    Digital playback does not need one.</p>

    <div class="label">Security</div>
    <div class="card"><div class="row col"><span class="k">Portal password</span>
      <input type="password" id="fPortalPw" aria-label="Portal password" autocomplete="off"></div></div>
    <p class="hint">Username <b>admin</b>. Without a password, anyone on your network can change
    these settings.</p>

    <div style="margin:24px 0 8px">
      <button class="btn primary" id="btnSave" onclick="saveSettings()">Save changes</button>
    </div>

    <div class="label">Frame</div>
    <div class="card">
      <div class="row"><span class="k">Address</span><span class="v" id="dIp">—</span></div>
      <div class="row"><span class="k">Polling</span><span class="v" id="dPoll">—</span></div>
      <div class="row"><span class="k">Uptime</span><span class="v" id="dUp">—</span></div>
      <div class="row"><span class="k">Redraws</span><span class="v" id="dRefresh">—</span></div>
      <div class="row"><span class="k">Free memory</span><span class="v" id="dHeap">—</span></div>
      <div class="row"><span class="k">Last restart</span><span class="v" id="dReset">—</span></div>
    </div>

    <div class="label">Activity</div>
    <div class="card">
      <details>
        <summary><span id="logSummary">Recent events</span></summary>
        <div class="log" id="log"></div>
      </details>
    </div>

    <div class="label">Diagnostics</div>
    <div class="card">
      <div class="row tap" onclick="act(this,'/api/test-colors','Colour bars sent')">
        <span class="k">Colour bars</span><span class="chev">›</span></div>
      <div class="row tap" onclick="act(this,'/api/test-dither','Dither test sent')">
        <span class="k">Dither test pattern</span><span class="chev">›</span></div>
      <div class="row tap" onclick="act(this,'/api/test-calibration','Calibration card sent')">
        <span class="k">Palette calibration card</span><span class="chev">›</span></div>
      <div class="row tap" onclick="location.href='/api/last-audio'">
        <span class="k">Download last recording</span><span class="chev">›</span></div>
    </div>
    <p class="hint">Test patterns stay up for 30 minutes so they can be photographed.
    <b>Redraw</b> on the main screen releases the frame early.</p>

    <div class="label">Firmware</div>
    <div class="card"><div class="row col">
      <input type="file" id="fw" accept=".bin" aria-label="Firmware file">
      <button class="btn" id="btnUpload" onclick="upload()">Install update</button>
      <div id="fwProg" hidden><div class="bar2"><i id="fwBar"></i></div>
        <div class="hint" id="fwMsg" style="margin-top:8px"></div></div>
    </div></div>
    <p class="hint">The frame verifies the image before switching to it and restarts when
    finished. If the upload fails the current firmware keeps running. Do not remove power
    during the update.</p>
  </div>
</div>

<div id="scrim" onclick="closeSheet()"></div>
<div class="sheet" id="sheet" role="dialog" aria-modal="true" aria-labelledby="sheetTitle">
  <div class="grip"></div>
  <h3 id="sheetTitle"></h3>
  <p class="sub" id="sheetSub"></p>
  <div class="opts" id="sheetOpts"></div>
</div>
<div id="toasts" role="status" aria-live="polite"></div>

<script>
"use strict";
const $ = s => document.querySelector(s);
const $$ = s => Array.from(document.querySelectorAll(s));
const esc = s => String(s).replace(/[&<>"]/g, c =>
  ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));

function toast(msg, kind){
  const el = document.createElement('div');
  el.className = 'toast' + (kind ? ' ' + kind : '');
  el.textContent = msg;
  $('#toasts').appendChild(el);
  setTimeout(() => { el.classList.add('out'); setTimeout(() => el.remove(), 260); }, 2600);
}

async function api(path, opts){
  const r = await fetch(path, opts);
  if(!r.ok) throw new Error('HTTP ' + r.status);
  const t = r.headers.get('content-type') || '';
  return t.includes('json') ? r.json() : r.text();
}
const post = (p, b) => api(p, b
  ? {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:b}
  : {method:'POST'});
const postJson = (p, o) => api(p, {method:'POST',
  headers:{'Content-Type':'application/json'}, body: JSON.stringify(o)});

async function act(el, path, msg){
  const b = el.classList.contains('btn') ? el : null;
  if(b){ b.classList.add('busy'); b.disabled = true; }
  try { await post(path); toast(msg, 'ok'); }
  catch(e){ toast('Failed: ' + e.message, 'err'); }
  finally { if(b){ b.classList.remove('busy'); b.disabled = false; } }
}

/* ── Screen stack ───────────────────────────────────────── */
// Now Playing is the root and never moves. History and Settings slide over it
// and come back with Done, so there is one main view rather than four peers.
let openName = null;
function openScreen(name){
  openName = name;
  $('#' + name).classList.add('open');
  history.pushState({screen:name}, '');
  if(name === 'history') loadHistory();
  if(name === 'settings'){ loadSettings(); loadLog(); }
}
function closeScreen(){
  if(openName) history.back(); else dismiss();
}
function dismiss(){
  if(!openName) return;
  $('#' + openName).classList.remove('open');
  openName = null;
}
addEventListener('popstate', dismiss);
addEventListener('keydown', e => {
  if(e.key !== 'Escape') return;
  if($('#sheet').classList.contains('show')) closeSheet(); else closeScreen();
});
$$('.screen').forEach(s => s.addEventListener('scroll', () => {
  const bar = s.querySelector('.bar');
  if(bar) bar.classList.toggle('scrolled', s.scrollTop > 4);
}, {passive:true}));

/* ── Now Playing ────────────────────────────────────────── */
const STATE_TEXT = {BOOT:'Starting up', IDLE:'Nothing playing', DIGITAL:'Playing from Sonos',
                    VINYL:'Listening to vinyl', ERROR:'Error', SETUP:'Setup mode'};
const RESET = {1:'Power on', 3:'Software restart', 4:'Watchdog', 5:'Interrupt watchdog',
               6:'Task watchdog', 7:'Watchdog', 8:'Deep sleep', 9:'Brownout', 12:'CPU reset'};
let view = 'panel', seq = -1, lastSrc = null;

function toggleView(){
  view = (view === 'panel') ? 'source' : 'panel';
  $('#viewTag').hidden = (view === 'panel');
  $('#frame').setAttribute('aria-label', view === 'panel'
    ? 'Show the original artwork instead' : 'Show what is on the frame');
  seq = -1;
  if(view === 'source') show(lastSrc); else show();
}
function show(url){
  const img = $('#art');
  if(view === 'panel') url = '/api/display/current.bmp?v=' + seq;
  if(!url){ img.classList.remove('on'); $('#ph').hidden = false; return; }
  img.dataset.src = url;
  img.onload  = () => { img.classList.add('on'); $('#ph').hidden = true; };
  img.onerror = () => { img.classList.remove('on'); $('#ph').hidden = false; };
  img.src = url;
}

function fmtUptime(s){
  const d = Math.floor(s/86400), h = Math.floor(s%86400/3600), m = Math.floor(s%3600/60);
  return d ? d+'d '+h+'h' : h ? h+'h '+m+'m' : m+'m '+(s%60)+'s';
}

async function tick(){
  let d;
  try { d = await api('/api/status'); }
  catch(e){
    $('#statusRow').hidden = false;
    $('#npStatus').textContent = 'Frame offline';
    $('#dot').className = 'dot warn';
    return;
  }

  const st = d.state_name || 'IDLE';
  $('#npTitle').textContent   = d.title  || (st === 'IDLE' ? 'Nothing playing' : ' ');
  $('#npArtist').textContent  = d.artist || ' ';
  $('#npAlbum').textContent   = d.album  || ' ';
  $('#npRelease').textContent = d.release || '';

  // Say something only when there is something to say. Playing normally is the
  // expected case and needs no caption; the exceptions are worth a line, but
  // only while they apply.
  let msg = '', warn = false;
  if(d.quiet){ msg = 'Quiet hours — frame paused'; warn = true; }
  else if(d.display_hold_sec > 0){
    msg = 'Test pattern held, ' + Math.ceil(d.display_hold_sec/60) + ' min left'; warn = true; }
  else if(d.cooldown_remaining_sec > 0){
    msg = 'Paused ' + Math.ceil(d.cooldown_remaining_sec/60) + ' min after failed matches'; warn = true; }
  else if(d.retry_in_sec > 0) msg = 'Retrying in ' + d.retry_in_sec + 's';
  else if(st === 'ERROR'){ msg = 'Error'; warn = true; }
  else if(st === 'VINYL') msg = 'Listening to vinyl';
  else if(st === 'SETUP'){ msg = 'Setup mode'; warn = true; }

  $('#statusRow').hidden = !msg;
  $('#npStatus').textContent = msg;
  $('#dot').className = 'dot' + (warn ? ' warn' : (st === 'VINYL' ? ' on' : ''));

  if(view === 'panel'){
    if(d.refreshes !== undefined && d.refreshes !== seq){ seq = d.refreshes; show(); }
  } else if(d.art_url){
    lastSrc = d.art_url;
    if($('#art').dataset.src !== lastSrc) show(lastSrc);
  }
  $('#artDesc').textContent = d.artist
    ? 'Frame showing ' + d.artist + (d.album ? ', ' + d.album : '') : '';

  $('#dIp').textContent = d.ip || '—';
  $('#dPoll').textContent = d.poll_ip || '—';
  $('#dUp').textContent = fmtUptime(d.uptime || 0);
  if(d.refreshes !== undefined) $('#dRefresh').textContent = d.refreshes.toLocaleString();
  if(d.free_heap !== undefined) $('#dHeap').textContent = Math.round(d.free_heap/1024) + ' KB';
  if(d.reset_reason !== undefined)
    $('#dReset').textContent = RESET[d.reset_reason] || ('code ' + d.reset_reason);
}

async function loadLog(){
  try {
    const l = await api('/api/log');
    $('#logSummary').textContent = l.length ? l[0].m.slice(0, 42) : 'No activity yet';
    $('#log').innerHTML = l.map(e => {
      const t = e.t, hh = Math.floor(t/3600), mm = Math.floor(t%3600/60);
      const stamp = (hh ? hh+'h' : '') + String(mm).padStart(2,'0') + 'm'
                  + String(t%60).padStart(2,'0') + 's';
      return '<div><time>'+stamp+'</time><span>'+esc(e.m)+'</span></div>';
    }).join('');
  } catch(e){}
}

/* ── History ────────────────────────────────────────────── */
let hLoaded = false, items = [];
const io = new IntersectionObserver(es => es.forEach(e => {
  if(!e.isIntersecting) return;
  const img = e.target;
  img.src = img.dataset.src;
  img.onload = () => img.classList.add('loaded');
  io.unobserve(img);
}), {rootMargin:'250px'});

async function loadHistory(force){
  if(hLoaded && !force) return;
  try { items = await api('/api/history'); } catch(e){ return; }
  hLoaded = true;
  const pin = items.filter(i => i.pin), rest = items.filter(i => !i.pin);
  $('#hEmpty').hidden  = items.length > 0;
  $('#hPinned').hidden = pin.length === 0;
  $('#hAll').hidden    = rest.length === 0;
  $('#hCount').textContent = rest.length + (rest.length === 1 ? ' cover' : ' covers');
  $('#gridPinned').innerHTML = pin.map(tile).join('');
  $('#gridAll').innerHTML    = rest.map(tile).join('');
  $$('.tile img[data-src]').forEach(i => io.observe(i));
}

function tile(i){
  const on = i.on !== false;
  const name = [i.a, i.al || i.t].filter(Boolean).join(' — ');
  return '<button class="tile'+(on?'':' off')+'" data-f="'+esc(i.f)+'"'
       + ' aria-label="'+esc(name || i.f)+(on?'':', excluded from rotation')+'">'
       + '<img data-src="/api/history/image?f='+encodeURIComponent(i.f)+'" alt="">'
       + (i.pin ? '<div class="badge"><svg viewBox="0 0 24 24" fill="currentColor" aria-hidden="true">'
                + '<path d="M16 3v6l3 3v2h-6v7l-1 1-1-1v-7H5v-2l3-3V3z"/></svg></div>' : '')
       + '</button>';
}

let sheetFile = null;
document.addEventListener('click', e => {
  const t = e.target.closest('.tile[data-f]'); if(t) openSheet(t.dataset.f);
  const o = e.target.closest('.opt[data-action]'); if(o) libAct(o.dataset.action);
});

function openSheet(f){
  const it = items.find(i => i.f === f); if(!it) return;
  sheetFile = f;
  $('#sheetTitle').textContent = it.a || 'Unknown artist';
  $('#sheetSub').textContent   = it.al || it.t || '';
  const on = it.on !== false;
  $('#sheetOpts').innerHTML = [
    opt('show','Show on the frame','<path d="M4 5h16v11H4zM9 20h6M12 16v4"/>'),
    opt('toggle', on ? 'Exclude from rotation' : 'Include in rotation',
      on ? '<path d="M2 12s4-7 10-7 10 7 10 7-4 7-10 7S2 12 2 12z"/><path d="M3 3l18 18"/>'
         : '<path d="M2 12s4-7 10-7 10 7 10 7-4 7-10 7S2 12 2 12z"/><circle cx="12" cy="12" r="3"/>'),
    opt('pin', it.pin ? 'Unpin' : 'Pin to keep',
      '<path d="M16 3v6l3 3v2h-6v7l-1 1-1-1v-7H5v-2l3-3V3z"/>'),
    opt('delete','Delete','<path d="M4 7h16M9 7V5h6v2M6 7l1 13h10l1-13"/>', true)
  ].join('');
  $('#scrim').classList.add('show'); $('#sheet').classList.add('show');
}
function opt(a,t,p,danger){
  return '<button class="opt'+(danger?' danger':'')+'" data-action="'+a+'">'
       + '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" '
       + 'stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">'+p+'</svg>'+t+'</button>';
}
function closeSheet(){
  $('#scrim').classList.remove('show'); $('#sheet').classList.remove('show');
}

async function libAct(action){
  const f = sheetFile, it = items.find(i => i.f === f);
  closeSheet(); if(!it) return;
  const q = 'f=' + encodeURIComponent(f);
  try {
    if(action === 'show'){
      await post('/api/history/show', q); toast('Sending to the frame', 'ok'); closeScreen();
    } else if(action === 'toggle'){
      const on = it.on !== false;
      await post('/api/history/toggle', q + '&on=' + (on ? '0' : '1'));
      toast(on ? 'Excluded from rotation' : 'Included in rotation', 'ok'); loadHistory(true);
    } else if(action === 'pin'){
      await post('/api/history/pin', q + '&pin=' + (it.pin ? '0' : '1'));
      toast(it.pin ? 'Unpinned' : 'Pinned', 'ok'); loadHistory(true);
    } else if(action === 'delete'){
      await post('/api/history/delete', q); toast('Deleted', 'ok'); loadHistory(true);
    }
  } catch(e){ toast('Failed: ' + e.message, 'err'); }
}

/* ── Settings ───────────────────────────────────────────── */
let sLoaded = false;
[['tPoll','s'],['tVinyl',' min'],['tCool',' min'],['tIdle',' min'],
 ['tMinRef','s'],['tUtc','h']].forEach(([id,suffix]) => {
  const el = $('#'+id);
  el.addEventListener('input', () => { $('#'+id+'V').textContent = el.value + suffix; });
});

async function loadSettings(force){
  if(sLoaded && !force) return;
  try {
    const d = await api('/api/settings');
    sLoaded = true;
    const sel = $('#fSpeaker');
    sel.innerHTML = '<option value="">No speaker selected</option>';
    if(d.sonos_name){
      const o = document.createElement('option');
      o.value = o.textContent = d.sonos_name; o.dataset.ip = d.sonos_ip || '';
      sel.appendChild(o); sel.value = d.sonos_name;
    }
    $('#fShazam').value = ''; $('#fPortalPw').value = '';
    $('#fShazam').placeholder = d.shazam_api_key_set ? 'Set — leave blank to keep' : 'Not set';
    $('#fPortalPw').placeholder = d.portal_password_set ? 'Set — leave blank to keep' : 'None';

    const set = (id,v,sfx) => { $('#'+id).value = v; $('#'+id+'V').textContent = v + sfx; };
    set('tPoll',  Math.round((d.sonos_poll_ms||10000)/1000), 's');
    set('tVinyl', Math.round((d.vinyl_recheck_ms||600000)/60000), ' min');
    set('tCool',  Math.round((d.no_match_cooldown_ms||300000)/60000), ' min');
    set('tIdle',  Math.round((d.idle_gallery_ms||300000)/60000), ' min');
    set('tMinRef',Math.round((d.min_refresh_ms||45000)/1000), 's');
    set('tUtc',   d.utc_offset_hours || 0, 'h');

    $('#fTrackInfo').checked = !!d.show_track_info;
    $('#fFill').value    = d.fill_mode !== undefined ? d.fill_mode : 1;
    $('#fBgMode').value  = d.bg_mode  !== undefined ? d.bg_mode  : 2;
    $('#fBgStyle').value = d.bg_style !== undefined ? d.bg_style : 0;
    for(const id of ['fQuietStart','fQuietEnd']){
      const s2 = $('#'+id);
      if(!s2.options.length) s2.innerHTML = Array.from({length:24}, (_,i) =>
        '<option value="'+i+'">'+String(i).padStart(2,'0')+':00</option>').join('');
    }
    $('#fQuietStart').value = d.quiet_start_hour || 0;
    $('#fQuietEnd').value   = d.quiet_end_hour || 0;

    try {
      const profs = await api('/api/profiles');
      $('#fProfile').innerHTML = profs.map(p =>
        '<option value="'+p.id+'">'+esc(p.name)+'</option>').join('');
      $('#fProfile').value = d.render_profile !== undefined ? d.render_profile : 1;
    } catch(e){}
    try { const w = await api('/api/wifi');
      $('#wifiCurrent').textContent = w.ssid || 'Not configured'; } catch(e){}
  } catch(e){ toast('Could not load settings', 'err'); }
}

function toggleWifi(){
  const p = $('#wifiPanel');
  p.hidden = !p.hidden; $('#wifiHint').hidden = p.hidden;
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
    toast(label + ' timed out', 'err'); return null;
  } catch(e){ toast('Scan failed', 'err'); return null; }
  finally { btn.classList.remove('busy'); btn.disabled = false; }
}

async function scanWifi(){
  const n = await pollScan('/api/wifi/scan', $('#btnScanWifi'), 'Wi-Fi scan');
  if(!n) return;
  n.sort((a,b) => b.rssi - a.rssi);
  $('#fSsid').innerHTML = '<option value="">Select a network…</option>' + n.map(x =>
    '<option value="'+esc(x.ssid)+'">'+esc(x.ssid)+(x.open?' (open)':'')+'</option>').join('');
  toast(n.length + ' networks found');
}

async function saveWifi(){
  const ssid = $('#fSsid').value;
  if(!ssid){ toast('Choose a network first', 'err'); return; }
  const b = $('#btnSaveWifi'); b.classList.add('busy'); b.disabled = true;
  try {
    await postJson('/api/wifi', {ssid, password: $('#fWifiPw').value});
    toast('Restarting to join ' + ssid, 'ok');
  } catch(e){ toast('Failed: ' + e.message, 'err'); }
  finally { b.classList.remove('busy'); b.disabled = false; }
}

async function scanSonos(){
  const found = await pollScan('/api/sonos/scan', $('#btnScanSonos'), 'Speaker scan');
  if(!found) return;
  const sel = $('#fSpeaker'), prev = sel.value;
  sel.innerHTML = '<option value="">No speaker selected</option>' + found.map(d =>
    '<option value="'+esc(d.name)+'" data-ip="'+esc(d.ip)+'">'+esc(d.name)+'</option>').join('');
  if(prev) sel.value = prev;
  toast(found.length ? found.length+' speakers found' : 'No speakers found',
        found.length ? 'ok' : 'err');
}

async function saveSettings(){
  const b = $('#btnSave'); b.classList.add('busy'); b.disabled = true;
  const sel = $('#fSpeaker'), o = sel.options[sel.selectedIndex];
  const body = {
    sonos_name: sel.value, sonos_ip: (o && o.dataset.ip) || '',
    sonos_poll_ms: +$('#tPoll').value * 1000,
    vinyl_recheck_ms: +$('#tVinyl').value * 60000,
    no_match_cooldown_ms: +$('#tCool').value * 60000,
    idle_gallery_ms: +$('#tIdle').value * 60000,
    min_refresh_ms: +$('#tMinRef').value * 1000,
    quiet_start_hour: +$('#fQuietStart').value,
    quiet_end_hour: +$('#fQuietEnd').value,
    utc_offset_hours: +$('#tUtc').value,
    show_track_info: $('#fTrackInfo').checked,
    fill_mode: +$('#fFill').value,
    bg_mode: +$('#fBgMode').value,
    bg_style: +$('#fBgStyle').value,
    render_profile: +($('#fProfile').value || 1)
  };
  if($('#fShazam').value)   body.shazam_api_key  = $('#fShazam').value;
  if($('#fPortalPw').value) body.portal_password = $('#fPortalPw').value;
  try {
    await postJson('/api/settings', body);
    toast('Settings saved', 'ok');
    sLoaded = false; loadSettings(true);
  } catch(e){ toast('Failed: ' + e.message, 'err'); }
  finally { b.classList.remove('busy'); b.disabled = false; }
}

function upload(){
  const f = $('#fw').files && $('#fw').files[0];
  if(!f){ toast('Choose a firmware file first', 'err'); return; }
  $('#fwProg').hidden = false;
  $('#fwMsg').textContent = 'Uploading ' + Math.round(f.size/1024) + ' KB…';
  const b = $('#btnUpload'); b.classList.add('busy'); b.disabled = true;
  const fd = new FormData(); fd.append('firmware', f, f.name);
  const x = new XMLHttpRequest();
  x.open('POST', '/api/update');
  x.upload.onprogress = e => {
    if(e.lengthComputable) $('#fwBar').style.width = Math.round(e.loaded/e.total*100)+'%';
  };
  x.onload = () => {
    b.classList.remove('busy'); b.disabled = false;
    if(x.status === 200){
      $('#fwBar').style.width = '100%';
      $('#fwMsg').textContent = 'Installed. The frame is restarting and will be back in about 20 seconds.';
      toast('Update installed', 'ok');
    } else {
      $('#fwMsg').textContent = 'Update failed (HTTP '+x.status+'). The current firmware is unchanged.';
      toast('Update failed', 'err');
    }
  };
  x.onerror = () => {
    b.classList.remove('busy'); b.disabled = false;
    $('#fwMsg').textContent = 'Connection lost during upload.';
    toast('Upload failed', 'err');
  };
  x.send(fd);
}

/* ── Polling ────────────────────────────────────────────── */
let timer = null;
function start(){
  stop(); tick();
  timer = setInterval(() => { tick(); if(openName === 'settings') loadLog(); }, 3000);
}
function stop(){ if(timer) clearInterval(timer); timer = null; }
// Don't poll a device that may be on battery while the tab is hidden.
document.addEventListener('visibilitychange', () => document.hidden ? stop() : start());
start();
</script>
</body>
</html>
)rawliteral";
