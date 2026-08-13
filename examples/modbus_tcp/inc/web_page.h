/**
 * @file    web_page.h
 * @brief   Embedded single-page UI for the Modbus TCP server example.
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * One self-contained string: no external CSS, JS, fonts or images, because the
 * device is the only server the browser can reach. An installed Modbus slave is
 * usually on a segment with no route to the internet, which is exactly when a
 * CDN reference turns a settings page into a blank one.
 *
 * Three panels: what the server is doing, what is in its registers, and what
 * can be changed. The register view is the one worth having open while a master
 * polls -- writes from the Modbus side appear in the browser without a reload,
 * which makes "is the master actually writing where I think it is" a question
 * you can answer by looking.
 *
 * HTML attributes use single quotes so the C string needs no escaping.
 */

#ifndef WEB_PAGE_H
#define WEB_PAGE_H

static const char WEB_INDEX_PAGE[] =
"<!DOCTYPE html>\n"
"<html lang='en'>\n"
"<head>\n"
"<meta charset='utf-8'>\n"
"<meta name='viewport' content='width=device-width,initial-scale=1'>\n"
"<title>WIZnet Modbus TCP</title>\n"
"<style>\n"
":root{--bg:#fff;--panel:#fff;--line:#dfe5ec;--text:#101418;--muted:#69737f;\n"
"  --accent:#e01b24;--ok:#3ddc84;--warn:#f5a623}\n"
"*{box-sizing:border-box}\n"
"body{margin:0;background:var(--bg);color:var(--text);\n"
"  font-family:'Segoe UI',Roboto,Helvetica,Arial,sans-serif}\n"
"header{display:flex;align-items:baseline;gap:14px;padding:12px 22px;\n"
"  border-bottom:3px solid var(--accent)}\n"
"header .mark{font-size:24px;font-weight:800;color:var(--accent);\n"
"  letter-spacing:1px}\n"
"header .sub{color:var(--muted);font-size:13px}\n"
"header .dot{margin-left:auto;font-size:13px;color:var(--muted)}\n"
"header .dot i{display:inline-block;width:9px;height:9px;border-radius:50%;\n"
"  background:var(--muted);margin-right:7px}\n"
"header .dot.live i{background:var(--ok);box-shadow:0 0 8px var(--ok)}\n"
"main{max-width:1180px;margin:0 auto;padding:16px;display:grid;\n"
"  grid-template-columns:1fr 320px;gap:16px}\n"
"@media(max-width:900px){main{grid-template-columns:1fr}}\n"
".card{background:var(--panel);border:1px solid var(--line);border-radius:10px;\n"
"  padding:14px;box-shadow:0 1px 3px rgba(16,20,24,.06)}\n"
".wide{grid-column:1/-1}\n"
"h2{margin:0 0 12px;font-size:12px;text-transform:uppercase;\n"
"  letter-spacing:1.4px;color:var(--muted);font-weight:600}\n"
"dl{display:grid;grid-template-columns:auto 1fr;gap:8px 12px;margin:0;\n"
"  font-size:13px}\n"
"dt{color:var(--muted)}\n"
"dd{margin:0;text-align:right;font-variant-numeric:tabular-nums}\n"
"label{display:block;font-size:11px;color:var(--muted);margin:10px 0 4px;\n"
"  text-transform:uppercase;letter-spacing:.8px}\n"
"input{width:100%;padding:8px;border:1px solid var(--line);border-radius:7px;\n"
"  font:inherit;font-size:13px;color:var(--text);background:var(--bg)}\n"
"button{margin-top:14px;width:100%;padding:10px;border:0;border-radius:7px;\n"
"  background:var(--accent);color:#fff;font-weight:700;font-size:13px;\n"
"  letter-spacing:.5px;cursor:pointer}\n"
"button:disabled{opacity:.4;cursor:not-allowed}\n"
"#msg{margin-top:12px;font-size:13px;line-height:1.5;display:none}\n"
"#msg.show{display:block}\n"
"#msg.ok{color:#0a7a3d}\n"
"#msg.err{color:var(--accent)}\n"

/* Registers: a dense grid, because sixty-four of anything is a wall of numbers
   unless the index is right next to the value. */
