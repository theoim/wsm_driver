/**
 * @file    web_page.h
 * @brief   Embedded single-page UI for the camera streaming example.
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * The whole UI is one self-contained string: no external CSS, JS, fonts or
 * images, because the device is the only server the browser can reach.
 *
 * Adapted from the WIZnet ArduCAM MEGA streaming page, which was built to put
 * two boards side by side and compare their stacks. Here there is one board and
 * two interfaces, so the same comparison happens between two browser tabs: the
 * badge and the accent colour come from the "link" field of /api/status, and
 * open http://<eth-ip> beside http://<wifi-ip>:81 to watch hardware TCP/IP and
 * software TCP/IP carry the same sensor at once.
 *
 * Changes from the ArduCAM original, all of them because the sensor is
 * different rather than because the page was:
 *
 *   - CLK_DIV / PLL_DIV became JPEG quality and XCLK. The OV3660 driven by
 *     esp32-camera has no divider pair; quality is the lever that actually
 *     moves link bandwidth, and XCLK is the closest thing to the old sweep.
 *   - The timing legend lost a bar. esp_camera_fb_get() returns a finished
 *     frame, so the VSYNC wait and the sensor readout arrive as one number and
 *     splitting them would mean inventing the split.
 *   - The header wordmark is text, not /logo.png, so the example carries no
 *     binary asset.
 *
 * Brand colours live in the :root block below. Change those variables to
 * restyle the page; nothing else hard-codes a colour except the chart, which
 * reads them back out of the computed style.
 *
 * HTML attributes use single quotes so the C string needs no escaping.
 */

#ifndef WEB_PAGE_H
#define WEB_PAGE_H

static const char HTTP_INDEX_PAGE[] =
"<!DOCTYPE html>\n"
"<html lang='en'>\n"
"<head>\n"
"<meta charset='utf-8'>\n"
"<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
"<title>WIZnet Camera Stream</title>\n"
"<style>\n"
":root{\n"
"  --bg:#ffffff; --panel:#ffffff; --line:#dfe5ec;\n"
"  --text:#101418; --muted:#69737f;\n"
"  --accent:#e01b24; --ok:#3ddc84; --stop:#ff5c5c; --warn:#f5a623;\n"
"  --stack:#e01b24;\n"
"}\n"
"*{box-sizing:border-box}\n"
"body{margin:0;background:var(--bg);color:var(--text);\n"
"  font-family:'Segoe UI',Roboto,Helvetica,Arial,sans-serif}\n"
"header{display:flex;align-items:center;gap:18px;padding:12px 24px;\n"
"  background:var(--panel);border-bottom:3px solid var(--stack)}\n"
"header .mark{font-size:26px;font-weight:800;letter-spacing:1px;\n"
"  color:var(--accent)}\n"
"header .sub{color:var(--muted);font-size:13px}\n"
"header .stackbox{margin-left:auto;display:flex;align-items:center;gap:16px}\n"
"header .tag{padding:6px 30px;border-radius:8px;background:var(--stack);\n"
"  color:#ffffff;font-weight:800;font-size:38px;line-height:1.1;\n"
"  letter-spacing:3px;min-width:200px;text-align:center}\n"
"header .what{color:var(--muted);font-size:13px;max-width:210px;\n"
"  line-height:1.4;text-align:right}\n"
"main{max-width:1500px;margin:0 auto;padding:16px;display:grid;\n"
"  grid-template-columns:minmax(0,1fr) 250px;gap:16px}\n"
"@media(max-width:960px){main{grid-template-columns:1fr}}\n"
".card{background:var(--panel);border:1px solid var(--line);\n"
"  border-radius:10px;padding:14px;box-shadow:0 1px 3px rgba(16,20,24,.06)}\n"
".wide{grid-column:1/-1}\n"
".card h2{margin:0 0 10px;font-size:12px;text-transform:uppercase;\n"
"  letter-spacing:1.5px;color:var(--muted);font-weight:600}\n"
".card.tight{padding:12px}\n"
".view{background:#000;border-radius:8px;overflow:hidden;min-height:240px}\n"
".view img{display:block;width:100%;max-width:100%;height:auto}\n"
".btns{display:flex;gap:8px;margin-bottom:12px}\n"
"button{padding:9px 12px;border-radius:7px;cursor:pointer;font-size:13px;\n"
"  font-weight:600;letter-spacing:.5px;transition:background .15s,color .15s}\n"
"button:disabled{opacity:.3;cursor:not-allowed}\n"
".btns button{flex:1;border:0;color:#08121c}\n"
"#start{background:var(--ok)} #stop{background:var(--stop)}\n"
".btns button:not(:disabled):hover{filter:brightness(1.1)}\n"
"button.ghost{background:transparent;border:1px solid var(--line);\n"
"  color:var(--muted);font-size:11px;padding:7px 12px;text-transform:uppercase}\n"
"button.ghost:hover{border-color:var(--accent);color:var(--accent)}\n"
"select{width:100%;padding:8px;border-radius:7px;border:1px solid var(--line);\n"
"  background:var(--bg);color:var(--text);font-size:13px}\n"
"label{display:block;font-size:11px;color:var(--muted);margin:0 0 5px}\n"
"dl{display:grid;grid-template-columns:auto 1fr;gap:7px 10px;margin:12px 0 0;font-size:13px}\n"
"dt{color:var(--muted)}\n"
"dd{margin:0;text-align:right;font-variant-numeric:tabular-nums}\n"
".dot{display:inline-block;width:8px;height:8px;border-radius:50%;\n"
"  margin-right:7px;background:var(--muted);vertical-align:middle}\n"
".dot.live{background:var(--ok);box-shadow:0 0 8px var(--ok)}\n"
/* Unreachable is not the same state as stopped, and the page used to draw them
   identically -- see the offline() handler. Amber, and blinking, so a frozen
   view reads as a dead link rather than as a live one that happens to be still. */
