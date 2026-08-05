#pragma once

/*
 * Dashboard served at "/".
 *
 * Kept in flash (PROGMEM) and streamed straight out with server.send_P() so
 * this ~7 KB page is never copied onto the heap - which matters on a board
 * that has to stay up for months without fragmenting itself to death.
 *
 * The page is self-contained: no CDN, no fonts, no external anything. It has
 * to work when the only thing on the network is the ESP32 itself.
 */
static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="color-scheme" content="dark light">
<title>AC Drain Controller</title>
<style>
:root{
--bg:#0a0e13;--card:#141b24;--sunk:#1c2632;--track:#222d3a;--line:#26333f;
--tx:#e8eff6;--dim:#8b9aaa;
--ok:#31c76a;--warn:#f0a92e;--bad:#ff4f52;--info:#3d9dff;--ac:var(--info);
--sh:0 10px 30px rgba(0,0,0,.4);
}
@media(prefers-color-scheme:light){
:root{--bg:#eceff4;--card:#fff;--sunk:#f4f6fa;--track:#e2e8f0;--line:#dfe5ed;
--tx:#0f1a24;--dim:#5c6b7a;--sh:0 10px 30px rgba(20,35,55,.14)}
}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--tx);
font:15px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,Helvetica,sans-serif;
-webkit-font-smoothing:antialiased;-webkit-tap-highlight-color:transparent}
main{max-width:520px;margin:0 auto;padding:20px 16px 40px}
.top{display:flex;align-items:flex-end;justify-content:space-between;margin-bottom:14px}
h1{margin:0;font-size:18px;font-weight:650;letter-spacing:-.2px}
h1 em{display:block;font-weight:600;font-size:10px;line-height:1.4;font-style:normal;color:var(--dim);
letter-spacing:1px;text-transform:uppercase;margin-bottom:3px}
.conn{display:flex;align-items:center;gap:7px;font-size:12px;color:var(--dim)}
.dot{width:9px;height:9px;border-radius:50%;background:var(--dim);transition:box-shadow .3s,background .3s}
.dot.up{background:var(--ok);box-shadow:0 0 0 4px rgba(49,199,106,.16)}
.dot.down{background:var(--bad);box-shadow:0 0 0 4px rgba(255,79,82,.16)}

.card{background:var(--card);border:1px solid var(--line);border-radius:16px}
.hero{padding:16px;display:flex;gap:16px}
.hero.s0{--ac:var(--info)}.hero.s1{--ac:var(--ok)}
.hero.s2{--ac:var(--warn)}.hero.s3{--ac:var(--warn)}
.hero.s4{--ac:var(--bad);border-color:var(--bad);animation:pulse 1.7s ease-in-out infinite}
@keyframes pulse{0%,100%{box-shadow:0 0 0 0 rgba(255,79,82,0)}
50%{box-shadow:0 0 0 5px rgba(255,79,82,.17)}}

