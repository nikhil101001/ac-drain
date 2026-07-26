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
h1 em{display:block;font:600 10px/1.4 inherit;font-style:normal;color:var(--dim);
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
.mk{position:absolute;left:0;right:0;border-top:1px dashed var(--dim);opacity:.5;
text-align:right;font:700 9px/1 inherit;letter-spacing:.5px;color:var(--dim)}
.mk b{display:inline-block;transform:translateY(-12px);padding-right:4px}
.m70{bottom:70%}.m90{bottom:90%}
.mk.on{opacity:1;border-top-style:solid}
.m70.on{border-color:var(--warn);color:var(--warn)}
.m90.on{border-color:var(--bad);color:var(--bad)}

.hi{flex:1;min-width:0;display:flex;flex-direction:column;justify-content:center}
.lbl{font:600 10px/1.4 inherit;letter-spacing:1px;text-transform:uppercase;color:var(--dim)}
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
.tile span{display:block;font:600 10px/1.4 inherit;letter-spacing:.6px;text-transform:uppercase;
color:var(--dim);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.tile b{display:block;font-size:15px;font-weight:650;margin-top:2px;
overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.g{color:var(--ok)}.a{color:var(--warn)}.r{color:var(--bad)}

.act{display:flex;gap:10px;margin-top:14px}
button{flex:1;padding:14px 10px;font:650 15px/1 inherit;border-radius:13px;cursor:pointer;
border:1px solid var(--line);background:var(--card);color:var(--tx);
transition:transform .08s,opacity .2s}
button.p{background:var(--ok);border-color:var(--ok);color:#04160b}
button:active:not(:disabled){transform:scale(.985)}
button:disabled{opacity:.38;cursor:not-allowed}
.hint{margin-top:9px;font-size:12px;color:var(--dim);text-align:center;min-height:17px}
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
    <div class="conn"><span class="dot" id="dot"></span><span id="ct">connecting</span></div>
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
    <div class="tile"><span>Pump starts</span><b id="tc">&mdash;</b></div>
    <div class="tile"><span>Total run</span><b id="tr">&mdash;</b></div>
    <div class="tile"><span>Uptime</span><b id="tu">&mdash;</b></div>
    <div class="tile"><span>WiFi</span><b id="ts">&mdash;</b></div>
  </section>

  <section class="act">
    <button class="p" id="bon" disabled>Start pump</button>
    <button id="boff" disabled>Stop pump</button>
  </section>
  <div class="hint" id="hint"></div>

  <footer>
    Auto cycle: 6 min per run, repeats while the 70% float is wet.<br>
    <code id="ip">&mdash;</code> &middot; refreshes every 2s
  </footer>
</main>
<div class="toast" id="toast"></div>

<script>
const $ = id => document.getElementById(id);
const NAME = ['Idle', 'Auto cycle', 'Manual run', 'Manual switch', 'Overflow'];
const SUB = [
  'Waiting for the 70% float',
  'Draining on the automatic cycle',
  'Manual override, 6 minute cap',
  'Rocker switch held closed, no time limit',
  'Water at 90% — clearing it'
];
const LOCK = [
  '', '', '',
  'Controls are locked while the manual rocker switch is closed.',
  'Controls are locked — the overflow safety handler owns the pump.'
];
let fails = 0, busy = false;

function dur(s) {
  s = s > 0 ? s | 0 : 0;
  const h = s / 3600 | 0, m = (s / 60 | 0) % 60;
  return h ? h + 'h ' + m + 'm' : m ? m + 'm ' + (s % 60) + 's' : s % 60 + 's';
}

function set(id, v, cls) { const e = $(id); e.textContent = v; e.className = cls || ''; }

function link(up) {
  $('dot').className = 'dot ' + (up ? 'up' : 'down');
  $('ct').textContent = up ? 'live' : 'no connection';
}

function render(d) {
  const s = d.state, locked = s === 3 || s === 4;
  $('hero').className = 'card hero s' + s;
  $('st').textContent = NAME[s] || '?';
  $('sub').textContent = SUB[s] || '';

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
  } else if (s === 4) {
    l = 'Pump stopped'; r = 'not draining';
  }
  $('bar').style.width = w + '%';
  $('bl').textContent = l;
  $('br').textContent = r;

  $('al').className = 'banner' + (d.blocked ? '' : ' hide');
  set('tp', d.pump ? 'ON' : 'OFF', d.pump ? 'g' : '');
  set('tw', d.manual ? 'CLOSED' : 'open', d.manual ? 'a' : '');
  set('tc', d.starts);
  set('tr', dur(d.pumpTotal));
  set('tu', dur(d.uptime));
  set('ts', d.rssi ? d.rssi + ' dBm' : '—',
      !d.rssi ? '' : d.rssi > -67 ? 'g' : d.rssi > -78 ? 'a' : 'r');
  $('ip').textContent = d.ip;

  $('bon').disabled = busy || locked || d.pump;
  $('boff').disabled = busy || locked || !d.pump;
  $('hint').textContent = LOCK[s] || '';
}

async function poll() {
  if (document.hidden) return;               // don't wake the ESP32 for a hidden tab
  try {
    const r = await fetch('/api/status', { cache: 'no-store' });
    if (!r.ok) throw 0;
    render(await r.json());
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