".dot.gone{background:var(--warn);animation:blink 1s steps(2,end) infinite}\n"
"@keyframes blink{50%{opacity:.15}}\n"
"footer{text-align:center;color:var(--muted);font-size:12px;padding:8px 0 24px}\n"
".charts{display:grid;grid-template-columns:1fr 1fr;gap:24px}\n"
"@media(max-width:900px){.charts{grid-template-columns:1fr}}\n"
".chead{display:flex;align-items:flex-end;gap:16px;margin-bottom:6px;flex-wrap:wrap}\n"
".chead .big{font-size:40px;font-weight:700;line-height:1;\n"
"  font-variant-numeric:tabular-nums}\n"
".chead .big small{font-size:14px;color:var(--muted);margin-left:8px;font-weight:400}\n"
".chead .stats{display:flex;gap:16px;margin-left:auto;font-size:12px;\n"
"  color:var(--muted)}\n"
".chead .stats b{color:var(--text);font-variant-numeric:tabular-nums}\n"
"canvas{display:block;width:100%;height:180px}\n"
".legend{display:flex;flex-wrap:wrap;gap:20px;margin-top:10px;\n"
"  font-size:13px;color:var(--muted)}\n"
".legend i{display:inline-block;width:10px;height:10px;border-radius:2px;\n"
"  margin-right:7px;vertical-align:middle}\n"
".legend b{color:var(--text);font-variant-numeric:tabular-nums}\n"
"i.cap{background:var(--warn)} i.send{background:#8e7bef}\n"
".hint{margin:12px 0 0;font-size:13px;color:var(--muted);line-height:1.5}\n"
".hint b{color:var(--accent)}\n"
".sweep{display:flex;flex-wrap:wrap;gap:14px;align-items:center}\n"
".sweep>div{flex:1;min-width:180px;display:flex;align-items:center;gap:10px}\n"
".sweep label{margin:0;white-space:nowrap;font-size:11px}\n"
".sweep label b{color:var(--accent);font-variant-numeric:tabular-nums}\n"
"input[type=range]{flex:1;min-width:90px;accent-color:var(--accent)}\n"

/* --- Sensor controls: four groups side by side, each a stack of rows.
       Built from /api/controls, so the layout has to survive whatever the
       device reports rather than assuming a fixed set. --- */