.tank{position:relative;flex:0 0 74px;height:142px;overflow:hidden;
border:2px solid var(--line);border-radius:9px 9px 13px 13px;background:var(--sunk)}
.water{position:absolute;left:0;right:0;bottom:0;height:24%;background:var(--ac);
opacity:.82;transition:height .8s cubic-bezier(.33,1,.68,1)}
.water:after{content:"";position:absolute;left:0;right:0;top:0;height:2px;background:#fff;opacity:.5}
/* height:0 is load-bearing. Without it the label gives .mk ~13px of height and
   pushes border-top that far above the level it is meant to mark - which put the
   90% line at the very top edge and clipped its label out of the tank entirely.
   align-items:center then straddles the label across the zero-height line, and
   its chip background stops the dashes striking through the text. */
.mk{position:absolute;left:0;right:0;height:0;border-top:1px dashed var(--dim);opacity:.55;
display:flex;align-items:center;justify-content:flex-end;font-weight:700;font-size:9px;line-height:1;
letter-spacing:.4px;color:var(--dim)}
.mk b{margin-right:3px;padding:2px 3px;border-radius:3px;background:var(--sunk)}
.m70{bottom:70%}.m90{bottom:90%}
.mk.on{opacity:1;border-top-style:solid}
.m70.on{border-color:var(--warn);color:var(--warn)}
.m90.on{border-color:var(--bad);color:var(--bad)}

.hi{flex:1;min-width:0;display:flex;flex-direction:column;justify-content:center}
.lbl{font-weight:600;font-size:10px;line-height:1.4;letter-spacing:1px;text-transform:uppercase;color:var(--dim)}
.st{font-size:23px;font-weight:700;letter-spacing:-.4px;line-height:1.15;color:var(--ac);margin:1px 0 3px}
.sub{font-size:13px;color:var(--dim);min-height:19px}
.bar{margin-top:11px;height:6px;border-radius:99px;background:var(--track);overflow:hidden}
.bar i{display:block;height:100%;width:0;border-radius:99px;background:var(--ac);
transition:width .9s linear}
.brow{display:flex;justify-content:space-between;gap:8px;margin-top:6px;
font-size:11px;color:var(--dim);white-space:nowrap}

.banner{margin-top:12px;padding:12px 14px;border-radius:13px;font-size:13px;line-height:1.4;
background:rgba(255,79,82,.12);border:1px solid var(--bad);color:var(--bad)}
.banner b{display:block;font-size:11px;letter-spacing:.8px;text-transform:uppercase;margin-bottom:3px}
.hide{display:none!important}

.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin-top:12px}
@media(max-width:399px){.grid{grid-template-columns:repeat(2,1fr)}}
.tile{background:var(--card);border:1px solid var(--line);border-radius:13px;padding:11px 12px;min-width:0}
.tile span{display:block;font-weight:600;font-size:10px;line-height:1.4;letter-spacing:.6px;text-transform:uppercase;
color:var(--dim);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.tile b{display:block;font-size:15px;font-weight:650;margin-top:2px;
overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.g{color:var(--ok)}.a{color:var(--warn)}.r{color:var(--bad)}

.act{display:flex;gap:10px;margin-top:14px}
button{flex:1;padding:14px 10px;font-weight:600;font-size:15px;line-height:1;border-radius:13px;cursor:pointer;
border:1px solid var(--line);background:var(--card);color:var(--tx);
transition:transform .08s,opacity .2s}
button.p{background:var(--ok);border-color:var(--ok);color:#04160b}
button:active:not(:disabled){transform:scale(.985)}
button:disabled{opacity:.38;cursor:not-allowed}
.hint{margin-top:9px;font-size:12px;color:var(--dim);text-align:center;min-height:17px}

.sec{margin-top:20px;display:flex;align-items:baseline;justify-content:space-between;gap:10px}
.sec h2{margin:0;font-weight:600;font-size:11px;line-height:1.4;letter-spacing:1px;text-transform:uppercase;color:var(--dim)}
.sec a{font-size:11px;color:var(--info);text-decoration:none;font-weight:600}
.sec a:hover{text-decoration:underline}
.logwrap{margin-top:8px;border:1px solid var(--line);border-radius:14px;background:var(--card);
max-height:290px;overflow-y:auto;-webkit-overflow-scrolling:touch}
.row{display:flex;gap:10px;align-items:baseline;padding:9px 13px;border-bottom:1px solid var(--line)}
.row:last-child{border-bottom:0}
.row .t{flex:0 0 62px;font:500 11px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace;color:var(--dim)}
.row .b{flex:1;min-width:0;font-size:13px}
.row .b i{font-style:normal;color:var(--dim)}
.row .p{flex:0 0 6px;height:6px;border-radius:50%;background:var(--dim);margin-top:6px}
.row.ok .p{background:var(--ok)}.row.warn .p{background:var(--warn)}.row.bad .p{background:var(--bad)}
.row.bad .b{color:var(--bad);font-weight:600}
.empty{padding:16px 13px;font-size:13px;color:var(--dim);text-align:center}
.day{padding:6px 13px;background:var(--sunk);border-bottom:1px solid var(--line);
font-weight:600;font-size:10px;line-height:1.4;letter-spacing:.8px;text-transform:uppercase;color:var(--dim);
position:sticky;top:0}
.note{margin-top:8px;font-size:11px;line-height:1.6;color:var(--dim)}

footer{margin-top:18px;text-align:center;font-size:11px;line-height:1.8;color:var(--dim)}
footer code{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:11px}

.toast{position:fixed;left:50%;bottom:20px;z-index:9;max-width:92vw;
padding:12px 16px;border-radius:12px;font-size:13px;box-shadow:var(--sh);
background:var(--card);border:1px solid var(--line);color:var(--tx);
transform:translate(-50%,160%);transition:transform .3s cubic-bezier(.2,.9,.3,1)}
.toast.show{transform:translate(-50%,0)}
@media(prefers-reduced-motion:reduce){*{animation:none!important;transition:none!important}}
</style>
</head><body>
<main>
  <div class="top">
    <h1><em>Condensate drain</em>AC Drain Controller</h1>
    <div class="conn"><span class="dot" id="dot"></span><span id="ct">connecting</span><span id="ts"></span></div>
  </div>

  <section class="card hero" id="hero">
    <div class="tank">
      <div class="water" id="water"></div>
      <div class="mk m90" id="m90"><b>90%</b></div>
      <div class="mk m70" id="m70"><b>70%</b></div>
    </div>
    <div class="hi">
      <div class="lbl">Status</div>
      <div class="st" id="st">&mdash;</div>
      <div class="sub" id="sub">reading controller&hellip;</div>
      <div class="bar"><i id="bar"></i></div>
      <div class="brow"><span id="bl">&nbsp;</span><span id="br"></span></div>
    </div>
  </section>

  <div class="banner hide" id="al">
    <b>Not draining</b>
    The pump ran a full cycle and the water is still at 90%. It was stopped to
    protect it &mdash; check the drain line for a blockage.
  </div>

  <section class="grid">
    <div class="tile"><span>Pump</span><b id="tp">&mdash;</b></div>
    <div class="tile"><span>Manual switch</span><b id="tw">&mdash;</b></div>
    <div class="tile"><span>Pump runs</span><b id="tc">&mdash;</b></div>
    <div class="tile"><span>Overflows</span><b id="to">&mdash;</b></div>
    <div class="tile"><span>Total run</span><b id="tr">&mdash;</b></div>
    <div class="tile"><span>Uptime</span><b id="tu">&mdash;</b></div>
  </section>

  <section class="act">
    <button class="p" id="bon" disabled>Start pump</button>
    <button id="boff" disabled>Stop pump</button>
  </section>
  <div class="hint" id="hint"></div>

  <div class="sec">
    <h2>Activity log</h2>
    <a href="/api/log.csv" download>Download CSV</a>
  </div>
  <div class="logwrap" id="lw"><div class="empty">no events yet</div></div>
  <div class="note" id="ln"></div>

  <footer>
    <span id="fd">&nbsp;</span><br>
    <code id="ip">&mdash;</code> &middot; refreshes every 2s
  </footer>
</main>
<div class="toast" id="toast"></div>

<script>
const $ = id => document.getElementById(id);
const NAME = ['Idle', 'Auto cycle', 'Manual run', 'Switched off', 'Overflow'];
const SUB = [
  'Waiting for the 70% float',
  'Draining on the automatic cycle',
  'Manual override',
  'Manual switch is OFF — pump inhibited',
  'Water at 90% — clearing it'
];
const LOCK = [
  '', '', '',
  'Controls are locked — the manual switch is in the OFF position.',
  'Controls are locked — the overflow safety handler owns the pump.'
];
let fails = 0, busy = false;

function dur(s) {
  s = s > 0 ? s | 0 : 0;
  const h = s / 3600 | 0, m = (s / 60 | 0) % 60;
  return h ? h + 'h ' + m + 'm' : m ? m + 'm ' + (s % 60) + 's' : s % 60 + 's';
}

// A configured run length rather than a measured one: whole minutes read '4m',
// not the '4m 0s' dur() would give. The device sends these, so retiming a run
// updates this page too — nothing here states a duration of its own.
const runlen = s => s && s % 60 === 0 ? (s / 60 | 0) + 'm' : dur(s);

function set(id, v, cls) { const e = $(id); e.textContent = v; e.className = cls || ''; }

function link(up) {
  $('dot').className = 'dot ' + (up ? 'up' : 'down');
  $('ct').textContent = up ? 'live' : 'no connection';
}

function render(d) {
  const s = d.state, locked = s === 3 || s === 4;
  $('hero').className = 'card hero s' + s;
  $('st').textContent = NAME[s] || '?';
  // A manual run is the one state whose cap is worth spelling out — it is the
  // only one somebody started by hand and might expect to keep going.
  $('sub').textContent = (SUB[s] || '') +
    (s === 2 && d.duration ? ', ' + runlen(d.duration) + ' cap' : '');

  // Reeds are the only level information we have, so show what they prove:
  // below 70%, at 70%, or at 90%.
  $('water').style.height = (d.reed90 ? 90 : d.reed70 ? 70 : 24) + '%';
  $('m70').className = 'mk m70' + (d.reed70 ? ' on' : '');
  $('m90').className = 'mk m90' + (d.reed90 ? ' on' : '');

  let w = 0, l = 'Pump idle', r = '';
  if (d.pump && d.remaining >= 0) {
    w = 100 * (d.duration - d.remaining) / d.duration;
    l = 'Pumping'; r = dur(d.remaining) + ' left';
  } else if (d.pump) {
    w = 100; l = 'Pumping'; r = dur(d.elapsed) + ' — no time limit';
  } else if (s === 3 && d.remaining >= 0) {
    // Cycle frozen by the switch, not cancelled. Showing the held remainder is
    // the only way the operator can tell those two apart from the panel.
    w = 100 * (d.duration - d.remaining) / d.duration;
    l = 'Paused'; r = dur(d.remaining) + ' left on resume';
  } else if (s === 4) {
    l = 'Pump stopped'; r = 'not draining';
  }
  $('bar').style.width = w + '%';
  $('bl').textContent = l;
  $('br').textContent = r;

  $('al').className = 'banner' + (d.blocked ? '' : ' hide');
  set('tp', d.pump ? 'ON' : 'OFF', d.pump ? 'g' : '');
  set('tw', d.enabled ? 'ON' : 'OFF', d.enabled ? '' : 'a');
  set('tc', d.starts);
  set('to', d.overflows, d.overflows ? 'a' : '');
  set('tr', dur(d.pumpTotal));
  set('tu', dur(d.uptime));
  $('ts').textContent = d.rssi ? d.rssi + ' dBm' : '';
  $('ip').textContent = d.ip;
  $('fd').textContent =
    'Auto cycle: ' + runlen(d.autoDur) + ' per run, repeats while the 70% float is wet. ' +
    'Overflow runs up to ' + runlen(d.ovfDur) + ' before it is called a blockage.';

  $('bon').disabled = busy || locked || d.pump;
  $('boff').disabled = busy || locked || !d.pump;
  $('hint').textContent = LOCK[s] || '';
}

/*
 * Activity log.
 *
 * The controller is the single source of truth - nothing is kept in
 * localStorage, so every phone and laptop that opens this page sees the same
 * history rather than its own partial copy. We only ever ask for events newer
 * than the last sequence number we hold, so the 2-second poll normally returns
 * an empty array.
 */
const EV = [
  ['Powered on',             ''    ],   // 0
  ['Pump started',           'ok'  ],   // 1
  ['Pump stopped',           ''    ],   // 2
  ['Overflow — water at 90%','bad' ],   // 3
  ['Overflow cleared',       'warn'],   // 4
  ['Blockage — not draining','bad' ],   // 5
  ['WiFi lost',              'warn'],   // 6
  ['WiFi restored',          ''    ],   // 7
  ['Manual switch OFF',      'warn'],   // 8
  ['Manual switch ON',       ''    ],   // 9
  ['Firmware update',        'warn']    // 10
];
const CAUSE = ['', 'auto cycle', 'manual', 'manual switch', 'overflow', 'firmware update'];

let evs = [], lastSeq = 0, bootEpoch = 0, lostOld = false;

function evDate(sec) { return new Date((bootEpoch + sec) * 1000); }

function evTime(sec) {
  if (!bootEpoch) return '+' + dur(sec);     // clock not synced yet
  return evDate(sec).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

function evDetail(e) {
  const [, , code, cause, detail] = e;
  if (code === 1 && cause) return CAUSE[cause];
  if (code === 2 && detail) return 'ran ' + dur(detail);
  if (code === 4 && detail) return 'lasted ' + dur(detail);
  return '';
}

function renderLog() {
  const lw = $('lw');
  if (!evs.length) { lw.innerHTML = '<div class="empty">no events yet</div>'; return; }

  let html = '', day = '';
  // Newest first, and capped: the device holds 256 but nobody scrolls that far.
  for (const e of evs.slice(-80).reverse()) {
    const [, sec, code] = e;
    const [label, cls] = EV[code] || ['event', ''];

    if (bootEpoch) {
      const d = evDate(sec).toDateString();
      if (d !== day) {
        day = d;
        const today = new Date().toDateString();
        html += '<div class="day">' + (d === today ? 'Today' :
          evDate(sec).toLocaleDateString([], { weekday: 'short', day: '2-digit', month: 'short' })) +
          '</div>';
      }
    }
    const det = evDetail(e);
    html += '<div class="row ' + cls + '"><span class="p"></span>' +
            '<span class="t">' + evTime(sec) + '</span>' +
            '<span class="b">' + label + (det ? ' <i>· ' + det + '</i>' : '') + '</span></div>';
  }
  lw.innerHTML = html;

  $('ln').textContent = evs.length + ' event' + (evs.length === 1 ? '' : 's') +
    ' · held in RAM on the controller, last 256' +
    (lostOld ? ' · older entries have been overwritten' : '') +
    (bootEpoch ? '' : ' · times shown relative to boot until the clock syncs');
}

async function pollLog() {
  const r = await fetch('/api/log?since=' + lastSeq, { cache: 'no-store' });
  if (!r.ok) throw 0;
  const d = await r.json();

  // seq going backwards means the controller restarted: its ring is empty again,
  // so drop what we have rather than stitching two boots together.
  if (d.seq < lastSeq) { evs = []; lastSeq = 0; }

  // NTP lands a few seconds after boot, which turns every "+3m" label into a
  // real clock time - so a change here has to force a full re-render.
  const bootChanged = (d.boot || 0) !== bootEpoch;
  bootEpoch = d.boot || 0;
  if (d.lost) lostOld = true;
  if (d.ev.length) evs = evs.concat(d.ev);
  if (evs.length > 300) evs = evs.slice(-300);

  lastSeq = d.seq;
  if (d.ev.length || bootChanged) renderLog();   // otherwise nothing changed
}

async function poll() {
  if (document.hidden) return;               // don't wake the ESP32 for a hidden tab
  try {
    const r = await fetch('/api/status', { cache: 'no-store' });
    if (!r.ok) throw 0;
    render(await r.json());
    await pollLog();
    fails = 0;
    link(true);
  } catch (e) {
    if (++fails > 1) link(false);            // one dropped poll is not an outage
  }
}

let tt;
function toast(m) {
  const t = $('toast');
  t.textContent = m;
  t.className = 'toast show';
  clearTimeout(tt);
  tt = setTimeout(() => { t.className = 'toast'; }, 2800);
}

async function cmd(path) {
  if (busy) return;
  busy = true;
  $('bon').disabled = $('boff').disabled = true;
  try {
    const r = await fetch(path, { method: 'POST' });
    toast((await r.text()) || (r.ok ? 'Done' : 'Failed'));
  } catch (e) {
    toast('Command failed — no connection to the controller');
  }
  busy = false;
  poll();
}

$('bon').onclick = () => cmd('/pump/on');
$('boff').onclick = () => cmd('/pump/off');

document.addEventListener('visibilitychange', poll);
poll();
setInterval(poll, 2000);
</script>
</body></html>
)HTML";
