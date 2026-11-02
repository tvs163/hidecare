// Hidecare captive portal page. Russian, no external resources.
#pragma once

namespace hidecare::setup {

inline constexpr char kPortalPage[] = R"HTML(<!doctype html>
<html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Hidecare — настройка</title>
<style>
body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;margin:0;background:#f4f6f8;color:#1b1f24}
main{max-width:420px;margin:0 auto;padding:20px}
h1{font-size:22px;margin:0 0 12px}
.code{background:#fff;border-radius:12px;padding:14px;text-align:center;margin-bottom:16px;box-shadow:0 1px 3px #0002}
.code b{display:block;font-size:34px;letter-spacing:2px;margin-top:4px}
label{display:block;font-size:14px;margin:12px 0 4px}
input{width:100%;box-sizing:border-box;font-size:17px;padding:12px;border:1px solid #c5ccd3;border-radius:10px}
button{width:100%;font-size:18px;padding:14px;margin-top:16px;border:0;border-radius:10px;background:#1f6feb;color:#fff}
button:disabled{background:#8aa9d8}
#nets{background:#fff;border-radius:10px;max-height:240px;overflow:auto;box-shadow:0 1px 3px #0002}
#nets div{padding:12px;border-bottom:1px solid #eef0f2;cursor:pointer}
#nets div.sel{background:#e3eefd}
#nets small{float:right;color:#667}
.msg{margin-top:16px;padding:14px;border-radius:10px;display:none}
.ok{background:#dcf5e3;display:block}.err{background:#fde2e1;display:block}.wait{background:#fff6d6;display:block}
.link{color:#1f6feb;font-size:13px;margin-top:6px;background:none;padding:4px;width:auto}
</style></head><body><main>
<h1>Настройка датчика Hidecare</h1>
<div class="code">Код устройства<b id="code">…</b></div>
<label>Выберите вашу Wi-Fi сеть</label>
<div id="nets"><div>Поиск сетей…</div></div>
<button class="link" type="button" onclick="scan()">Обновить список</button>
<label for="ssid">Имя сети (можно ввести вручную)</label>
<input id="ssid" autocomplete="off" autocapitalize="none">
<label for="pw">Пароль сети</label>
<input id="pw" type="password" autocomplete="off">
<button id="go" onclick="go()">Подключить</button>
<div id="msg" class="msg"></div>
</main><script>
var code='',busy=false;
function $(i){return document.getElementById(i)}
function show(c,t){var m=$('msg');m.className='msg '+c;m.innerHTML=t}
function esc(s){return s.replace(/[&<>"]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]})}
function j(u,o){return fetch(u,o).then(function(r){return r.json()})}
function scan(){j('/scan').then(function(d){
 if(d.code){code=d.code;$('code').textContent=code}
 if(d.scanning&&!d.networks.length){setTimeout(scan,1500);return}
 var h='';d.networks.forEach(function(n){h+='<div data-s="'+esc(n.s)+'">'+esc(n.s)+(n.l?' 🔒':'')+'<small>'+n.q+'</small></div>'});
 $('nets').innerHTML=h||'<div>Сети не найдены. Введите имя вручную.</div>';
 Array.prototype.forEach.call($('nets').children,function(e){e.onclick=function(){
  Array.prototype.forEach.call($('nets').children,function(x){x.className=''});
  e.className='sel';$('ssid').value=e.getAttribute('data-s');$('pw').focus()}});
 if(d.scanning)setTimeout(scan,1500)}).catch(function(){setTimeout(scan,2000)})}
function go(){var s=$('ssid').value;if(!s){show('err','Выберите сеть или введите её имя.');return}
 busy=true;$('go').disabled=true;show('wait','Подключаемся к «'+esc(s)+'»… Это займёт до минуты.');
 var b='ssid='+encodeURIComponent(s)+'&password='+encodeURIComponent($('pw').value);
 fetch('/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b})
 .then(function(){poll(0)}).catch(function(){poll(0)})}
function poll(n){setTimeout(function(){j('/status').then(function(d){
 if(d.state=='ok'){show('ok','<b>Готово! Датчик подключён к Wi-Fi.</b><br><br>Код устройства: <b>'+esc(d.code)+'</b><br>Теперь отключитесь от сети Hidecare, откройте Telegram и добавьте датчик в <a href="'+d.url+'">@hidecarebot</a> (кнопка «Добавить датчик», код '+esc(d.code)+').');$('go').disabled=false;return}
 if(d.state=='failed'){show('err','Не удалось подключиться: '+esc(d.msg||'')+'<br>Проверьте имя сети и пароль. Датчик работает только с сетями 2,4 ГГц.');$('go').disabled=false;return}
 poll(n+1)}).catch(function(){if(n>40){show('wait','Связь с датчиком прервалась. Если он подключился, точка Hidecare скоро исчезнет — проверьте датчик в @hidecarebot по коду '+esc(code)+'.');$('go').disabled=false}else poll(n+1)})},1500)}
scan();
</script></body></html>)HTML";

}  // namespace hidecare::setup