".ctrls{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));\n"
"  gap:10px 24px}\n"
".grp h3{margin:0 0 8px;font-size:11px;text-transform:uppercase;\n"
"  letter-spacing:1.2px;color:var(--accent);font-weight:700}\n"
".row{display:flex;align-items:center;gap:10px;margin-bottom:7px;font-size:12px}\n"
".row .nm{color:var(--muted);flex:0 0 96px}\n"
".row .vl{color:var(--text);font-variant-numeric:tabular-nums;\n"
"  flex:0 0 34px;text-align:right}\n"
".row input[type=range]{margin:0}\n"
".row input[type=checkbox]{accent-color:var(--accent);width:16px;height:16px;\n"
"  cursor:pointer}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<header>\n"
"  <div class='mark'>WIZnet</div>\n"
/* The address the page was fetched from, shown because the whole point of
   serving this on both interfaces is putting two tabs side by side -- and a
   screenshot of two identical headers is a screenshot of nothing. */
"  <div class='sub'>OV3660 &middot; <span id='addr'></span></div>\n"
"  <div class='stackbox'>\n"
"    <div class='what' id='what'>&nbsp;</div>\n"
"    <div class='tag' id='stack'>&nbsp;</div>\n"
"  </div>\n"
"</header>\n"
"<main>\n"
"  <section class='card'>\n"
"    <h2><span id='dot' class='dot'></span>Live view</h2>\n"
"    <div class='view'><img id='view' src='/stream' alt='camera stream'></div>\n"
"  </section>\n"
"  <section class='card'>\n"
"    <h2>Control</h2>\n"
"    <div class='btns'>\n"
"      <button id='start'>START</button>\n"
"      <button id='stop'>STOP</button>\n"
"    </div>\n"
"    <label for='res'>Resolution</label>\n"
"    <select id='res'>\n"
"      <option value='320x240'>320 x 240</option>\n"
"      <option value='640x480'>640 x 480</option>\n"
"      <option value='800x600'>800 x 600</option>\n"
"      <option value='1280x720'>1280 x 720</option>\n"
"      <option value='1600x1200'>1600 x 1200</option>\n"
"    </select>\n"
"    <dl>\n"
"      <dt>State</dt><dd id='s-state'>-</dd>\n"
"      <dt>Resolution</dt><dd id='s-res'>-</dd>\n"
"      <dt>Frames</dt><dd id='s-frames'>-</dd>\n"
"      <dt>Dropped</dt><dd id='s-drop'>-</dd>\n"
"    </dl>\n"
"  </section>\n"
"  <section class='card wide'>\n"
"    <h2>Live performance</h2>\n"
"    <div class='charts'>\n"
"      <div>\n"
"        <div class='chead'>\n"
"          <div class='big'><span id='s-fps'>0.0</span><small>fps</small></div>\n"
"          <div class='stats'>\n"
"            <span>min <b id='s-min'>-</b></span>\n"
"            <span>avg <b id='s-avg'>-</b></span>\n"
"            <span>max <b id='s-max'>-</b></span>\n"
"          </div>\n"
"        </div>\n"
"        <canvas id='c-fps'></canvas>\n"
"      </div>\n"
"      <div>\n"
"        <div class='chead'>\n"
"          <div class='big'><span id='s-bw'>0.0</span><small>Mbps</small></div>\n"
"          <div class='stats'>\n"
"            <span>frame <b id='s-kb'>0</b> KB</span>\n"
"            <span>peak <b id='s-bwmax'>-</b></span>\n"
"          </div>\n"
"        </div>\n"
"        <canvas id='c-bw'></canvas>\n"
"      </div>\n"
"    </div>\n"
"    <div class='legend'>\n"
"      <span><i class='cap'></i>Capture <b id='s-capture'>0</b> ms</span>\n"
"      <span><i class='send'></i>Network send <b id='s-send'>0</b> ms</span>\n"
"    </div>\n"
"    <p class='hint' id='s-hint'>Start streaming to measure.</p>\n"
"  </section>\n"
"  <section class='card tight wide'>\n"
"    <div class='sweep'>\n"
"      <div>\n"
"        <label for='quality'>JPEG QUALITY <b id='v-quality'>12</b></label>\n"
"        <input type='range' id='quality' min='4' max='40' value='12'>\n"
"      </div>\n"
"      <div>\n"
"        <label for='xclk'>XCLK MHz <b id='v-xclk'>20</b></label>\n"
"        <input type='range' id='xclk' min='10' max='24' value='20'>\n"
"      </div>\n"
"      <button class='ghost' id='apply'>Apply</button>\n"
"      <button class='ghost' id='reset'>Defaults</button>\n"
"      <button class='ghost' id='recover'>Recover</button>\n"
"    </div>\n"
"  </section>\n"
"  <section class='card wide'>\n"
"    <h2>Sensor controls</h2>\n"
"    <div id='ctrls' class='ctrls'>loading...</div>\n"
"  </section>\n"
"</main>\n"
"<footer>Served from the device</footer>\n"
"<script>\n"
"var $=function(id){return document.getElementById(id)};\n"
"$('addr').textContent=location.host;\n"
"var css=getComputedStyle(document.documentElement);\n"
"var C=function(n){return css.getPropertyValue(n).trim()};\n"
"var last={},stalled=0,misses=0,wasGone=false;\n"
"var MAXP=60;\n"
"function mkChart(id,rgb,fixed,div){\n"
"  var c={cv:$(id),rgb:rgb,fixed:fixed,div:div,hist:[],shown:0,target:0};\n"
"  c.cx=c.cv.getContext('2d');\n"
"  return c;\n"
"}\n"
"var chF=mkChart('c-fps','0,184,212',30,3);\n"
"var chB=mkChart('c-bw','142,123,239',10,5);\n"
"var charts=[chF,chB];\n"
"function fit(){\n"
"  var d=window.devicePixelRatio||1;\n"
"  charts.forEach(function(c){\n"
"    var r=c.cv.getBoundingClientRect();\n"
"    c.cv.width=Math.max(1,r.width*d);c.cv.height=Math.max(1,r.height*d);\n"
"    c.cx.setTransform(d,0,0,d,0,0);\n"
"  });\n"
"}\n"
"window.addEventListener('resize',fit);fit();\n"
"function push(c,v){\n"
"  c.target=v;c.hist.push(v);if(c.hist.length>MAXP){c.hist.shift()}\n"
"}\n"
"function draw(c){\n"
"  var cx=c.cx,r=c.cv.getBoundingClientRect(),W=r.width,H=r.height;\n"
"  var L=34,R=W-6,T=10,B=H-14,n=c.hist.length;\n"
"  cx.clearRect(0,0,W,H);\n"
"  var mx=c.fixed,dv=c.div;\n"
"  cx.strokeStyle=C('--line');cx.fillStyle=C('--muted');\n"
"  cx.lineWidth=1;cx.font='10px Segoe UI,sans-serif';\n"
"  for(var g=0;g<=dv;g++){\n"
"    var y=T+(B-T)*g/dv,lv=mx*(1-g/dv);\n"
"    cx.beginPath();cx.moveTo(L,y);cx.lineTo(R,y);cx.stroke();\n"
"    cx.fillText(String(Math.round(lv)),4,y+3);\n"
"  }\n"
"  if(n<2){return}\n"
"  var step=(R-L)/(MAXP-1);\n"
"  var X=function(k){return R-(n-1-k)*step};\n"
"  var Y=function(v){return B-(B-T)*Math.min(v,mx)/mx};\n"
"  var path=function(){\n"
"    cx.beginPath();cx.moveTo(X(0),Y(c.hist[0]));\n"
"    for(var k=0;k<n-1;k++){\n"
"      var xc=(X(k)+X(k+1))/2,yc=(Y(c.hist[k])+Y(c.hist[k+1]))/2;\n"
"      cx.quadraticCurveTo(X(k),Y(c.hist[k]),xc,yc);\n"
"    }\n"
"    cx.lineTo(X(n-1),Y(c.hist[n-1]));\n"
"  };\n"
"  var gr=cx.createLinearGradient(0,T,0,B);\n"
"  gr.addColorStop(0,'rgba('+c.rgb+',.34)');\n"
"  gr.addColorStop(1,'rgba('+c.rgb+',0)');\n"
"  path();cx.lineTo(X(n-1),B);cx.lineTo(X(0),B);cx.closePath();\n"
"  cx.fillStyle=gr;cx.fill();\n"
"  path();cx.strokeStyle='rgb('+c.rgb+')';cx.lineWidth=2.5;\n"
"  cx.lineJoin='round';cx.stroke();\n"
"  cx.beginPath();cx.arc(X(n-1),Y(c.hist[n-1]),4,0,6.2832);\n"
"  cx.fillStyle='rgb('+c.rgb+')';cx.fill();\n"
"}\n"
"function tick(){\n"
"  charts.forEach(function(c){\n"
"    c.shown+=(c.target-c.shown)*0.12;\n"
"    if(c.hist.length){c.hist[c.hist.length-1]=c.shown}\n"
"    draw(c);\n"
"  });\n"
"  $('s-fps').textContent=chF.shown.toFixed(1);\n"
"  $('s-bw').textContent=chB.shown.toFixed(1);\n"
"  requestAnimationFrame(tick);\n"
"}\n"
"requestAnimationFrame(tick);\n"
/* 409 means the sensor turned a setting down. The body is still the status
   document, so render it either way -- the panel then snaps back to what the
   hardware actually has, which is the honest answer -- but say so, because a
   slider quietly returning to its old position reads as a broken page. */
