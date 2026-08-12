/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * The page served at GET /.
 *
 * Kept as a string literal rather than an embedded file so the example stays a
 * plain component with no extra build wiring; at ~2 KB it costs less than the
 * EMBED_FILES machinery would explain. It is deliberately dependency-free --
 * no CDN, no framework -- because the device it is served from may be on a
 * network with no route to the internet.
 *
 * Relative WebSocket URL on purpose: `new WebSocket('ws://' + location.host +
 * '/ws')` follows whatever address the browser used to fetch the page, so the
 * same firmware works on Ethernet, on Wi-Fi, and after the address changes,
 * with nothing to edit.
 */
#ifndef WS_INDEX_HTML_H
#define WS_INDEX_HTML_H

static const char kIndexHtml[] =
"<!doctype html>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>WSM Driver WebSocket</title>\n"
"<style>\n"
"body{font:14px/1.6 ui-monospace,Consolas,monospace;margin:2rem auto;max-width:44rem;padding:0 1rem}\n"
"h1{font-size:1.2rem}\n"
"#s{padding:.2rem .5rem;border-radius:3px;background:#eee}\n"
"#s.on{background:#cfc}#s.off{background:#fcc}\n"
"#log{border:1px solid #bbb;height:18rem;overflow:auto;padding:.5rem;white-space:pre-wrap;margin:1rem 0}\n"
".rx{color:#060}.tx{color:#039}.sys{color:#777}\n"
"input,button{font:inherit;padding:.35rem}input{width:18rem}\n"
"</style>\n"
"<h1>WIZnet WSM Driver &mdash; WebSocket echo</h1>\n"
"<p>Status <span id=s class=off>connecting</span></p>\n"
"<div id=log></div>\n"
"<p><input id=m value=\"hello from the browser\" disabled>\n"
"<button id=b disabled>Send</button>\n"
"<button id=big disabled>300 bytes</button>\n"
"<button id=bin disabled>Binary</button></p>\n"
"<script>\n"
"var L=document.getElementById('log'),S=document.getElementById('s');\n"
"function log(c,t){var d=document.createElement('div');d.className=c;\n"
"d.textContent=new Date().toLocaleTimeString()+'  '+t;L.appendChild(d);L.scrollTop=L.scrollHeight}\n"
"function en(o){S.textContent=o?'connected':'disconnected';S.className=o?'on':'off';\n"
"['m','b','big','bin'].forEach(function(i){document.getElementById(i).disabled=!o})}\n"
"var ws;\n"
"function open_(){\n"
"  ws=new WebSocket('ws://'+location.host+'/ws');ws.binaryType='arraybuffer';\n"
"  ws.onopen=function(){log('sys','open');en(true)};\n"
"  ws.onclose=function(e){log('sys','closed, code '+e.code);en(false);\n"
"    setTimeout(open_,2000)};\n"
"  ws.onmessage=function(e){typeof e.data=='string'\n"
"    ?log('rx','<- text '+e.data.length+'B  '+e.data)\n"
"    :log('rx','<- binary '+e.data.byteLength+'B')};\n"
"}\n"
"function tx(p,l){ws.send(p);log('tx','-> '+l)}\n"
"document.getElementById('b').onclick=function(){var v=document.getElementById('m').value;\n"
"  tx(v,'text '+v.length+'B  '+v)};\n"
"document.getElementById('big').onclick=function(){var v=Array(301).join('x');\n"
"  tx(v,'text '+v.length+'B  (extended length path)')};\n"
"document.getElementById('bin').onclick=function(){var b=new Uint8Array([0,1,2,3,255,254]);\n"
"  tx(b,'binary '+b.length+'B')};\n"
"open_();\n"
"</script>\n";

#endif /* WS_INDEX_HTML_H */