".tabs{display:flex;gap:6px;margin-bottom:12px;flex-wrap:wrap}\n"
".tabs button{margin:0;width:auto;padding:6px 12px;background:transparent;\n"
"  border:1px solid var(--line);color:var(--muted);font-weight:600;\n"
"  font-size:11px;text-transform:uppercase}\n"
".tabs button.on{background:var(--accent);border-color:var(--accent);\n"
"  color:#fff}\n"
".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(88px,1fr));\n"
"  gap:4px;font-size:12px;font-variant-numeric:tabular-nums}\n"
".cell{display:flex;justify-content:space-between;gap:6px;padding:4px 7px;\n"
"  border:1px solid var(--line);border-radius:5px}\n"
".cell .i{color:var(--muted)}\n"
".cell.on{background:#eaf9f0;border-color:var(--ok)}\n"
".cell.chg{background:#fff4e0;border-color:var(--warn)}\n"
"footer{text-align:center;color:var(--muted);font-size:12px;padding:6px 0 22px}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<header>\n"
"  <div class='mark'>WIZnet</div>\n"
"  <div class='sub'>Modbus TCP Server</div>\n"
"  <div class='dot' id='dot'><i></i><span id='dot-t'>connecting</span></div>\n"
"</header>\n"
"<main>\n"
"  <section class='card'>\n"
"    <h2>Server</h2>\n"
"    <dl>\n"
"      <dt>State</dt><dd id='s-state'>-</dd>\n"
"      <dt>Ethernet link</dt><dd id='s-link'>-</dd>\n"
"      <dt>MAC</dt><dd id='s-mac'>-</dd>\n"
"      <dt>Address</dt><dd id='s-ip'>-</dd>\n"
"      <dt>Modbus port</dt><dd id='s-port'>-</dd>\n"
"      <dt>Master</dt><dd id='s-client'>-</dd>\n"
"      <dt>Sessions</dt><dd id='s-sessions'>-</dd>\n"
"      <dt>Requests</dt><dd id='s-req'>-</dd>\n"
"      <dt>Exceptions</dt><dd id='s-exc'>-</dd>\n"
"      <dt>Last function</dt><dd id='s-last'>-</dd>\n"
"      <dt>Uptime</dt><dd id='s-up'>-</dd>\n"
"      <dt>Free heap</dt><dd id='s-heap'>-</dd>\n"
"    </dl>\n"
"  </section>\n"
"  <section class='card'>\n"
"    <h2>Network</h2>\n"
"    <label for='f-ip'>IP address</label>\n"
"    <input id='f-ip' placeholder='192.168.11.2'>\n"
"    <label for='f-mask'>Subnet mask</label>\n"
"    <input id='f-mask' placeholder='255.255.255.0'>\n"
"    <label for='f-gw'>Gateway</label>\n"
"    <input id='f-gw' placeholder='192.168.11.1'>\n"
"    <label for='f-port'>Modbus port</label>\n"
"    <input id='f-port' type='number' min='1' max='65535' placeholder='502'>\n"
"    <button id='save'>Save and apply</button>\n"
"    <div id='msg'></div>\n"
"  </section>\n"
"  <section class='card wide'>\n"
"    <h2>Registers</h2>\n"
"    <div class='tabs'>\n"
"      <button data-t='holding' class='on'>Holding</button>\n"
"      <button data-t='input'>Input</button>\n"
"      <button data-t='coil'>Coils</button>\n"
"      <button data-t='discrete'>Discrete</button>\n"
"    </div>\n"
"    <div class='grid' id='grid'></div>\n"
"  </section>\n"
"</main>\n"
"<footer>Served from the device</footer>\n"
"<script>\n"
"var $=function(id){return document.getElementById(id)};\n"
"var tab='holding',regs=null,prev=null,touched=false,applying=false;\n"

/* Any keystroke in the settings panel stops the poller from overwriting the
   fields underneath the user; a save or a reload gives them back. */
"['f-ip','f-mask','f-gw','f-port'].forEach(function(id){\n"
"  $(id).oninput=function(){touched=true}\n"
"});\n"

"function live(on,text){\n"
"  $('dot').className='dot'+(on?' live':'');\n"
"  $('dot-t').textContent=text;\n"
"}\n"

"function uptime(sec){\n"
"  var d=Math.floor(sec/86400),h=Math.floor(sec%86400/3600);\n"
"  var m=Math.floor(sec%3600/60);\n"
"  if(d){return d+'d '+h+'h'}\n"
"  if(h){return h+'h '+m+'m'}\n"
"  return m+'m '+(sec%60)+'s';\n"
"}\n"
"function fn(code){\n"
"  var m={1:'01 Read Coils',2:'02 Read Discrete',3:'03 Read Holding',\n"
"    4:'04 Read Input',5:'05 Write Coil',6:'06 Write Register',\n"
"    15:'0F Write Coils',16:'10 Write Registers'};\n"
"  return code?(m[code]||('0x'+code.toString(16))):'-';\n"
"}\n"

"function status(){\n"
"  return fetch('/api/status').then(function(r){return r.json()}).then(function(s){\n"
"    live(true,'connected');\n"
"    $('s-state').textContent=s.running?'RUNNING':'STOPPED';\n"
/* The PHY link, not the driver's own "brought up" flag: a device with the cable
   out otherwise reports itself healthy right until someone tries to reach it. */