"function call(p){\n"
"  return fetch(p).then(function(r){\n"
"    var refused=(r.status===409);\n"
"    return r.json().then(function(s){\n"
"      render(s);\n"
"      if(refused){$('s-hint').innerHTML='The sensor <b>refused</b> that '\n"
"        +'setting; showing what it actually has.'}\n"
"      return s;\n"
"    });\n"
"  });\n"
"}\n"
"function render(s){\n"
/* A poll getting through after the link was down is the earliest moment the
   view can come back. Waiting for the stall counter to reach three works too,
   but it spends three more seconds staring at a frozen frame for a link that is
   already known to be up. */
"  if(wasGone){wasGone=false;setTimeout(restartStream,300)}\n"
"  misses=0;\n"
"  $('s-state').textContent=s.streaming?'STREAMING':'STOPPED';\n"
"  $('s-res').textContent=s.res;\n"
"  $('s-frames').textContent=s.frames;\n"
"  $('s-drop').textContent=s.dropped;\n"
/* The dot is set further down, once the frame counter has been compared --
 * "streaming" alone does not mean frames are arriving. */
"  $('start').disabled=s.streaming;\n"
"  $('stop').disabled=!s.streaming;\n"
"  if($('res').value!==s.res){$('res').value=s.res}\n"
"  $('s-kb').textContent=s.kb;\n"
"  $('s-capture').textContent=s.capture_ms;\n"
"  $('s-send').textContent=s.send_ms;\n"
"  push(chF,s.fps);\n"
"  push(chB,s.fps*s.kb*8/1000);\n"
"  var mn=1e9,mx=0,sum=0,bmx=0,i,v;\n"
"  for(i=0;i<chF.hist.length;i++){v=chF.hist[i];\n"
"    if(v<mn){mn=v}if(v>mx){mx=v}sum+=v}\n"
"  for(i=0;i<chB.hist.length;i++){if(chB.hist[i]>bmx){bmx=chB.hist[i]}}\n"
"  if(chF.hist.length){\n"
"    $('s-min').textContent=mn.toFixed(1);\n"
"    $('s-max').textContent=mx.toFixed(1);\n"
"    $('s-avg').textContent=(sum/chF.hist.length).toFixed(1);\n"
"    $('s-bwmax').textContent=bmx.toFixed(1)+' Mbps';\n"
"  }\n"
"  $('s-hint').innerHTML=hint(s);\n"
/* The badge names the link, not the stack: someone opening two windows knows
   which cable they pulled, and "lwIP" asks them to know what lwIP is before the
   page has told them. The line beside it still names the stack, so nothing is
   lost -- WI-FI reads instantly and "Software TCP/IP on the MCU" explains it. */
