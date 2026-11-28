// The pages the head serves. Kept out of the sketch file: the Arduino builder scans sketches for C functions and
// trips over the JavaScript in here.
#pragma once
#include <Arduino.h>

const char WAITPAGE[] PROGMEM = R"HTML(<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>X70</title>
<style>body{margin:0;background:#0d0f13;color:#e8ecf1;font:16px -apple-system,Segoe UI,Roboto,sans-serif;display:flex;min-height:100vh;align-items:center;justify-content:center}
.c{width:min(88vw,360px);background:#161a21;border:1px solid #2a313c;border-radius:16px;padding:22px}h2{margin:0 0 12px;font-size:18px;display:flex;gap:10px;align-items:center}
i{width:18px;height:18px;border-radius:50%;border:2px solid #ffb347;border-top-color:transparent;animation:s .8s linear infinite;flex:none}
p{color:#8e98a7;font-size:14px;line-height:1.45}@keyframes s{to{transform:rotate(360deg)}}</style>
<div class=c><h2><i></i><span>%HEAD%</span></h2><p id=m>This page comes back by itself. Nothing to do.</p><p id=n hidden>%NOTE%</p></div>
<script>var t0=Date.now(),g=function(i){return document.getElementById(i)};setTimeout(function k(){var c=new AbortController();setTimeout(function(){c.abort()},2000);
fetch('%PING%',{signal:c.signal,cache:'no-store'}).then(function(r){if(r.ok)location.replace('/');else throw 0}).catch(function(){var s=Math.round((Date.now()-t0)/1000);
g('m').textContent='Waiting for it to come back: '+s+' s';if(s>%LATE%)g('n').hidden=false;setTimeout(k,2000)})},6000)</script>)HTML";
String waitPage(const char *head, const char *note, const char *ping, int late) {
  String h = FPSTR(WAITPAGE); h.replace("%HEAD%", head); h.replace("%NOTE%", note); h.replace("%PING%", ping); h.replace("%LATE%", String(late)); return h;
}
const char PAGE[] PROGMEM = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover"><meta name="theme-color" content="#0d0f13"><title>X70 Head</title>
<style>
:root{--bg:#0d0f13;--s1:#161a21;--s2:#1e232c;--ln:#2a313c;--tx:#eef1f5;--dm:#8e98a7;--ac:#ffb347;--ok:#4cd08a;--aim:#5cc8ff;--bad:#ff5d52;--r:16px;color-scheme:dark}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;background:var(--bg);color:var(--tx);font:15px/1.4 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;-webkit-user-select:none;user-select:none;touch-action:manipulation}
.w{max-width:480px;margin:auto;padding:14px max(16px,env(safe-area-inset-left)) 150px max(16px,env(safe-area-inset-right))}
[hidden]{display:none!important}
.n{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-variant-numeric:tabular-nums}
.lb{font-size:11px;letter-spacing:.12em;text-transform:uppercase;color:var(--dm)}
/* header */
.hd{display:flex;align-items:center;justify-content:space-between;gap:8px}
.hd b{font-size:13px;letter-spacing:.18em}.hd b i{font-style:normal;color:var(--ac)}
.pl{display:flex;gap:6px}.pi{display:flex;align-items:center;gap:5px;font-size:11px;color:var(--dm);background:var(--s1);border:1px solid var(--ln);border-radius:99px;padding:4px 9px}
.pi::before{content:"";width:7px;height:7px;border-radius:50%;background:#4a5261}.pi.ok::before{background:var(--ok)}.pi.wn::before{background:var(--ac)}.pi.ok{color:var(--tx)}
#hero{margin:16px 0 4px;font-size:24px;font-weight:650;letter-spacing:-.01em;line-height:1.15;text-wrap:balance;min-height:28px}
#hero.t{color:var(--ok)}#hero.j{color:var(--ac)}#hero.b{color:var(--bad)}
#sub{color:var(--dm);font-size:13px;min-height:19px}
#al{margin-top:10px;display:flex;flex-direction:column;gap:6px}
.a{display:flex;align-items:center;gap:10px;background:#2a2113;border:1px solid #5b4520;color:#ffd9a0;border-radius:12px;padding:9px 12px;font-size:13px;text-align:left;width:100%}
.a.r{background:#2b1514;border-color:#6b2a25;color:#ffc2bd}.a span{flex:1}.a em{font-style:normal;font-weight:600;white-space:nowrap}
/* cards */
.c{background:var(--s1);border:1px solid var(--ln);border-radius:var(--r);padding:14px;margin-top:14px}
.ch{display:flex;justify-content:space-between;align-items:baseline;margin-bottom:10px}.ch small{color:var(--dm);font-size:12px}
button{font:inherit;color:inherit;background:var(--s2);border:1px solid var(--ln);border-radius:12px;padding:12px;cursor:pointer}
button:active{filter:brightness(1.25)}button:focus-visible,input:focus-visible{outline:2px solid var(--ac);outline-offset:2px}
.pr{background:var(--ac);border-color:var(--ac);color:#1a1205;font-weight:650}.gh{background:none}.dg{background:var(--bad);border-color:var(--bad);color:#fff;font-weight:650}
.bw{width:100%}.row{display:flex;gap:8px}.row>*{flex:1}
/* stage map */
#map{display:block;width:100%;height:auto;border-radius:10px;background:#0a0c10;touch-action:manipulation}
#mapx{position:relative}#mape{position:absolute;inset:0;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:10px;text-align:center;padding:16px;color:var(--dm);font-size:13px}
/* pad */
#pad,.pad{width:min(64vw,250px);aspect-ratio:1;margin:6px auto 12px;border-radius:50%;position:relative;touch-action:none;
background:radial-gradient(circle,#232a35 0,#161a21 68%,#11141a 100%);border:1px solid var(--ln);box-shadow:inset 0 0 0 10px #0f1217,inset 0 0 24px #000}
.pad::before,.pad::after{content:"";position:absolute;background:var(--ln)}.pad::before{left:50%;top:14%;bottom:14%;width:1px}.pad::after{top:50%;left:14%;right:14%;height:1px}
.kn{width:34%;height:34%;border-radius:50%;position:absolute;left:33%;top:33%;pointer-events:none;z-index:1;background:radial-gradient(circle at 35% 30%,#ffd08a,var(--ac) 55%,#b87416);box-shadow:0 6px 18px #000a}
.tr{min-height:20px;text-align:center;font-size:13px;color:var(--dm);margin:-4px 0 8px}.tr.b{color:var(--bad);font-weight:600}.tr.g{color:var(--ok)}
.sg{display:grid;grid-template-columns:repeat(3,1fr);background:var(--bg);border:1px solid var(--ln);border-radius:12px;padding:3px}
.sg button{border:0;background:none;padding:9px 4px;border-radius:9px;color:var(--dm);font-size:13px}.sg button.on{background:var(--s2);color:var(--tx);box-shadow:0 0 0 1px var(--ln)}
/* presets */
#pr{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}
.p{position:relative;text-align:left;padding:10px 10px 12px;min-height:64px;display:flex;flex-direction:column;justify-content:space-between;overflow:hidden}
.p u{text-decoration:none;font-size:11px;color:var(--dm)}.p s{text-decoration:none;font-size:14px;font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.p.e{border-style:dashed;background:none;color:var(--dm)}.p.e s{font-weight:400}.p.at{border-color:var(--aim);box-shadow:inset 0 0 0 1px var(--aim)}
.p i{position:absolute;right:0;top:0;width:34px;height:30px;display:flex;align-items:center;justify-content:center;font-style:normal;color:var(--dm);font-size:18px;line-height:1}
/* switch */
.sw{display:flex;align-items:center;justify-content:space-between;gap:12px;width:100%;background:none;border:0;padding:4px 0;text-align:left}
.sw span small{display:block;color:var(--dm);font-size:12px}.tg{flex:none;width:50px;height:30px;border-radius:99px;background:#39414e;position:relative;transition:.2s}
.tg::after{content:"";position:absolute;left:3px;top:3px;width:24px;height:24px;border-radius:50%;background:#fff;transition:.2s}.sw[aria-checked=true] .tg{background:var(--ok)}.sw[aria-checked=true] .tg::after{left:23px}
.lk{display:flex;justify-content:space-between;align-items:center;width:100%;margin-top:10px;text-align:left}.lk::after{content:"›";color:var(--dm);font-size:20px;line-height:1}
/* sliders */
.f{margin-top:14px}.f label{display:flex;justify-content:space-between;font-size:13px}.f label b{font-weight:600}.f small{display:block;color:var(--dm);font-size:12px;margin-top:2px}
input[type=range]{width:100%;accent-color:var(--ac);margin:8px 0 0;height:28px}
input[type=text],input[type=number]{width:100%;background:var(--bg);color:var(--tx);border:1px solid var(--ln);border-radius:10px;padding:11px;font:inherit;font-size:16px;-webkit-user-select:text;user-select:text}
/* checklist */
.ck{display:flex;align-items:center;gap:12px;width:100%;text-align:left;background:none;border:0;border-top:1px solid var(--ln);border-radius:0;padding:12px 0}
.ck:first-of-type{border-top:0}.ck i{flex:none;width:26px;height:26px;border-radius:50%;border:2px solid #4a5261;display:flex;align-items:center;justify-content:center;font-style:normal;font-size:12px;color:var(--dm)}
.ck.d i{background:var(--ok);border-color:var(--ok);color:#06210f;font-weight:700}.ck span{flex:1}.ck span small{display:block;color:var(--dm);font-size:12px}.ck::after{content:"›";color:var(--dm);font-size:20px}
.ring{--p:0;width:52px;height:52px;border-radius:50%;background:conic-gradient(var(--ok) calc(var(--p)*1%),#2a313c 0);display:flex;align-items:center;justify-content:center;flex:none}
.ring b{width:42px;height:42px;border-radius:50%;background:var(--s1);display:flex;align-items:center;justify-content:center;font-size:13px}
.hl{display:flex;gap:10px;align-items:flex-start;padding:8px 0;font-size:13px;border-top:1px solid var(--ln)}.hl:first-child{border-top:0}
.hl::before{content:"";flex:none;width:9px;height:9px;border-radius:50%;margin-top:5px;background:var(--ok)}.hl.wn::before{background:var(--ac)}.hl.bd::before{background:var(--bad)}.hl span{flex:1}.hl small{display:block;color:var(--dm)}
dl{display:grid;grid-template-columns:auto 1fr;gap:6px 14px;margin:0;font-size:13px}dt{color:var(--dm)}dd{margin:0;text-align:right}
pre{font:12px/1.5 ui-monospace,Menlo,Consolas,monospace;white-space:pre-wrap;color:#cfd6df;margin:10px 0;max-height:320px;overflow:auto;-webkit-user-select:text;user-select:text}
details summary{list-style:none;display:flex;justify-content:space-between;cursor:pointer}details summary::after{content:"+";color:var(--dm)}details[open] summary::after{content:"–"}
/* fixed bottom */
#bt{position:fixed;left:0;right:0;bottom:0;background:linear-gradient(#0d0f1300,var(--bg) 22%);padding:18px max(16px,env(safe-area-inset-left)) calc(8px + env(safe-area-inset-bottom)) max(16px,env(safe-area-inset-right));z-index:5}
#bt>div{max-width:448px;margin:auto}#stop{width:100%;padding:15px;font-size:17px;letter-spacing:.14em;border-radius:14px;box-shadow:0 8px 24px #ff5d5233}
nav{display:grid;grid-template-columns:repeat(3,1fr);margin-top:8px}nav a{color:var(--dm);text-decoration:none;text-align:center;font-size:12px;padding:6px 0 2px}
nav a svg{display:block;margin:0 auto 2px;width:22px;height:22px;fill:none;stroke:currentColor;stroke-width:1.7;stroke-linecap:round;stroke-linejoin:round}nav a.on{color:var(--ac)}nav a em{font-style:normal;display:inline-block;width:7px;height:7px;border-radius:50%;background:var(--ac);margin-left:4px;vertical-align:1px}
/* overlays */
.ov{position:fixed;inset:0;z-index:9;background:#07080bee;display:flex;align-items:flex-end;justify-content:center}
.sh{width:100%;max-width:480px;background:var(--s1);border:1px solid var(--ln);border-bottom:0;border-radius:20px 20px 0 0;padding:18px 16px calc(16px + env(safe-area-inset-bottom));text-align:left;animation:up .22s ease-out}
.sh h2{margin:0 0 6px;font-size:18px}.sh p{margin:0 0 12px;color:var(--dm);font-size:14px}.sh .st{display:flex;flex-direction:column;gap:8px;margin-top:12px}
@keyframes up{from{transform:translateY(24px);opacity:0}}@keyframes sp{to{transform:rotate(360deg)}}
@media(prefers-reduced-motion:reduce){*{animation:none!important;transition:none!important}}
#shv{z-index:12}#up{z-index:14}
#wz{background:var(--bg);align-items:stretch;overflow:auto}.wzi{width:100%;max-width:480px;margin:auto;padding:14px 16px calc(18px + env(safe-area-inset-bottom));display:flex;flex-direction:column;min-height:100%}
.wzh{display:flex;align-items:center;gap:12px}.wzh button{padding:8px 12px;border-radius:99px;font-size:13px}.wzp{flex:1;height:4px;border-radius:2px;background:var(--ln);overflow:hidden}.wzp b{display:block;height:100%;background:var(--ac);transition:width .3s}
#wzb{flex:1;display:flex;flex-direction:column}#wzb h2{font-size:22px;margin:18px 0 6px;letter-spacing:-.01em;text-wrap:balance}#wzb p{color:#c3cad4;margin:0 0 8px;font-size:15px}
#wzb .fig{margin:8px auto;width:min(70vw,230px)}#wzb .fig svg{display:block;width:100%;height:auto}.wzf{margin-top:auto;display:flex;flex-direction:column;gap:8px;padding-top:12px}
.rd{display:flex;justify-content:center;gap:18px;font-size:13px;color:var(--dm);margin:2px 0 6px}.rd b{color:var(--tx);font-weight:600}
.ch3{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}.ch3 button.on,.opt.on{border-color:var(--ac);box-shadow:inset 0 0 0 1px var(--ac)}
.opt{display:block;width:100%;text-align:left;margin-top:8px}.opt b{display:block}.opt small{color:var(--dm);font-size:13px}
.tip{background:var(--s1);border:1px solid var(--ln);border-radius:12px;padding:10px 12px;font-size:13px;color:var(--dm);margin-top:6px}
/* update */
.us{display:flex;align-items:center;gap:10px;padding:7px 0;color:#6b7686;flex-wrap:wrap}.us i{width:18px;height:18px;border-radius:50%;border:2px solid #3a4452;flex:none}
.us.now{color:var(--tx)}.us.now i{border-color:var(--ac);border-top-color:transparent;animation:sp .8s linear infinite}.us.ok{color:#9fdcb5}.us.ok i{background:var(--ok);border-color:var(--ok)}.us.bad{color:#ff8a80}.us.bad i{background:var(--bad);border-color:var(--bad)}
.bar{flex-basis:100%;height:8px;background:var(--s2);border-radius:4px;overflow:hidden;margin-left:28px}.bar b{display:block;height:100%;width:0;background:var(--ac);transition:width .4s}
.spin{display:inline-block;width:11px;height:11px;border-radius:50%;border:2px solid var(--ac);border-top-color:transparent;animation:sp .8s linear infinite;vertical-align:-2px}
#um{font-size:13px;color:var(--dm);min-height:34px;margin:10px 0 6px}
</style></head><body>
<div id="up" class="ov" hidden><div class="sh"><h2 id="ut"></h2>
<div class="us" id="u1"><i></i>Restarting into update mode</div>
<div class="us" id="u2"><i></i>Connecting to GitHub</div>
<div class="us" id="u3"><i></i><span>Downloading <span id="upc"></span></span><div class="bar"><b id="ub"></b></div></div>
<div class="us" id="u4"><i></i>Installing and restarting</div>
<div class="us" id="u5"><i></i><span id="u5t">Head running the new build</span></div>
<div class="us" id="u6"><i></i><span id="uj">Joystick box</span></div>
<p id="um"></p><button id="ux" class="bw" hidden>Close</button></div></div>
<div id="shv" class="ov" hidden><div class="sh"><h2 id="sht"></h2><p id="shp"></p><input id="shi" type="text" maxlength="14" hidden><div class="st" id="shb"></div></div></div>
<div id="wz" class="ov" hidden><div class="wzi"><div class="wzh"><button id="wzk" class="gh">Back</button><div class="wzp"><b id="wzbar"></b></div><button id="wzx" class="gh">Close</button></div><div id="wzb"></div></div></div>

<div class="w">
<div class="hd"><b>X70 <i>HEAD</i></b><div class="pl"><span class="pi" id="cO">OBS</span><span class="pi" id="cJ">Joystick</span><span class="pi" id="cW">Wi-Fi</span></div></div>
<div id="hero">Connecting…</div><div id="sub"></div><div id="al"></div>

<section id="t-control">
<div class="c"><div class="ch"><span class="lb">Stage map</span><small id="mh">double-tap to aim</small></div>
<div id="mapx"><svg id="map" viewBox="0 0 320 170" role="img" aria-label="Where the camera is aimed within its travel"></svg>
<div id="mape" hidden><span>The map shows where the camera can aim once its travel limits are set.</span><button class="pr" onclick="wzOpen()">Start guided setup</button></div></div></div>

<div class="c"><div class="ch"><span class="lb">Joystick</span><small id="jh"></small></div>
<div id="pad" class="pad"><div class="kn"></div></div><div class="tr"></div>
<div class="sg" id="feel"><button data-v="0.2">Precise</button><button data-v="0.5">Normal</button><button data-v="1">Fast</button></div></div>

<div class="c"><div class="ch"><span class="lb">Presets</span><small>tap to go · ⋯ to edit</small></div><div id="pr"></div></div>

<div class="c"><button class="sw" id="trk" role="switch"><span><b>Follow the presenter</b><small id="trs"></small></span><span class="tg"></span></button>
<button class="lk" onclick="location.href='/tune'">Tracking settings</button></div>
</section>

<section id="t-setup" hidden>
<div class="c"><div style="display:flex;gap:14px;align-items:center;margin-bottom:6px"><div class="ring" id="ring"><b class="n" id="ringn"></b></div><div><b id="sut" style="font-size:17px"></b><div id="sus" style="color:var(--dm);font-size:13px"></div></div></div>
<div id="cks"></div><button class="pr bw" id="sug" style="margin-top:8px"></button></div>
<div class="c"><div class="ch"><span class="lb">Movement feel</span><small id="recn"></small></div>
<div class="ch3" id="fl3"><button data-f="0">Calm</button><button data-f="1">Balanced</button><button data-f="2">Lively</button></div>
<details style="margin-top:12px"><summary><span style="font-size:13px;color:var(--dm)">Exact numbers</span></summary>
<div class="f"><label>Top speed <b class="n" id="mv"></b></label><input id="max" type="range" min="10" max="300" step="5"><small>The ceiling for everything: joystick, presets and tracking.</small></div>
<div class="f"><label>Joystick speed <b class="n" id="jv"></b></label><input id="jsc" type="range" min="0.05" max="1" step="0.05"><small>How fast the camera turns with the stick fully over.</small></div>
<div class="f"><label>Smoothness <b class="n" id="rv"></b></label><input id="ramp" type="range" min="0.2" max="3" step="0.1"><small>Higher = gentler starts and stops, slower to react.</small></div></details></div>
<div class="c"><details><summary><span class="lb">Advanced calibration</span></summary>
<div class="row" style="margin-top:12px"><button onclick="q('/set?invp=1')">Flip left / right</button><button onclick="q('/set?invt=1')">Flip up / down</button></div>
<div class="row" style="margin-top:8px"><button onclick="ask('Clear the pan limits?','The head turns slowly left and right until both are set again.','Clear',function(){q('/lim?a=p&e=c')})">Clear pan limits</button><button onclick="ask('Clear the tilt limits?','The head tilts slowly until both are set again.','Clear',function(){q('/lim?a=t&e=c')})">Clear tilt limits</button></div>
<div class="f"><label>Gears: motor turns for one camera turn</label><div class="row" style="margin-top:8px;align-items:center"><input id="gp" type="number" step="0.01" inputmode="decimal" aria-label="Pan gear"><input id="gt" type="number" step="0.01" inputmode="decimal" aria-label="Tilt gear"><button onclick="q('/set?gp='+$('gp').value+'&gt='+$('gt').value)">Save</button></div><small>Pan, then tilt. 0 = not known. The guided setup can measure these.</small></div></details></div>
</section>

<section id="t-system" hidden>
<div class="c"><div class="ch"><span class="lb">Health check</span><small id="hls"></small></div><div id="hl"></div></div>
<div class="c"><div class="ch"><span class="lb">Software</span><small id="ck" hidden><i class="spin"></i> asking GitHub…</small></div><dl id="fw"></dl>
<div class="row" style="margin-top:12px"><button onclick="$('ck').hidden=false;q('/fw')">Check</button><button class="pr" id="upb" onclick="upGo()">Update both</button></div>
<button class="sw" id="au" role="switch" style="margin-top:12px"><span><b>Update automatically</b><small>Installs new builds when the head is idle.</small></span><span class="tg"></span></button></div>
<div class="c"><div class="ch"><span class="lb">Network</span></div><dl id="nw"></dl><button class="lk" onclick="location.href='/wifi'">Wi-Fi networks and password</button></div>
<div class="c"><details id="evd"><summary><span class="lb">Recent events</span></summary><pre id="evt">loading…</pre><div class="row"><button onclick="evLoad()">Refresh</button><button onclick="evSel()">Select all to copy</button></div></details></div>
<div class="c"><details id="dgd"><summary><span class="lb">Diagnostics</span></summary><pre id="dgt">loading…</pre><div class="row"><button onclick="dgLoad()">Refresh</button><button onclick="evSel('dgt')">Select all to copy</button></div></details></div>
<div class="c"><button class="sw" id="mo" role="switch"><span><b>Motors powered</b><small>Off lets you move the camera by hand. It may sag.</small></span><span class="tg"></span></button></div>
</section>
</div>

<div id="bt"><div><button id="stop" class="dg">STOP</button>
<nav><a href="#control" data-t="control"><svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="3"/></svg>Control</a>
<a href="#setup" data-t="setup"><svg viewBox="0 0 24 24"><path d="M4 7h10M18 7h2M4 17h2M10 17h10"/><circle cx="16" cy="7" r="2"/><circle cx="8" cy="17" r="2"/></svg><span>Setup<em id="nd" hidden></em></span></a>
<a href="#system" data-t="system"><svg viewBox="0 0 24 24"><path d="M3 12h4l2-6 4 12 2-6h6"/></svg><span>System<em id="nh" hidden></em></span></a></nav></div></div>

<script>
var N={sent:0,ok:0,fail:0,ms:0,last:0,err:''},pushT=0;
var X=0,Y=0,act=false,busy=false,hold=0,S={},last=0,idle=0,U=null,W=null,NS='http://www.w3.org/2000/svg';
function $(i){return document.getElementById(i)}
function esc(t){return String(t).replace(/[&<>"]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]})}
function fx(v,d){return (v<0?'−':'')+Math.abs(v).toFixed(d)}
// ---- talking to the head
var QN=Promise.resolve();function q(u){QN=QN.then(function(){var c=new AbortController(),t=setTimeout(function(){c.abort()},3000);hold++;return fetch(u,{signal:c.signal}).then(function(r){return r.json()}).then(show).catch(function(){offline()}).then(function(){clearTimeout(t);hold--})});return QN}
function offline(){$('hero').textContent='No connection to the head';$('hero').className='b';$('sub').textContent='Trying again…';if(U)upDraw(null)}
// ---- joystick pads (the main one and the one inside the guided setup share the stick)
function padOn(p){var k=p.firstElementChild;function mv(e){var r=p.getBoundingClientRect(),x=(e.clientX-r.left)/r.width*2-1,y=1-(e.clientY-r.top)/r.height*2,m=Math.hypot(x,y);if(m>1){x/=m;y/=m}X=x;Y=y;k.style.left=(33+X*33)+'%';k.style.top=(33-Y*33)+'%'}
p.onpointerdown=function(e){act=true;p.setPointerCapture(e.pointerId);mv(e)};p.onpointermove=function(e){if(act)mv(e)};
p.onpointerup=p.onpointercancel=function(){act=false;X=Y=0;k.style.left=k.style.top='33%'}}
padOn($('pad'));
// ---- units: degrees when the gears are known, motor turns when not
function deg(a,v){var g=a?S.gt:S.gp;return g>0?v/g*360:null}
function pos(a,v){var d=deg(a,v);return d==null?fx(v,2)+' turns':fx(d,1)+'°'}
function dps(a,rpm){var g=a?S.gt:S.gp;return g>0?rpm*6/g:null}
// ---- sheets (instead of browser pop-ups)
function sheet(o){$('sht').textContent=o.t;$('shp').textContent=o.p||'';var i=$('shi');i.hidden=o.v==null;if(o.v!=null){i.value=o.v;i.placeholder=o.ph||''}
var b=$('shb');b.innerHTML='';o.b.concat([['Cancel','gh']]).forEach(function(a){var e=document.createElement('button');e.textContent=a[0];e.className=a[1]||'';e.onclick=function(){$('shv').hidden=true;if(a[2])a[2](i.value.trim())};b.appendChild(e)});$('shv').hidden=false}
function ask(t,p,l,f){sheet({t:t,p:p,b:[[l,'pr',f]]})}
$('shv').onclick=function(e){if(e.target==this)this.hidden=true};
// ---- stage map
var M={x0:14,x1:306,y0:12,y1:142},mapOK=false;
function sv(n,a,p){var e=document.createElementNS(NS,n);for(var k in a)e.setAttribute(k,a[k]);if(p)p.appendChild(e);return e}
function mapXY(p,t){var l=S.lv;return[M.x0+(p-l[0])/(l[1]-l[0])*(M.x1-M.x0),M.y1-(t-l[2])/(l[3]-l[2])*(M.y1-M.y0)]}
function mapBuild(){var m=$('map');m.innerHTML='';var g=sv('g',{},m);
for(var i=0;i<=8;i++){var x=M.x0+(M.x1-M.x0)*i/8;sv('line',{x1:x,y1:M.y0,x2:x,y2:M.y1,stroke:i==4?'#2f3743':'#1a1f27'},g)}
for(i=0;i<=4;i++){var y=M.y0+(M.y1-M.y0)*i/4;sv('line',{x1:M.x0,y1:y,x2:M.x1,y2:y,stroke:i==2?'#2f3743':'#1a1f27'},g)}
sv('rect',{x:M.x0,y:M.y0,width:M.x1-M.x0,height:M.y1-M.y0,rx:6,fill:'none',stroke:'#3a4452'},g);
['mL','mC','mR'].forEach(function(id,i){var t=sv('text',{id:id,x:[M.x0,160,M.x1][i],y:160,'text-anchor':['start','middle','end'][i],'font-size':10,fill:'#8e98a7','font-family':'ui-monospace,Menlo,monospace'},m)});
sv('g',{id:'mP'},m);var a=sv('g',{id:'mA'},m);sv('circle',{r:13,fill:'#5cc8ff22',stroke:'#5cc8ff','stroke-width':1.5},a);sv('circle',{r:2.5,fill:'#5cc8ff'},a);
[[-19,0,-9,0],[9,0,19,0],[0,-19,0,-9],[0,9,0,19]].forEach(function(l){sv('line',{x1:l[0],y1:l[1],x2:l[2],y2:l[3],stroke:'#5cc8ff','stroke-width':1.5,'stroke-linecap':'round'},a)})}
mapBuild();
function mapDraw(s){mapOK=!!(s.pl&&s.tl&&s.lv[1]>s.lv[0]&&s.lv[3]>s.lv[2]);$('mape').hidden=mapOK;$('map').style.opacity=mapOK?1:.25;$('mh').hidden=!mapOK;$('mA').style.display=mapOK?'':'none';if(!mapOK)return;
var c=mapXY(s.pt,s.tt);$('mA').setAttribute('transform','translate('+c[0].toFixed(1)+','+c[1].toFixed(1)+')');
$('mL').textContent='◂ '+pos(0,s.lv[0]);$('mR').textContent=pos(0,s.lv[1])+' ▸';$('mC').textContent='tilt '+pos(1,s.lv[2])+' to '+pos(1,s.lv[3]);
var g=$('mP');g.innerHTML='';for(var i=0;i<6;i++)if(s.ps[i]){var p=mapXY(s.pp[i][0],s.pp[i][1]);p[0]=Math.max(M.x0,Math.min(M.x1,p[0]));p[1]=Math.max(M.y0,Math.min(M.y1,p[1]));
sv('circle',{cx:p[0],cy:p[1],r:9,fill:'#161a21',stroke:'#ffb347','stroke-width':1.5},g);var t=sv('text',{x:p[0],y:p[1]+3.5,'text-anchor':'middle','font-size':10,'font-weight':700,fill:'#ffb347'},g);t.textContent=i+1}}
var tapT=0,tapX=0,tapY=0;$('map').addEventListener('pointerup',function(e){if(!mapOK)return;var n=Date.now(),r=this.getBoundingClientRect();
if(n-tapT<400&&Math.hypot(e.clientX-tapX,e.clientY-tapY)<30){tapT=0;var x=(e.clientX-r.left)/r.width*320,y=(e.clientY-r.top)/r.height*170,l=S.lv;
var p=l[0]+(Math.max(M.x0,Math.min(M.x1,x))-M.x0)/(M.x1-M.x0)*(l[1]-l[0]),t=l[2]+(M.y1-Math.max(M.y0,Math.min(M.y1,y)))/(M.y1-M.y0)*(l[3]-l[2]);q('/go?p='+p.toFixed(3)+'&t='+t.toFixed(3))}else{tapT=n;tapX=e.clientX;tapY=e.clientY}});
// ---- presets
for(var i=0;i<6;i++){var b=document.createElement('button');b.id='p'+i;b.className='p';b.innerHTML='<u></u><s></s><i>⋯</i>';b.onclick=(function(n){return function(e){if(!S.ps)return;if(!S.ps[n]||e.target.tagName=='I')pEdit(n);else q('/p?n='+n)}})(i);$('pr').appendChild(b)}
function pName(n){return S.pn&&S.pn[n]?S.pn[n]:'Preset '+(n+1)}
function pEdit(n){var set=S.ps[n],sv=function(v){q('/p?n='+n+'&save=1&name='+encodeURIComponent(v))};
if(!set)sheet({t:'Save preset '+(n+1),p:'The camera will come back to exactly where it points now. Give it a name you will recognise.',v:'',ph:'e.g. Pulpit, Choir, Wide',b:[['Save this aim','pr',sv]]});
else sheet({t:pName(n),p:'Change the name here, then choose what to do.',v:S.pn[n]||'',ph:'Name',b:[['Rename','',function(v){q('/p?n='+n+'&name='+encodeURIComponent(v))}],['Replace with where the camera points now','pr',sv],['Delete this preset','',function(){q('/p?n='+n+'&clear=1')}]]})}
// ---- recommended movement feel, worked out from the gears
var FEEL=[['Calm',40,12,1.4],['Balanced',60,20,1.0],['Lively',90,35,0.6]];
function feel(i){var f=FEEL[i],g=S.gp;if(!(g>0))return null;var rpm=Math.max(10,Math.min(300,Math.round(f[1]*g/6/5)*5)),top=rpm*6/g;return{max:rpm,jsc:Math.max(.05,Math.min(1,Math.round(f[2]/top*20)/20)),ramp:f[3]}}
function feelNow(){for(var i=0;i<3;i++){var f=feel(i);if(f&&f.max==S.max&&Math.abs(f.jsc-S.jsc)<.03&&Math.abs(f.ramp-S.ramp)<.06)return i}return -1}
function feelSet(i){var f=feel(i);if(!f){ask('The gears are not known yet','The head needs them to work out speeds in degrees per second. The guided setup measures them in a minute.','Open guided setup',function(){wzOpen(2)});return}
q('/set?max='+f.max+'&jsc='+f.jsc+'&ramp='+f.ramp+'&sd='+(S.sd|4))}
[].forEach.call($('fl3').children,function(b){b.onclick=function(){feelSet(+b.dataset.f)}});
[].forEach.call($('feel').children,function(b){b.onclick=function(){q('/set?jsc='+b.dataset.v)}});
// ---- setup checklist
function steps(s){var sp=s.lm[0]&&s.lm[1]?pos(0,s.lv[0])+' to '+pos(0,s.lv[1]):s.lm[0]||s.lm[1]?'one end set':'not set: pan stays slow',st=s.lm[2]&&s.lm[3]?pos(1,s.lv[2])+' to '+pos(1,s.lv[3]):s.lm[2]||s.lm[3]?'one end set':'not set: tilt stays slow';
return[['Directions','Right is right, up is up',!!(s.sd&1)],['Centre','Where straight ahead is',!!(s.sd&2)],['Gears',s.gp>0&&s.gt>0?s.gp.toFixed(2)+' pan, '+s.gt.toFixed(2)+' tilt motor turns per camera turn':'not known: angles and speeds cannot be shown',s.gp>0&&s.gt>0],
['Left and right limits',sp,!!s.pl],['Up and down limits',st,!!s.tl],['Movement feel',s.sd&4?'chosen':'using the factory values',!!(s.sd&4)]]}
function setupDraw(s){var st=steps(s),n=st.filter(function(x){return x[2]}).length,h='';st.forEach(function(x,i){h+='<button class="ck'+(x[2]?' d':'')+'" onclick="wzOpen('+i+')"><i>'+(x[2]?'✓':i+1)+'</i><span>'+x[0]+'<small>'+esc(x[1])+'</small></span></button>'});
if($('cks').dataset.h!=h){$('cks').innerHTML=h;$('cks').dataset.h=h}$('ring').style.setProperty('--p',n/6*100);$('ringn').textContent=n+'/6';
$('sut').textContent=n==6?'Setup complete':n?'Setup: '+(6-n)+' step'+(n==5?'':'s')+' left':'Let’s set up the head';$('sus').textContent=n==6?'Tap any line to redo it.':'About five minutes. The head stays slow and safe until its limits are set.';
$('sug').textContent=n==6?'Run the guided setup again':n?'Continue':'Start guided setup';$('sug').onclick=function(){var k=0;for(var i=0;i<6;i++)if(!st[i][2]){k=i;break}wzOpen(n==6?0:k)};$('nd').hidden=n==6;return n}
// ---- health check
function health(s){var h=[],g=s.rssi>=-60?'strong':s.rssi>=-70?'good':s.rssi>=-80?'weak':'very weak';
h.push(s.dp&&s.dt?['ok','Motor drivers answering']:['bd',(s.dp?'Tilt':s.dt?'Pan':'Both')+' motor driver not answering','Check the driver board is seated and powered.']);
h.push(s.on?['ok','Motors powered and holding']:['wn','Motors are switched off','Turn them on at the bottom of this page.']);
h.push(s.pl&&s.tl?['ok','Travel limits set']:['wn','Travel limits not set','The head is held to a slow, safe speed. Setup tab.']);
h.push(s.gp>0&&s.gt>0?['ok','Gears known']:['wn','Gears not known','Tracking and presets cannot use real angles. Setup tab.']);
h.push(s.rssi>=-70?['ok','Wi-Fi signal '+g]:['wn','Wi-Fi signal '+g,'Move the access point closer or use the second saved network.']);
h.push(s.jon?['ok','Joystick box connected (build '+s.jfw+')']:['wn','Joystick box not seen','Fine if you are not using it.']);
h.push(s.obs?['ok','OBS connected']:['wn','OBS is not connected','Open OBS and check the Face Tracker connection.']);
h.push(s.blk>=30?['ok','Memory healthy ('+s.heap+' KB free)']:['wn','Memory is low ('+s.heap+' KB free, largest piece '+s.blk+' KB)','Restart the head before a service.']);
h.push(s.lat>s.fw&&s.fw>0?['wn','Update available: build '+s.lat,'Software, below.']:['ok',s.lat<0?'Running build '+s.fw+' (newest not checked yet)':'Software up to date (build '+s.fw+')']);return h}
// ---- everything on the page follows the head's status
function show(s){S=s;var mv=s.src;
var H=s.ust>0?['Updating…','']:!s.on?['Motors are off','b']:mv==1?['You are driving','j']:mv==2?(s.trk&&s.obs?['Following the presenter','t']:['Moving to a preset','']):mv==3?['Tracking is blocked','']:['Holding still',''];
$('hero').textContent=H[0];$('hero').className=H[1];
$('sub').innerHTML='<span class="n">Pan '+pos(0,s.pt)+' · Tilt '+pos(1,s.tt)+'</span>'+(mv==3?' · OBS is sending moves, ignored':s.trk&&s.obs&&mv==0?' · tracking is standing by':'');
$('cO').className='pi '+(s.obs?'ok':'');$('cJ').className='pi '+(s.jon?'ok':'');$('cW').className='pi '+(s.rssi>=-70?'ok':'wn');
var a='';if(!s.dp||!s.dt)a+='<button class="a r" onclick="go(\'system\')"><span>'+(s.dp?'Tilt':s.dt?'Pan':'Both')+' motor driver'+(s.dp||s.dt?' is':'s are')+' not answering.</span><em>Details</em></button>';
if(s.lat>s.fw&&s.fw>0)a+='<button class="a" onclick="go(\'system\')"><span>Build '+s.lat+' is ready to install.</span><em>Update</em></button>';
if($('al').dataset.h!=a){$('al').innerHTML=a;$('al').dataset.h=a}
mapDraw(s);
var top=dps(0,s.max);$('jh').textContent=top?'full stick = '+(top*s.jsc).toFixed(0)+'° per second':'';
[].forEach.call($('feel').children,function(b){b.className=Math.abs(b.dataset.v-s.jsc)<.03?'on':''});
for(var i=0;i<6;i++){var b=$('p'+i),at=s.ps[i]&&Math.abs(s.pp[i][0]-s.pt)<.02&&Math.abs(s.pp[i][1]-s.tt)<.02;b.className='p'+(s.ps[i]?(at?' at':''):' e');b.children[0].textContent=i+1;b.children[1].textContent=s.ps[i]?pName(i):'+ Save here';b.children[2].hidden=!s.ps[i]}
$('trk').setAttribute('aria-checked',!!s.trk);$('trs').textContent=!s.trk?'Blocked: the head ignores OBS. Joystick and presets still work.':s.obs?'Allowed: OBS face tracking can move the head.':'Allowed, but OBS is not connected.';
var n=setupDraw(s),f=feelNow();[].forEach.call($('fl3').children,function(b,i){b.className=i==f?'on':''});$('recn').textContent=s.gp>0?(f<0?'custom':'worked out for your gears'):'needs the gears';
function sl(id,v){if(document.activeElement!=$(id))$(id).value=v}sl('max',s.max);sl('jsc',s.jsc);sl('ramp',s.ramp);
var tp=dps(0,s.max),tt=dps(1,s.max);$('mv').textContent=tp?tp.toFixed(0)+'°/s pan'+(tt?', '+tt.toFixed(0)+'°/s tilt':''):s.max+' RPM at the motor';
$('jv').textContent=tp?(tp*s.jsc).toFixed(0)+'°/s':Math.round(s.jsc*100)+' % of top speed';$('rv').textContent='full speed in '+(s.ramp*2).toFixed(1)+' s';
if(document.activeElement!=$('gp')&&document.activeElement!=$('gt')){$('gp').value=s.gp;$('gt').value=s.gt}
var h=health(s),bad=h.filter(function(x){return x[0]!='ok'}).length,hh='';h.forEach(function(x){hh+='<div class="hl '+x[0]+'"><span>'+esc(x[1])+(x[2]?'<small>'+esc(x[2])+'</small>':'')+'</span></div>'});
if($('hl').dataset.h!=hh){$('hl').innerHTML=hh;$('hl').dataset.h=hh}$('hls').textContent=bad?bad+' to look at':'all good';$('nh').hidden=!(s.lat>s.fw&&s.fw>0)&&s.dp&&s.dt;
var up=s.up||0;$('fw').innerHTML='<dt>Head</dt><dd class="n">build '+s.fw+'</dd><dt>Joystick box</dt><dd class="n">'+(s.jon?'build '+s.jfw:'not seen')+'</dd><dt>Newest on GitHub</dt><dd class="n">'+(s.lat<0?'not checked yet':'build '+s.lat)+'</dd>'+(s.msg?'<dt>Note</dt><dd>'+esc(s.msg)+'</dd>':'');
$('nw').innerHTML='<dt>Head address</dt><dd class="n">'+esc(s.ip)+'</dd><dt>Joystick box</dt><dd class="n">'+(s.jip?esc(s.jip):'not seen')+'</dd><dt>Wi-Fi signal</dt><dd>'+(s.rssi>=-60?'strong':s.rssi>=-70?'good':s.rssi>=-80?'weak':'very weak')+'</dd><dt>Running for</dt><dd class="n">'+Math.floor(up/3600)+' h '+Math.floor(up/60%60)+' min</dd>';
$('ck').hidden=!s.chk;$('au').setAttribute('aria-checked',!!s.auto);$('mo').setAttribute('aria-checked',!!s.on);
if(!U&&s.ust>0)U={from:s.fw,target:0,jon:!!s.jon,t0:Date.now(),step:2,end:''};if(U)upDraw(s);if(W)wzLive();trace()}
// ---- live trace under every stick: what you ask, what the head has, and what it does about it
function trace(){var e=document.querySelectorAll('.tr'),s=S,t='',c='';if(!s.why){return}
var push=Math.abs(X)>.08||Math.abs(Y)>.08;if(push){if(!pushT)pushT=Date.now()}else pushT=0;
var W=['','moving','no motor on this axis','REFUSED: the motors are off','busy driving to a preset','REFUSED: limit','REFUSED: limit','starting'];
function ax(i,v){var w=s.why[i],nm=i?(v>0?'up':'down'):(v>0?'right':'left'),r;
if(w==5||w==6){var hi=(w==5)!=!!s.inv[i];r='REFUSED: the '+(i?(hi?'up':'down'):(hi?'right':'left'))+' limit is here ('+pos(i,s.lv[i*2+(hi?1:0)])+', now '+pos(i,i?s.tt:s.pt)+')';c='b'}
else if(w==1||w==7){var d=dps(i,Math.abs(s.vn[i])*60);r=(w==7?'starting ':'moving ')+(s.vn[i]>0?(i?'up':'right'):(i?'down':'left'))+(d?' '+d.toFixed(1)+'°/s':'');if(!c)c='g'}
else if(w==3){r=W[3];c='b'}else if(w==0)r='head sees no command';else r=W[w];return (i?'Tilt ':'Pan ')+nm+': '+r}
if(push){var got=Math.abs(s.sk[0])>.001||Math.abs(s.sk[1])>.001||Math.abs(X)<.1&&Math.abs(Y)<.1,a=[];
if(!got&&Date.now()-pushT>700){t='The head is not getting this stick: '+N.sent+' sent, '+N.ok+' answered, '+N.fail+' failed'+(N.err?' ('+N.err+')':'')+', last answer '+(N.last?((Date.now()-N.last)/1000).toFixed(1)+' s ago':'never');c='b'}
else{if(Math.abs(X)>.08)a.push(ax(0,X));if(Math.abs(Y)>.08)a.push(ax(1,Y));t=a.join(' · ')}}
else if(Math.abs(s.sk[2])>.001||Math.abs(s.sk[3])>.001)t='The joystick box is driving';
else if(s.why[0]==4||s.why[1]==4)t='Driving to a preset';
[].forEach.call(e,function(x){x.textContent=t;x.className='tr '+c})}
// ---- tabs
function go(t){location.hash=t}
function tab(){var t=(location.hash||'#control').slice(1);if(!$('t-'+t))t='control';['control','setup','system'].forEach(function(k){$('t-'+k).hidden=k!=t});[].forEach.call(document.querySelectorAll('nav a'),function(a){a.className=a.dataset.t==t?'on':''});scrollTo(0,0)}
addEventListener('hashchange',tab);tab();
// ---- guided setup
var WZ=['dir','ctr','gear','pan','tilt','feel','done'],SVGH='<svg viewBox="0 0 200 200">';
function arc(k,a,b,r){var d='';for(var i=0;i<=40;i++){var p=pt(k,a+(b-a)*i/40,r);d+=(i?'L':'M')+p[0].toFixed(1)+' '+p[1].toFixed(1)}return d}
function pt(k,a,r){a=a*Math.PI/180;return k?[100+r*Math.cos(a),100-r*Math.sin(a)]:[100+r*Math.sin(a),100-r*Math.cos(a)]}
// the dial: looking down on the head for pan, from the side for tilt. Without gears the angles are a sketch.
function dialDeg(k,v){var d=deg(k,v);if(d!=null)return d;var l=S.lv,m=Math.max(1.5,Math.abs(k?S.tt:S.pt),Math.abs(l[k*2]),Math.abs(l[k*2+1]));return v*(k?60:120)/m}
function dial(k){var s=SVGH+'<circle cx="100" cy="100" r="82" fill="none" stroke="#2a313c" stroke-width="1"/>';for(var a=0;a<360;a+=15){var p=pt(k,a,82),p2=pt(k,a,a%90?77:72);s+='<line x1="'+p[0]+'" y1="'+p[1]+'" x2="'+p2[0]+'" y2="'+p2[1]+'" stroke="#3a4452"/>'}
s+='<path id="dA" fill="none" stroke="#4cd08a" stroke-width="7" stroke-linecap="round" opacity=".9"/><g id="dL"></g><g id="dN"><path d="'+(k?'M100 100L172 100':'M100 100L100 28')+'" stroke="#5cc8ff" stroke-width="2" stroke-dasharray="3 4"/>'+
(k?'<rect x="78" y="86" width="40" height="28" rx="5" fill="#1e232c" stroke="#eef1f5" stroke-width="1.5"/><rect x="118" y="91" width="16" height="18" rx="2" fill="#ffb347"/>':'<rect x="86" y="82" width="28" height="40" rx="5" fill="#1e232c" stroke="#eef1f5" stroke-width="1.5"/><rect x="91" y="66" width="18" height="16" rx="2" fill="#ffb347"/>')+'</g>'+
'<text x="100" y="'+(k?196:14)+'" text-anchor="middle" font-size="10" fill="#8e98a7">'+(k?'side view':'stage')+'</text></svg>';return s}
function dialDraw(k){if(!$('dN'))return;var v=k?S.tt:S.pt,a=dialDeg(k,v),lm=S.lm,lv=S.lv,lo=dialDeg(k,lv[k*2]),hi=dialDeg(k,lv[k*2+1]),hl=lm[k*2],hh=lm[k*2+1];
$('dN').setAttribute('transform','rotate('+(k?-a:a).toFixed(1)+' 100 100)');var f=hl?lo:Math.min(a,hh?hi:a),t=hh?hi:Math.max(a,hl?lo:a);
$('dA').setAttribute('d',t-f>.5?arc(k,f,t,82):'');$('dA').setAttribute('stroke',hl&&hh?'#4cd08a':'#ffb347');var g='';[[hl,lo],[hh,hi]].forEach(function(e){if(e[0]){var p=pt(k,e[1],82);g+='<circle cx="'+p[0].toFixed(1)+'" cy="'+p[1].toFixed(1)+'" r="6" fill="#4cd08a" stroke="#0d0f13" stroke-width="2"/>'}});$('dL').innerHTML=g;
if($('dR'))$('dR').innerHTML='<span>now <b class="n">'+pos(k,v)+'</b></span>'+(hl&&hh?'<span>travel <b class="n">'+pos(k,lv[k*2+1]-lv[k*2]).replace('−','')+'</b></span>':'')}
function wzOpen(i){W={i:i||0,s:0};$('wz').hidden=false;wzDraw()}
function wzGo(d){AU=0;X=Y=0;W.p0=null;W.i=Math.max(0,Math.min(6,W.i+d));W.s=0;W.m=null;wzDraw()}
$('wzx').onclick=function(){AU=0;X=Y=0;W=null;$('wz').hidden=true};$('wzk').onclick=function(){AU=0;X=Y=0;if(W.s>0){W.s--;wzDraw()}else wzGo(-1)};
function wzSet(h,f){$('wzb').innerHTML=h+'<div class="wzf">'+f+'</div>';var p=$('wzb').querySelector('.pad');if(p)padOn(p);$('wzbar').style.width=(W.i/6*100)+'%';$('wzk').style.visibility=W.i||W.s?'visible':'hidden';$('wz').scrollTop=0;wzLive()}
var PAD='<div class="pad"><div class="kn"></div></div><div class="tr"></div>',NX='<button class="pr" onclick="wzGo(1)">',SK='<button class="gh" onclick="wzGo(1)">Skip for now</button>';
function wzDraw(){var k=WZ[W.i],s=S;
if(k=='dir'){var up=W.s%2==1,box=W.s>1;wzSet('<span class="lb">Step 1 of 6 · Directions · '+(box?'joystick box':'this screen')+'</span><h2>Push the '+(box?'JOYSTICK BOX stick':'stick on this screen')+' '+(up?'UP':'to the RIGHT')+' and watch the camera.</h2><p>Did the camera '+(up?'tilt up':'turn to its right, as seen from behind it')+'?</p>'+(box?'<div class="tip">The screen and the box each remember their own directions. This makes the box agree with the screen.</div><div class="tr"></div>':PAD),
'<button class="pr" onclick="wzDir(0)">Yes, it went '+(up?'up':'right')+'</button><button onclick="wzDir(1)">No, it went '+(up?'down':'left')+': fix it</button>'+(box?'<button class="gh" onclick="q(\'/set?sd=\'+(S.sd|1)).then(function(){wzGo(1)})">Skip the joystick box</button>':''))}
if(k=='ctr')wzSet('<span class="lb">Step 2 of 6 · Centre</span><h2>Aim at the middle of the stage, level.</h2><p>This becomes “straight ahead”. Angles are counted from here, and presets you already saved keep their real places.</p>'+PAD+'<div class="rd" id="dR0"></div>',
'<button class="pr" onclick="q(\'/zero\').then(function(){return q(\'/set?sd=\'+(S.sd|2))}).then(function(){wzGo(1)})">This is centre</button>'+SK);
if(k=='pan'||k=='tilt'){var a=k=='pan'?0:1,n=a?['DOWN','UP','d','u','down','up']:['LEFT','RIGHT','l','r','left','right'],h0=s.lm[a*2],h1=s.lm[a*2+1],e=W.s;
if(e==0&&h0&&h1&&!W.re){wzSet('<span class="lb">Step '+(4+a)+' of 6 · '+(a?'Up and down':'Left and right')+' limits</span><h2>These limits are set.</h2><p>The head slows down by itself as it gets near them and never drives past.</p><div class="fig">'+dial(a)+'</div><div class="rd" id="dR"></div>',
NX+'Keep them</button><button onclick="q(\'/lim?a='+(a?'t':'p')+'&e=c\').then(function(){W.re=1;wzDraw()})">Set them again</button>');return}
if(e>1){W.re=0;wzSet('<span class="lb">Step '+(4+a)+' of 6</span><h2>'+(a?'Tilt':'Pan')+' limits saved.</h2><p>Try it: push the stick all the way each side. The head eases to a stop at each end.</p><div class="fig">'+dial(a)+'</div><div class="rd" id="dR"></div>'+PAD,NX+'Next</button><button class="gh" onclick="wzRe('+a+')">Not right? Clear and set them again</button>');return}
wzSet('<span class="lb">Step '+(4+a)+' of 6 · '+(a?'Up and down':'Left and right')+' limits</span><h2>Drive '+n[e]+' to the furthest safe point.</h2><p>Go slowly. Stop before a cable pulls tight or the camera touches anything'+(a?'.':', then leave a finger of slack.')+'</p><div class="fig">'+dial(a)+'</div><div class="rd" id="dR"></div>'+PAD,
'<button class="pr" onclick="wzLim('+a+','+e+')">Set the '+n[e+4]+' limit here</button>'+(e==0?SK:'<button class="gh" onclick="wzRe('+a+')">Wrong spot? Clear and start this step over</button>'))}
if(k=='gear'){var m=W.m;if(!m){wzSet('<span class="lb">Step 3 of 6 · Gears</span><h2>How far does the camera turn for each motor turn?</h2><p>With this the head knows real angles, so speeds read in degrees per second and tracking is calibrated.</p>'+
'<button class="opt" onclick="W.m={a:0,g:90};wzDraw()"><b>Measure it for me</b><small>The head turns the camera slowly by itself. You tap Stop when it has turned a quarter turn, and the head does the maths. About a minute per axis.</small></button>'+
'<button class="opt" onclick="W.m={k:1};wzDraw()"><b>I know the numbers</b><small>Type the motor turns per camera turn for pan and tilt.</small></button>'+(s.gp>0&&s.gt>0?'<div class="tip">Saved now: pan <b class="n">'+s.gp.toFixed(2)+'</b>, tilt <b class="n">'+s.gt.toFixed(2)+'</b></div>':''),(s.gp>0&&s.gt>0?NX+'Keep these</button>':SK));return}
if(m.k){wzSet('<span class="lb">Step 3 of 6 · Gears</span><h2>Motor turns for one full camera turn</h2><p>Pan, then tilt.</p><div class="row"><input id="wgp" type="number" step="0.01" inputmode="decimal" value="'+(s.gp||'')+'" placeholder="Pan"><input id="wgt" type="number" step="0.01" inputmode="decimal" value="'+(s.gt||'')+'" placeholder="Tilt"></div>',
'<button class="pr" onclick="q(\'/set?gp=\'+($(\'wgp\').value||0)+\'&gt=\'+($(\'wgt\').value||0)).then(function(){wzGo(1)})">Save</button><button class="gh" onclick="W.m=null;wzDraw()">Measure instead</button>');return}
var ax=m.a,nm=ax?'tilt':'pan',ch=ax?[20,30,45]:[45,90,180],cs='';ch.forEach(function(c){cs+='<button class="'+(m.g==c?'on':'')+'" onclick="W.m.g='+c+';wzDraw()">'+c+'°</button>'});
if(m.r!=null){wzSet('<span class="lb">Step 3 of 6 · Gears · '+nm+'</span><h2 class="n">'+m.r.toFixed(2)+'</h2><p>motor turns for one full camera '+(ax?'tilt circle':'turn')+'. '+(m.r<2||m.r>400?'That looks unlikely. Measure again, moving exactly '+m.g+'°.':'Measured from '+Math.abs(m.d).toFixed(2)+' motor turns over '+m.g+'°.')+'</p>',
'<button class="pr" onclick="wzGearSave()">Save and '+(ax?'finish':'measure tilt')+'</button><button onclick="W.m={a:'+ax+',g:'+m.g+'};wzDraw()">Measure again</button>');return}
if(m.p0==null)wzSet('<span class="lb">Step 3 of 6 · Gears · '+nm+'</span><h2>Aim the camera at a starting mark.</h2><p>'+(ax?'Level is easiest: a phone’s level app laid on top of the camera shows 0°.':'Pick something easy to line up with, like the edge of a door, and centre it in the viewfinder.')+'</p><p>Use the stick here to line it up. Then choose how far the head should turn it:</p><div class="ch3">'+cs+'</div>'+PAD,
'<button class="pr" onclick="W.m.p0='+(ax?'S.tt':'S.pt')+';wzDraw()">It is on the mark: next</button><button class="gh" onclick="'+(ax?'wzGo(1)':'W.m={a:1,g:30};wzDraw()')+'">Skip '+nm+'</button>');
else wzSet('<span class="lb">Step 3 of 6 · Gears · '+nm+'</span><h2>Let the head '+(ax?'tilt':'turn')+' the camera exactly '+m.g+'°.</h2><p>Tap Start. The camera '+(ax?'tilts up':'turns right')+' slowly by itself. Tap Stop when it has '+(ax?'tilted':'turned')+' '+m.g+'°: '+(ax?'the phone’s level app on top of the camera reads '+m.g+'°.':m.g==90?'a quarter turn, from facing the stage to squarely facing the side wall.':m.g==180?'half a turn, facing exactly backwards.':'half of a quarter turn.')+'</p>'+
'<button id="gA" class="pr bw" style="padding:22px;font-size:18px;margin-top:10px" onclick="wzAuto()">Start turning</button><div class="row" style="margin-top:8px"><button onclick="wzNudge(-1)">◂ back a little</button><button onclick="wzNudge(1)">a little more ▸</button></div><div class="rd" style="margin-top:14px"><span>motor turns so far <b class="n" id="gN">0.00</b></span></div><div class="tip" id="gT" hidden>The motor is not moving. If the head is at a travel limit, tap Start over, aim the other way and measure from there.</div>',
'<button class="pr" onclick="wzGearDone()">Done, it has '+(ax?'tilted':'turned')+' '+m.g+'°</button><button class="gh" onclick="AU=0;X=Y=0;W.m.p0=null;wzDraw()">Start over</button>')}
if(k=='feel'){var o='',cur=feelNow();if(!(s.gp>0)){wzSet('<span class="lb">Step 6 of 6 · Movement feel</span><h2>The gears are needed for this step.</h2><p>Without them the head cannot work out speeds in degrees per second. You can still set speeds by hand on the Setup tab.</p>','<button class="pr" onclick="W.i=2;W.s=0;W.m=null;wzDraw()">Go to gears</button>'+SK);return}
FEEL.forEach(function(f,i){var v=feel(i);o+='<button class="opt'+(i==cur?' on':'')+'" onclick="feelSet('+i+');setTimeout(wzDraw,400)"><b>'+f[0]+(i==1?' · recommended':'')+'</b><small>'+['Slow, graceful moves for a seated sermon and a tight shot.','A natural pace for most services.','Quick moves for an active stage and a wide shot.'][i]+' Joystick up to '+(v.max*6/s.gp*v.jsc).toFixed(0)+'°/s, top speed '+(v.max*6/s.gp).toFixed(0)+'°/s.</small></button>'});
wzSet('<span class="lb">Step 6 of 6 · Movement feel</span><h2>How should the camera move?</h2><p>Worked out for your gears. Pick one and try it on the stick; change it any time.</p>'+o+PAD,NX+(cur<0?'Keep my own numbers':'Finish')+'</button>')}
if(k=='done'){var st=steps(s),n=st.filter(function(x){return x[2]}).length,l='';st.forEach(function(x){l+='<div class="hl '+(x[2]?'':'wn')+'"><span>'+x[0]+'<small>'+esc(x[1])+'</small></span></div>'});
wzSet('<h2>'+(n==6?'The head is ready.':'Saved. '+(6-n)+' step'+(n==5?'':'s')+' still open.')+'</h2><p>'+(n==6?'It knows its directions, its centre, how far it may travel and how fast to move. Save a few presets next: aim, then tap an empty preset.':'You can finish the open ones any time from the Setup tab.')+'</p><div class="c" style="margin-top:8px">'+l+'</div>','<button class="pr" onclick="$(\'wzx\').onclick();go(\'control\')">Go to the controls</button>')}}
// Saving an end. If the camera got there by pushing the stick the other way, the stick is reversed: say so and fix it
// first, otherwise the end would be saved on the wrong side.
function wzLim(a,e){var n=a?['d','u','down','up']:['l','r','left','right'],now=a?S.tt:S.pt,d=now-(W.p0==null?now:W.p0),wrong=e==0?d>.03:d<-.03,
go=function(){q('/lim?a='+(a?'t':'p')+'&e='+n[e]).then(function(){W.s++;W.p0=null;wzDraw()})};
if(wrong)sheet({t:(a?'Up and down':'Left and right')+' look reversed',p:'You were asked to drive '+n[e+2]+', and the stick was pushed '+n[3-e]+' to get here. If the camera really did go '+n[e+2]+', this stick is back to front. Fix it now? The '+n[e+2]+' limit is then saved here.',b:[['Yes, fix the stick and save','pr',function(){q('/set?inv'+(a?'t':'p')+'=1').then(go)}],['No, the stick is right: save as it is','',go]]});else go()}
function wzRe(a){q('/lim?a='+(a?'t':'p')+'&e=c').then(function(){W.s=0;W.re=1;W.p0=null;wzDraw()})}
function wzDir(flip){var up=W.s%2==1,box=W.s>1;if(flip){q(box?'/boxflip?a='+(up?'u':'l'):'/set?inv'+(up?'t':'p')+'=1').then(function(){ask('Fixed. Try it again.','Push the '+(box?'joystick box ':'')+'stick '+(up?'up':'right')+' once more to check.','OK',function(){})});return}
if(W.s<(S.jon?3:1)){W.s++;wzDraw()}else q('/set?sd='+(S.sd|1)).then(function(){wzGo(1)})}
var AU=0,AUt=0,AUp=0;
// the head turns the camera by itself for the gear measurement: a steady slow turn, and short nudges to land on the mark
function wzAuto(){var m=W.m;if(AU){AU=0;X=Y=0}else{AU=1;AUt=Date.now();AUp=m.a?S.tt:S.pt;if(m.a)Y=.6;else X=.6}var b=$('gA');if(b){b.textContent=AU?'Stop':'Start turning again';b.className=(AU?'dg':'pr')+' bw'}}
function wzNudge(d){if(AU)wzAuto();var m=W.m;if(m.a)Y=d*.35;else X=d*.35;setTimeout(function(){if(!AU)X=Y=0},300)}
function wzGearDone(){AU=0;X=Y=0;var m=W.m,d=(m.a?S.tt:S.pt)-m.p0;if(Math.abs(d)<.15){ask('The motor has hardly moved','Tap Start turning and let the camera turn '+m.g+'° from the starting mark, then tap Done.','OK',function(){});return}m.d=d;m.r=Math.abs(d)*360/m.g;wzDraw()}
function wzGearSave(){var m=W.m;q('/set?'+(m.a?'gt':'gp')+'='+m.r.toFixed(2)).then(function(){if(m.a)wzGo(1);else{W.m={a:1,g:30};wzDraw()}})}
function wzLive(){if(!W||!S.lm)return;var k=WZ[W.i];if((k=='pan'||k=='tilt')&&W.p0==null&&W.s<2)W.p0=k=='tilt'?S.tt:S.pt;if(k=='pan')dialDraw(0);if(k=='tilt')dialDraw(1);if(k=='ctr'&&$('dR0'))$('dR0').innerHTML='<span>pan <b class="n">'+pos(0,S.pt)+'</b></span><span>tilt <b class="n">'+pos(1,S.tt)+'</b></span>';
if(k=='gear'&&$('gN')&&W.m&&W.m.p0!=null){var c=W.m.a?S.tt:S.pt;$('gN').textContent=Math.abs(c-W.m.p0).toFixed(2);if(AU&&Date.now()-AUt>2500){if(Math.abs(c-AUp)<.01){wzAuto();$('gT').hidden=false}AUt=Date.now();AUp=c}}}
// ---- live update progress
try{U=JSON.parse(sessionStorage.x70up||'null')}catch(e){}if(U&&Date.now()-U.t0>600000)U=null;
function upGo(){var f=function(){U={from:S.fw||0,target:S.lat||0,jon:!!S.jon,t0:Date.now(),step:1,end:''};upDraw(S);q('/fwgo')};
if(S.lat>0&&S.fw>=S.lat&&(!S.jon||S.jfw>=S.lat))ask('Already on the newest build','Both the head and the joystick box are up to date. Install it again anyway?','Install again',f);else f()}
function upDraw(s){var n=Date.now();
if(s&&!U.end){if(s.ust==1)U.step=Math.max(U.step,2);else if(s.ust==2){U.step=3;U.pct=s.upc}else if(s.ust==3)U.step=4;else if(s.ust==4){U.end='bad';U.msg='The download failed 5 times. The head is going back to the build it had.'}
else if(s.fw>U.from||(U.step>=2&&U.target&&s.fw>=U.target)){U.step=5;U.end='ok';U.fw=s.fw}
else if(U.step>=2||n-U.t0>30000){U.end='bad';U.msg=s.msg||'The head came back on the same build.'}}
if(!s&&!U.end&&U.step==3&&U.pct>=95)U.step=4;if(s&&U.end=='bad'&&s.ust==0&&s.msg)U.msg=s.msg;
for(var i=1;i<=5;i++)$('u'+i).className='us '+(U.end=='ok'||i<U.step?'ok':i==U.step?(U.end=='bad'?'bad':'now'):'');
$('upc').textContent=U.step==3?(U.pct||0)+' %':'';if(s&&s.ust==1&&s.utry>1)$('um').dataset.t='Try '+s.utry+' of 5. ';else if(s)$('um').dataset.t='';$('ub').style.width=(U.step>3||U.end=='ok'?100:U.step==3?(U.pct||0):0)+'%';
if(U.end=='ok')$('u5t').textContent='Head running build '+U.fw;
var j='';if(s){if(!U.jon&&!s.jon){j='';$('uj').textContent='Joystick box: not switched on'}else if(s.jon&&(U.target||U.fw)&&s.jfw>=(U.target||U.fw)){j='ok';$('uj').textContent='Joystick box running build '+s.jfw}else if(U.end!='ok'){j='';$('uj').textContent='Joystick box: next, after the head'}else{j=n-U.t0>360000?'bad':'now';$('uj').textContent=s.jon?'Joystick box: on build '+s.jfw+', updating now':'Joystick box: restarting'}$('u6').className='us '+j}
$('ut').textContent=U.end=='ok'?'Update finished':U.end=='bad'?'Update did not finish':'Updating the head';
$('um').textContent=U.end=='ok'?'':U.end=='bad'?U.msg:!s?'The head is restarting. This page reconnects by itself.':n-U.t0>180000?'This is taking longer than normal. Leave everything powered.':($('um').dataset.t||'')+'About a minute for the head, then the joystick box. Nothing else is needed.';
$('ux').hidden=!U.end;if(U.end=='ok'&&$('u6').className=='us now'){$('ut').textContent='Head done, joystick box next';$('um').textContent='The joystick box updates by itself now. You can close this.'}$('up').hidden=false;try{sessionStorage.x70up=JSON.stringify(U)}catch(e){}}
$('ux').onclick=function(){U=null;$('up').hidden=true;try{sessionStorage.removeItem('x70up')}catch(e){}};
// ---- the rest of the controls
function evLoad(){fetch('/log').then(function(r){return r.text()}).then(function(t){$('evt').textContent=t}).catch(function(){$('evt').textContent='could not load'})}
function dgLoad(){fetch('/diag').then(function(r){return r.text()}).then(function(t){$('dgt').textContent=t+'page: stick requests '+N.sent+' sent, '+N.ok+' answered, '+N.fail+' failed'+(N.err?' ('+N.err+')':'')+', last reply took '+N.ms+' ms\n'+navigator.userAgent}).catch(function(){$('dgt').textContent='could not load'})}
function evSel(i){var r=document.createRange();r.selectNodeContents($(i||'evt'));var s=getSelection();s.removeAllRanges();s.addRange(r)}
$('evd').ontoggle=function(){if(this.open)evLoad()};$('dgd').ontoggle=function(){if(this.open)dgLoad()};
$('stop').onclick=function(){if(AU)wzAuto();X=Y=0;q('/stop')};$('trk').onclick=function(){q('/trk')};$('au').onclick=function(){q('/set?auto=1')};
$('mo').onclick=function(){if(S.on)ask('Switch the motors off?','The camera is no longer held and may sag forward. Hold it if it is front-heavy.','Switch off',function(){q('/en?on=0')});else q('/en?on=1')};
$('max').onchange=function(){q('/set?max='+this.value)};$('jsc').onchange=function(){q('/set?jsc='+this.value)};$('ramp').onchange=function(){q('/set?ramp='+this.value)};
setInterval(function(){if(busy||hold)return;var mvg=act||X||Y;if(mvg||last){busy=true;last=mvg?1:0;var c=new AbortController(),t=setTimeout(function(){c.abort()},2500);var t0=Date.now();N.sent++;fetch('/j?x='+X.toFixed(3)+'&y='+Y.toFixed(3),{signal:c.signal}).then(function(r){return r.json()}).then(function(s){N.ok++;N.ms=Date.now()-t0;N.last=Date.now();show(s)}).catch(function(e){N.fail++;N.err=String(e&&e.name||e)}).then(function(){clearTimeout(t);busy=false;trace()})}
else if(++idle>=(W?3:10)){idle=0;q('/s')}},100);
</script></body></html>)HTML";

const char TUNE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>X70: adjust face tracking</title>
<style>
body{margin:0;background:#0d0f13;color:#e8ecf1;font:15px -apple-system,Segoe UI,Roboto,sans-serif}
.w{max-width:440px;margin:auto;padding:12px}a{color:#ffb347}
button{background:#1e232c;color:#e8ecf1;border:1px solid #2a313c;border-radius:10px;padding:12px 8px;font-size:15px;margin:3px 0;width:100%}
button:active{background:#2a313c}.on{background:#1c6b43}.sv{background:#8a6d1a}
h3{margin:18px 0 4px;font-size:13px;color:#8e98a7;text-transform:uppercase;letter-spacing:.08em}
label{display:block;margin:12px 0 2px;font-size:13px;color:#cfd6df}label b{float:right;color:#e8ecf1}
input[type=range]{width:100%}small{color:#8e98a7;display:block}
input[type=text],input[type=password],input[type=number],select{width:100%;box-sizing:border-box;background:#161a21;color:#e8ecf1;border:1px solid #2a313c;border-radius:8px;padding:9px;font-size:16px;margin:2px 0 6px}
.warn{color:#f2a93b;font-size:13px}.ok{color:#6fcf97}.hi{color:#f2a93b}.g3{display:grid;grid-template-columns:1fr;gap:0}details{margin-top:14px;background:#161a21;border-radius:10px;padding:8px 12px}summary{padding:6px 0}label i{font-style:normal;font-size:12px;margin-left:6px}#os{font-size:13px;color:#8e98a7;margin:6px 0}
</style></head><body><div class="w">
<a href="/">&larr; Head controls</a>
<h3>Head (changes apply at once)</h3>
<small id="adr"></small><div id="hw" class="warn"></div>
<button id="trk"></button>
<label>Gentleness of tracking moves <b id="v_tsc"></b></label><input id="tsc" type="range" min="0.05" max="1" step="0.05">
<small>Left = slower, finer moves. Right = faster, coarser. Good: 0.15 to 0.30. The joystick is not affected.</small>
<label>Soft start and stop <b id="v_tramp"></b></label><input id="tramp" type="range" min="0.2" max="2" step="0.1">
<small>How long a tracking move takes to speed up and slow down. Good: 0.4 to 0.8 s. Longer looks smoother but reacts later.</small>
<h3>OBS Face Tracker PTZ (live)</h3>
<div id="cn">
<small>In OBS: Tools &rarr; WebSocket Server Settings &rarr; enable, and copy the password. Stored only in this browser.</small>
PC address<input id="ip" type="text" placeholder="192.168.1.50" autocapitalize="off" autocomplete="off">
Port<input id="port" type="number" value="4455">
Password<input id="pw" type="password" autocomplete="off">
<button id="go">Connect to OBS</button></div>
<div id="os">Not connected to OBS.</div>
<div id="ob" hidden>
<button id="en"></button>
<h3>1. Start here</h3>
<select id="shot" hidden><option value="11" selected>Full-body shot</option></select>
<button id="rec">Load the recommended settings</button>
<h3>Who to follow</h3>
<button id="lk" class="on">LOCK on the person at the yellow cross</button>
<small>Aim the camera with the joystick so the person is on the yellow cross, then tap. The camera stays with that person and never picks anybody else by itself.</small>
<button id="au2">Follow automatically again (the biggest person)</button>
<div id="fm" class="warn"></div>
<h3>2. How the camera behaves</h3><div id="ch"></div><div id="sl" hidden></div>
<details><summary>Exact numbers (used when a choice above is Custom)</summary><div id="sl2"></div>
<button id="ix"></button><button id="iy"></button></details>
</div></div>
<script>
var $=function(i){return document.getElementById(i)},H={},ws=null,rid=0,cb={},SRC='',FLT='',FS={},FPS=30,tm={};
// key, label, min, max, step, unit, good-from, good-to, advice when below, advice when above, basic(1)/more(0)
var P=[['head_top_pct','Head: distance from the top',5,60,1,' % of height',18,30,'head near the top edge','a lot of space above the head',0],
['zone_w_pct','Rest zone: width',4,80,1,' % of width',15,30,'follows every small move','person can sit far off-centre',0],
['zone_h_pct','Rest zone: height',4,80,1,' % of height',15,30,'bobs when the person nods','slow to follow up and down',0],
['zone_strength','Bringing the person back (0.6 gentle, 1 normal, 1.6 firm)',0.6,1.6,0.1,'',0.8,1.6,'gentle, a fast walker may get away','',0],
['zone_anticipation','When the person walks (0 wait, 1 travel with them)',0,1,0.1,'',0.6,1,'follows after the person has moved','',0],
['pursuit_s','Keep turning after the person walks out of the picture',0,5,0.5,' s',1,3,'stops at once when the person is lost','may turn away from where they stopped',0],
['search_b_pct','Search area: ignore the bottom (congregation)',0,80,1,' % of height',15,40,'people in the seats may be picked','a person low in the picture is missed',0],
['lock_hold','Automatic: wait for a lost person before picking another',1,60,1,' s',5,15,'may jump to someone else quickly','may follow nobody for a while',0],
['head_left_pct','Head: distance from the left',20,80,1,' % of width',45,55,'person sits left','person sits right',0],
['search_t_pct','Search area: ignore the top',0,80,1,' % of height',0,10,'','tall people are missed',0],
['search_l_pct','Search area: ignore the left side',0,80,1,' % of width',0,15,'','person is lost on the left',0],
['search_r_pct','Search area: ignore the right side',0,80,1,' % of width',0,15,'','person is lost on the right',0]];
var C=[['headroom','Head room (space above the head)',[['tight','Tight (closer shots)'],['normal','Normal'],['generous','Generous (wide shots)'],['custom','Custom (number below)']]],
['lead','Space in front of a walking person',[['off','None: always centred'],['some','A little'],['more','More'],['custom','Custom (number below)']]],
['sensitivity','Sensitivity',[['low','Low: calm camera, ignores small movements'],['medium','Medium'],['high','High: follows small movements too'],['custom','Custom (numbers below)']]],
['zone_strength','Speed',[[0.6,'Gentle: slow, smooth catching up'],[1,'Normal'],[1.6,'Firm: for people who move fast']]],
['zone_anticipation','When the person walks',[[1,'Travel with them (like a camera operator)'],[0.6,'Travel with them, a little behind'],[0,'Only move once they are off-centre']]],
['scope','The camera may',[['pt','Turn and tilt'],['pan','Turn only (tilt stays where the joystick left it)']]],
['pursuit_s','If the person walks out of the picture',[[0,'Stop and wait'],[2,'Keep turning that way for 2 seconds'],[4,'Keep turning that way for 4 seconds']]],
['seats','Seats in the bottom of the picture',[['none','None in view'],['low','A few heads'],['medium','Front rows'],['high','Many rows'],['custom','Custom (number below)']]],
['lock_hold','Automatic: if the person is lost',[[5,'Wait 5 seconds, then follow somebody else'],[10,'Wait 10 seconds'],[30,'Wait 30 seconds']]]];
var KP=56;
function q(u){return fetch(u).then(function(r){return r.json()}).then(head).catch(function(){})}
function head(s){H=s;var w='';if(!(s.gp>0)||!(s.gt>0))w='Gear ratios are not set on the head page, so tracking speeds are not calibrated. ';
if(!s.trk)w+='Tracking is blocked (CW/CCW button is out). ';$('hw').textContent=w;$('adr').textContent='Head '+(s.ip||'?')+'   Joystick '+(s.jip||'not seen');
if(document.activeElement!=$('tsc'))$('tsc').value=s.tsc;$('v_tsc').textContent=(+s.tsc).toFixed(2)+' x';
if(document.activeElement!=$('tramp'))$('tramp').value=s.tramp;$('v_tramp').textContent=(+s.tramp).toFixed(1)+' s';
$('trk').textContent=s.trk?'Face tracking allowed (tap to block)':'Face tracking BLOCKED (tap to allow)';$('trk').className=s.trk?'on':'sv'}
$('tsc').oninput=function(){$('v_tsc').textContent=(+this.value).toFixed(2)+' x'};$('tsc').onchange=function(){q('/set?tsc='+this.value).then(function(){if(SRC)paint()})};
$('tramp').oninput=function(){$('v_tramp').textContent=(+this.value).toFixed(1)+' s'};$('tramp').onchange=function(){q('/set?tramp='+this.value)};
$('trk').onclick=function(){q('/trk')};
setInterval(function(){q('/s')},1500);q('/s');
// ---- SHA-256 (the page is plain http, so the browser's own crypto is not available) ----
function sha(str){var m=new TextEncoder().encode(str),l=m.length,n=((l+8)>>6)+1,w=new Uint32Array(n*16),i,j;
for(i=0;i<l;i++)w[i>>2]|=m[i]<<(24-(i%4)*8);w[l>>2]|=0x80<<(24-(l%4)*8);w[n*16-1]=l*8;w[n*16-2]=Math.floor(l/536870912);
var K=[],h=[],c=2,x=0;function fr(v){return((v-Math.floor(v))*4294967296)|0}
while(x<64){var pr=true;for(j=2;j*j<=c;j++)if(c%j==0){pr=false;break}if(pr){if(x<8)h[x]=fr(Math.sqrt(c));K[x++]=fr(Math.cbrt(c))}c++}
var W=new Int32Array(64);function r(v,s){return(v>>>s)|(v<<(32-s))}
for(i=0;i<n;i++){for(j=0;j<16;j++)W[j]=w[i*16+j];
for(j=16;j<64;j++){var a=W[j-15],b=W[j-2];W[j]=(W[j-16]+(r(a,7)^r(a,18)^(a>>>3))+W[j-7]+(r(b,17)^r(b,19)^(b>>>10)))|0}
var A=h[0],B=h[1],C=h[2],D=h[3],E=h[4],F=h[5],G=h[6],I=h[7];
for(j=0;j<64;j++){var t1=(I+(r(E,6)^r(E,11)^r(E,25))+((E&F)^(~E&G))+K[j]+W[j])|0,t2=((r(A,2)^r(A,13)^r(A,22))+((A&B)^(A&C)^(B&C)))|0;
I=G;G=F;F=E;E=(D+t1)|0;D=C;C=B;B=A;A=(t1+t2)|0}
h[0]=(h[0]+A)|0;h[1]=(h[1]+B)|0;h[2]=(h[2]+C)|0;h[3]=(h[3]+D)|0;h[4]=(h[4]+E)|0;h[5]=(h[5]+F)|0;h[6]=(h[6]+G)|0;h[7]=(h[7]+I)|0}
var o='';for(i=0;i<8;i++)for(j=3;j>=0;j--)o+=String.fromCharCode((h[i]>>>(j*8))&255);return btoa(o)}
// ---- OBS WebSocket (protocol 5) ----
function os(t){$('os').textContent=t}
function req(t,d){return new Promise(function(ok,no){if(!ws||ws.readyState!=1){no('not connected');return}var id='r'+(++rid);cb[id]=[ok,no];
ws.send(JSON.stringify({op:6,d:{requestType:t,requestId:id,requestData:d||{}}}))})}
function connect(){var ip=$('ip').value.trim(),port=$('port').value||4455,pw=$('pw').value;if(!ip){os('Enter the PC address.');return}
try{localStorage.setItem('x70obs',JSON.stringify({ip:ip,port:port,pw:pw}))}catch(e){}
if(ws){ws.onclose=null;ws.close()}os('Connecting...');
try{ws=new WebSocket('ws://'+ip+':'+port)}catch(e){os('Cannot open the connection: '+e.message);return}
ws.onmessage=function(m){var j=JSON.parse(m.data);
if(j.op==0){var a=j.d.authentication,id={rpcVersion:1,eventSubscriptions:0};if(a)id.authentication=sha(sha(pw+a.salt)+a.challenge);ws.send(JSON.stringify({op:1,d:id}))}
else if(j.op==2){find()}
else if(j.op==7){var c=cb[j.d.requestId];delete cb[j.d.requestId];if(c){if(j.d.requestStatus.result)c[0](j.d.responseData||{});else c[1](j.d.requestStatus.comment||('error '+j.d.requestStatus.code))}}};
ws.onclose=function(e){$('ob').hidden=true;os(e.code==4009?'OBS rejected the password.':'Not connected to OBS'+(e.code&&e.code!=1000?' (code '+e.code+').':'.'))};
ws.onerror=function(){os('Could not reach OBS at '+ip+':'+port+'. Is the WebSocket server enabled?')}}
function find(){os('Connected. Looking for the Face Tracker PTZ filter...');
req('GetVideoSettings').then(function(v){if(v.fpsDenominator)FPS=v.fpsNumerator/v.fpsDenominator;return req('GetInputList')}).then(function(l){
var ins=l.inputs||[],i=0;function next(){if(i>=ins.length){os('Connected, but no source has a Face Tracker PTZ filter yet.');return}
var nm=ins[i++].inputName;req('GetSourceFilterList',{sourceName:nm}).then(function(f){var fl=(f.filters||[]).filter(function(x){return x.filterKind=='face_tracker_ptz'})[0];
if(!fl){next();return}SRC=nm;FLT=fl.filterName;
req('GetSourceFilterDefaultSettings',{filterKind:'face_tracker_ptz'}).then(function(d){FS=Object.assign({},d.defaultFilterSettings||{},fl.filterSettings||{});FS._en=fl.filterEnabled;build()},function(){FS=Object.assign({},fl.filterSettings||{});FS._en=fl.filterEnabled;build()})},next)}
next()}).catch(function(e){os('OBS error: '+e)})}
function fmt(p,v){return(+v).toFixed(p[4]<0.1?2:(p[4]<1?1:0))+p[5]}
function good(p){if(p[0]=='Kp_x_db')return[KP-5,KP+3];if(p[0]=='Kp_y_db')return[KP-8,KP];return[p[6],p[7]]}
function note(p,v){var g=good(p),t=v<g[0]?p[8]:v>g[1]?p[9]:'';return t?'<i class="hi">'+t+'</i>':'<i class="ok">good</i>'}
function base(){var fov=+$('shot').value,s=+(H.tsc||0.2);return Math.max(25,Math.min(77,Math.round(20*Math.log10(fov*0.75*FPS/s))))}
function fmode(){$('fm').textContent=FS.follow_mode=='manual'?'Manual: staying on the person you locked.':'Automatic: following the biggest person in the search area.'}
$('lk').onclick=function(){setv('follow_mode','manual');setv('lock_request',Date.now()%1000000000);fmode();os('Lock sent. The green box should now be on that person.')};
$('au2').onclick=function(){setv('follow_mode','auto');fmode()};
function setv(k,v){FS[k]=v;clearTimeout(tm[k]);tm[k]=setTimeout(function(){var o={};o[k]=v;req('SetSourceFilterSettings',{sourceName:SRC,filterName:FLT,filterSettings:o,overlay:true}).catch(function(e){os('OBS error: '+e)})},120)}
function build(){os('Connected: filter "'+FLT+'" on "'+SRC+'", OBS at '+FPS.toFixed(0)+' fps.');$('ob').hidden=false;var h=['',''];
P.forEach(function(p){var g=good(p);h[p[10]?0:1]+='<label>'+p[1]+' <b id="v_'+p[0]+'"></b></label><input type="range" id="s_'+p[0]+'" min="'+p[2]+'" max="'+p[3]+'" step="'+p[4]+'"><small id="n_'+p[0]+'"></small>'});$('sl').innerHTML=h[0];$('sl2').innerHTML=h[1];
P.forEach(function(p){var e=$('s_'+p[0]);e.oninput=function(){var v=+e.value;show1(p,v);setv(p[0],v)}});paint()}
function show1(p,v){var g=good(p);$('v_'+p[0]).innerHTML=fmt(p,v)+note(p,v);$('n_'+p[0]).textContent='Good: '+fmt(p,g[0])+' to '+fmt(p,g[1])}
function nudge(k,d){var p=P.filter(function(x){return x[0]==k})[0],v=Math.max(p[2],Math.min(p[3],(+FS[k]||0)+d));setv(k,v);$('s_'+k).value=v;show1(p,v)}
function buildC(){var h='';C.forEach(function(c){h+='<label>'+c[1]+'</label><select id="c_'+c[0]+'">'+c[2].map(function(o){return '<option value="'+o[0]+'">'+o[1]+'</option>'}).join('')+'</select>'});$('ch').innerHTML=h;
C.forEach(function(c){$('c_'+c[0]).onchange=function(){var v=this.value;setv(c[0],typeof c[2][0][0]=='number'?+v:v)}})}
function paintC(){C.forEach(function(c){var e=$('c_'+c[0]);if(e&&FS[c[0]]!==undefined)e.value=FS[c[0]]})}
$('shot').onchange=function(){KP=base();if(SRC)paint()};
function paint(){fmode();if(!$('c_headroom'))buildC();paintC();KP=base();P.forEach(function(p){var v=FS[p[0]];if(v===undefined)v=0;$('s_'+p[0]).value=v;show1(p,+v)});
$('en').textContent=FS._en?'Plugin tracking is ON (tap to switch off)':'Plugin tracking is OFF (tap to switch on)';$('en').className=FS._en?'on':'sv';
$('ix').textContent='Reverse pan direction: '+(FS.invert_x?'ON':'off');$('iy').textContent='Reverse tilt direction: '+(FS.invert_y?'ON':'off')}
$('en').onclick=function(){var v=!FS._en;req('SetSourceFilterEnabled',{sourceName:SRC,filterName:FLT,filterEnabled:v}).then(function(){FS._en=v;paint()},function(e){os('OBS error: '+e)})};
$('ix').onclick=function(){setv('invert_x',!FS.invert_x);paint()};$('iy').onclick=function(){setv('invert_y',!FS.invert_y);paint()};
// Start values: loop gain about 1 per second for the chosen shot width, the OBS frame rate and the head's speed scale.
function rec(){
var v={zone_mode:true,headroom:'normal',lead:'some',sensitivity:'medium',zone_strength:1,zone_anticipation:1,scope:'pt',pursuit_s:2,lock_hold:10,person_model:'tiny',detector_engine:2};
Object.assign(FS,v);req('SetSourceFilterSettings',{sourceName:SRC,filterName:FLT,filterSettings:v,overlay:true}).then(function(){paint();os('Recommended settings loaded. The search area was left as it is.')},function(e){os('OBS error: '+e)})}
$('rec').onclick=rec;$('go').onclick=connect;
try{var sv=JSON.parse(localStorage.getItem('x70obs')||'null');if(sv){$('ip').value=sv.ip;$('port').value=sv.port;$('pw').value=sv.pw}}catch(e){}
</script></body></html>)HTML";