"    $('s-link').textContent=s.link?'up':'DOWN';\n"
"    $('s-link').style.color=s.link?'':'var(--accent)';\n"
"    $('s-mac').textContent=s.mac||'-';\n"
"    $('s-ip').textContent=s.ip;\n"
"    $('s-port').textContent=s.port;\n"
"    $('s-client').textContent=s.client?'connected':'idle';\n"
"    $('s-sessions').textContent=s.sessions;\n"
"    $('s-req').textContent=s.requests;\n"
"    $('s-exc').textContent=s.exceptions\n"
"      +(s.last_exception?(' (last 0x0'+s.last_exception+')'):'');\n"
"    $('s-last').textContent=fn(s.last_function);\n"
"    $('s-up').textContent=uptime(s.uptime);\n"
/* Both numbers, because the low-water mark is the one that reveals a leak: free
   heap moves with every request, the minimum only ever falls. */
"    $('s-heap').textContent=(s.heap/1024).toFixed(0)+' KB (min '\n"
"      +(s.heap_min/1024).toFixed(0)+')';\n"
/* Saved but not running. The two diverge when a stuck master keeps the Modbus
   task from stopping, and a device that quietly waits for a reboot to change
   its port is one nobody will think to check. */
"    if(s.pending&&!applying){\n"
"      show('err','Settings are saved but not applied yet. They will take '\n"
"        +'effect at the next reboot.');\n"
"    }\n"
"    if(!touched){\n"
"      $('f-ip').value=s.ip;$('f-mask').value=s.mask;\n"
"      $('f-gw').value=s.gateway;$('f-port').value=s.port;\n"
"    }\n"
"  }).catch(function(){live(false,'no response')});\n"
"}\n"

"function registers(){\n"
"  return fetch('/api/registers').then(function(r){return r.json()}).then(function(d){\n"
"    prev=regs;regs=d;draw();\n"
"  }).catch(function(){});\n"
"}\n"

/* Values that moved since the last poll are highlighted, which is what makes
   this useful while a master is writing: the change is visible even when the
   number it changed to is unremarkable. */
"function draw(){\n"
"  if(!regs){return}\n"
"  var a=regs[tab],b=prev?prev[tab]:null,bits=(tab==='coil'||tab==='discrete');\n"
"  var out='';\n"
"  for(var i=0;i<a.length;i++){\n"
"    var cls='cell';\n"
"    if(bits&&a[i]){cls+=' on'}\n"
"    if(b&&b[i]!==a[i]){cls+=' chg'}\n"
"    out+=\"<div class='\"+cls+\"'><span class='i'>\"+i+\"</span><span>\"\n"
"      +(bits?(a[i]?'ON':'OFF'):a[i])+'</span></div>';\n"
"  }\n"
"  $('grid').innerHTML=out;\n"
"}\n"

"Array.prototype.forEach.call(document.querySelectorAll('.tabs button'),\n"
"  function(b){b.onclick=function(){\n"
"    tab=b.getAttribute('data-t');prev=null;\n"
"    Array.prototype.forEach.call(document.querySelectorAll('.tabs button'),\n"
"      function(x){x.className=(x===b)?'on':''});\n"
"    draw();\n"
"  }});\n"

"function show(cls,text){\n"
"  var m=$('msg');m.className='show '+cls;m.textContent=text;\n"
"}\n"

"$('save').onclick=function(){\n"
"  var body={ip:$('f-ip').value.trim(),mask:$('f-mask').value.trim(),\n"
"    gateway:$('f-gw').value.trim(),port:parseInt($('f-port').value,10)};\n"
"  $('save').disabled=true;\n"
"  show('','Saving...');\n"
"  fetch('/api/config',{method:'POST',body:JSON.stringify(body)})\n"
"    .then(function(r){return r.json()})\n"
"    .then(function(res){\n"
"      $('save').disabled=false;\n"
"      if(!res.ok){show('err',res.error);return}\n"
"      touched=false;applying=true;\n"
"      show('ok',res.message);\n"
/* The device is about to move. Following it only works when the browser is on
   the same subnet as the new address, so the link is offered rather than
   navigated to -- a redirect that lands nowhere is worse than a click. */
"      var url='http://'+res.ip+'/';\n"
"      var m=$('msg');\n"
"      m.innerHTML=res.message+\" <a href='\"+url+\"'>\"+url+'</a>';\n"
"    })\n"
"    .catch(function(){\n"
"      $('save').disabled=false;\n"
"      show('err','No response. If the address changed, the device is already '\n"
"        +'on the new one.');\n"
"    });\n"
"};\n"

"function poll(){\n"
"  status();\n"
"  if(!applying){registers()}\n"
"}\n"
"poll();setInterval(poll,1000);\n"
"</script>\n"
"</body>\n"
"</html>\n";

#endif /* WEB_PAGE_H */