"  if(s.link){\n"
"    var toe=(s.link==='TOE');\n"
"    $('stack').textContent=s.link;\n"
"    $('what').textContent=toe\n"
"      ?'Hardwired TCP/IP - the WIZnet chip terminates TCP'\n"
"      :'Software TCP/IP - lwIP terminates TCP on the MCU';\n"
"    document.documentElement.style.setProperty('--stack',\n"
"      toe?'#e01b24':'#1f2933');\n"
"    document.title=s.link+' camera';\n"
"  }\n"
"  if(document.activeElement!==$('quality')&&document.activeElement!==$('xclk')){\n"
"    $('quality').value=s.quality;$('v-quality').textContent=s.quality;\n"
"    $('xclk').value=s.xclk;$('v-xclk').textContent=s.xclk;\n"
"  }\n"
"  if(s.ctrl){renderCtrls(s.ctrl)}\n"
/* ---- Reconnect a stream that died under us ----
   `streaming` is what the user asked for, not what the connection is doing, so
   it stays true when the link drops -- correctly. But a multipart response that
   breaks is not retried by the browser, and this page only ever set img.src on
   START or a settings change. Pull the Ethernet cable and the view stays frozen
   even after the cable goes back in, until someone presses STOP and START.

   The device already reports the answer: if it says it is streaming and its
   frame counter has not moved for a few seconds, nothing is arriving. Ask for
   the stream again. While the cable is still out this fails quietly and retries;
   when it returns, the view comes back on its own. */
"  if(s.streaming&&last.frames!==undefined){\n"
"    if(s.frames===last.frames){stalled++}else{stalled=0}\n"
"    if(stalled>=3){stalled=0;restartStream()}\n"
"  }else{stalled=0}\n"
/* The dot follows the frames, not the flag: "STREAMING" with nothing arriving
   is the state this whole block exists because of. */
"  $('dot').className='dot'+((s.streaming&&stalled===0)?' live':'');\n"
"  last=s;\n"
"}\n"

/* ---- The status poll itself failed ----
   Everything above runs only when /api/status came back. Pull this interface's
   cable and the request never completes, so render() does not run at all: the
   stall counter never advances, the dot keeps whatever class it had, and the
   numbers stay at their last good values. The page then shows a live-looking
   view of an interface that is gone -- which is precisely the wrong answer, and
   was the reason a "failover" screenshot showed both sides healthy.

   So the failed poll is a state of its own. The device's own numbers are left
   on screen deliberately -- they are the last thing that was true, and blanking
   them would throw away the comparison the page exists to make -- but the state
   line and the dot say the link is down.

   Two consecutive misses rather than one, because a single dropped poll happens
   on a busy link and a page that flickers "unreachable" every few minutes is
   worse than one that is two seconds late. */
"function offline(){\n"
"  if(++misses<2){return}\n"
"  stalled=0;wasGone=true;\n"
"  $('s-state').textContent='UNREACHABLE';\n"
"  $('dot').className='dot gone';\n"
"  $('s-hint').innerHTML='No reply from <b>'+location.host+'</b>. '\n"
"    +'Values below are the last ones it reported.';\n"
"}\n"

/* ---- Sensor controls, built from what the device reports ----
   The device owns the list: names, labels, groups and ranges all arrive from
   /api/controls, so a control added in cam_source.c appears here on its own.
   A range of 0..1 is a checkbox and anything wider is a slider. */
"var ctrlEls={};\n"
"function buildCtrls(list){\n"
"  var box=$('ctrls'),groups={},order=[];\n"
"  list.forEach(function(c){\n"
"    if(!groups[c.group]){groups[c.group]=[];order.push(c.group)}\n"
"    groups[c.group].push(c);\n"
"  });\n"
"  box.innerHTML='';\n"
"  order.forEach(function(g){\n"
"    var d=document.createElement('div');d.className='grp';\n"
"    var h=document.createElement('h3');h.textContent=g;d.appendChild(h);\n"
"    groups[g].forEach(function(c){d.appendChild(ctrlRow(c))});\n"
"    box.appendChild(d);\n"
"  });\n"
"}\n"
"function ctrlRow(c){\n"
"  var row=document.createElement('div');row.className='row';\n"
"  var nm=document.createElement('span');nm.className='nm';\n"
"  nm.textContent=c.label;row.appendChild(nm);\n"
"  var toggle=(c.min===0&&c.max===1);\n"
"  var el=document.createElement('input');\n"
"  el.type=toggle?'checkbox':'range';\n"
"  if(!toggle){el.min=c.min;el.max=c.max}\n"
"  row.appendChild(el);\n"
"  var vl=null;\n"
"  if(!toggle){\n"
"    vl=document.createElement('span');vl.className='vl';row.appendChild(vl);\n"
/* Slider: follow the thumb live for the label, but only send on release, or a
   drag across the range fires a request per step and each one re-inits the
   sensor. */
"    el.oninput=function(){vl.textContent=el.value};\n"
"    el.onchange=function(){send(c.name,el.value)};\n"
"  }else{\n"
"    el.onchange=function(){send(c.name,el.checked?1:0)};\n"
"  }\n"
"  ctrlEls[c.name]={el:el,vl:vl,toggle:toggle};\n"
"  return row;\n"
"}\n"
"function send(name,value){call('/api/cam?'+name+'='+value).catch(offline)}\n"
"function renderCtrls(values){\n"
"  for(var k in values){\n"
"    var e=ctrlEls[k];\n"
/* Leave whatever the user is touching alone; the poll would otherwise snap the
   thumb back mid-drag. */
"    if(!e||document.activeElement===e.el){continue}\n"
"    if(e.toggle){e.el.checked=!!values[k]}\n"
"    else{e.el.value=values[k];if(e.vl){e.vl.textContent=values[k]}}\n"
"  }\n"
"}\n"
"fetch('/api/controls').then(function(r){return r.json()}).then(buildCtrls)\n"
"  .catch(function(){$('ctrls').textContent='controls unavailable'});\n"
"function hint(s){\n"
"  if(!s.streaming){return 'Start streaming to measure.'}\n"
"  if(s.capture_ms>=s.send_ms){\n"
"    return 'Bottleneck: <b>capture</b>. Getting the frame out of the sensor'+\n"
"      ' takes longer than putting it on the wire, so the rate is set by the'+\n"
"      ' sensor mode and the XCLK below, not by the network.'}\n"
"  return 'Bottleneck: <b>network send</b>. Pushing the JPEG out takes the'+\n"
"    ' longest - this is where a hardwired stack and a software stack differ.'+\n"
"    ' Raising JPEG quality shrinks the frame and moves the line.';\n"
"}\n"
"var seq=0;\n"
"function restartStream(){$('view').src='/stream?'+(++seq)}\n"
/* Every one of these ends in .catch(offline) for the same reason the poll does:
   a control that cannot reach the device is itself evidence the link is down,
   and an uncaught rejection here would leave the page looking healthy while the
   button quietly did nothing. */
"$('start').onclick=function(){call('/api/start').then(function(){\n"
"  setTimeout(restartStream,150)}).catch(offline)};\n"
"$('stop').onclick=function(){call('/api/stop').catch(offline)};\n"
"$('res').onchange=function(){\n"
"  var v=this.value;\n"
"  charts.forEach(function(c){c.hist=[];c.shown=0;c.target=0});\n"
"  call('/api/res?v='+v).then(function(){setTimeout(restartStream,400)})\n"
"    .catch(offline);\n"
"};\n"
"$('quality').oninput=function(){$('v-quality').textContent=this.value};\n"
"$('xclk').oninput=function(){$('v-xclk').textContent=this.value};\n"
"function applyCam(q,x){\n"
"  $('quality').value=q;$('xclk').value=x;\n"
"  $('v-quality').textContent=q;$('v-xclk').textContent=x;\n"
"  charts.forEach(function(c){c.hist=[];c.shown=0;c.target=0});\n"
"  return call('/api/cam?quality='+q+'&xclk='+x)\n"
"    .then(function(){setTimeout(restartStream,400)}).catch(offline);\n"
"}\n"
"$('apply').onclick=function(){applyCam($('quality').value,$('xclk').value)};\n"
"$('reset').onclick=function(){applyCam(12,20)};\n"
"$('recover').onclick=function(){\n"
"  $('recover').textContent='...';\n"
"  call('/api/reset').then(function(){\n"
"    $('recover').textContent='Recover';setTimeout(restartStream,400)})\n"
"    .catch(function(){$('recover').textContent='Recover';offline()});\n"
"};\n"
"setInterval(function(){call('/api/status').catch(offline)},1000);\n"
"call('/api/status').catch(offline);\n"
"</script>\n"
"</body>\n"
"</html>\n";

#endif /* WEB_PAGE_H */
