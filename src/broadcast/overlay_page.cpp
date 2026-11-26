#include "overlay_server.hpp"

namespace deckhand::broadcast {

// Original overlay: a card that slides in with the on-air track. Query string options:
//   theme=dark|light|accent|minimal   size=<10..120 px>   align=left|center|right
//   accent=<hex colour without #>     label=<small caption text, empty hides it>   art=0 (hide artwork)
const char* overlay_html() {
  return R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DeckHand now playing</title>
<link rel="icon" href="data:image/png;base64,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">
<style>
:root{--size:28px;--accent:#ff4f8b;--fg:#fff;--fg2:rgba(255,255,255,.72);--bg:rgba(14,15,20,.78);--edge:rgba(255,255,255,.14)}
html,body{margin:0;height:100%;background:transparent;overflow:hidden;
  font-family:-apple-system,BlinkMacSystemFont,"Segoe UI","Helvetica Neue",Arial,sans-serif}
body{display:flex;align-items:flex-end;padding:2vmin;box-sizing:border-box}
body.left{justify-content:flex-start}body.center{justify-content:center}body.right{justify-content:flex-end}
body.light{--fg:#14151a;--fg2:rgba(20,21,26,.68);--bg:rgba(255,255,255,.86);--edge:rgba(0,0,0,.12)}
#card{display:flex;align-items:center;gap:.6em;max-width:92vw;font-size:var(--size);color:var(--fg);
  background:var(--bg);border:1px solid var(--edge);border-radius:.45em;padding:.45em .8em .45em .6em;
  box-shadow:0 .2em .9em rgba(0,0,0,.35);backdrop-filter:blur(6px);
  opacity:0;transform:translateY(.5em);transition:opacity .45s ease,transform .45s ease}
body.accent #card{border-left:.22em solid var(--accent)}
body.minimal #card{background:none;border:0;box-shadow:none;backdrop-filter:none;padding:0;text-shadow:0 .05em .25em rgba(0,0,0,.85)}
body.minimal.light #card{text-shadow:0 .05em .25em rgba(255,255,255,.85)}
#card.show{opacity:1;transform:none}
#card.swap #text{opacity:0;transform:translateY(.3em)}
#art{width:2.3em;height:2.3em;border-radius:.25em;object-fit:cover;display:none}
#art.has{display:block}
#text{min-width:0;transition:opacity .3s ease,transform .3s ease;text-align:left}
body.center #text{text-align:center}body.right #text{text-align:right}
#label{font-size:.42em;letter-spacing:.16em;text-transform:uppercase;color:var(--accent);font-weight:700}
#label:empty{display:none}
#title{font-size:1em;font-weight:700;line-height:1.15;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#artist{font-size:.66em;color:var(--fg2);line-height:1.25;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#artist:empty{display:none}
@media (prefers-reduced-motion:reduce){#card,#text{transition:none}}
</style></head>
<body class="left dark">
<div id="card"><img id="art" alt=""><div id="text"><div id="label"></div><div id="title"></div><div id="artist"></div></div></div>
<script>
(function(){
  var q=new URLSearchParams(location.search),b=document.body,card=document.getElementById('card');
  var theme=q.get('theme')||'dark';if(['dark','light','accent','minimal'].indexOf(theme)<0)theme='dark';
  var align=q.get('align')||'left';if(['left','center','right'].indexOf(align)<0)align='left';
  var size=parseInt(q.get('size')||'28',10);if(!(size>=10&&size<=120))size=28;
  var accent=q.get('accent');
  b.className=align+' '+(theme==='light'?'light':'dark')+(theme==='accent'||theme==='minimal'?' '+theme:'');
  document.documentElement.style.setProperty('--size',size+'px');
  if(accent&&/^[0-9a-fA-F]{3,8}$/.test(accent))document.documentElement.style.setProperty('--accent','#'+accent);
  var label=q.has('label')?q.get('label'):'Now playing';
  var showArt=q.get('art')!=='0';
  var el={label:document.getElementById('label'),title:document.getElementById('title'),
          artist:document.getElementById('artist'),art:document.getElementById('art')};
  var shown=null,timer=null;
  function apply(d){
    var key=d.on_air?[d.title,d.artist,d.album,d.artwork].join('\u0001'):'';
    if(key===shown)return;
    shown=key;
    clearTimeout(timer);
    if(!d.on_air){card.classList.remove('show');return;}
    function fill(){
      el.label.textContent=label;
      el.title.textContent=d.title||'';
      el.artist.textContent=d.artist||'';
      if(showArt&&d.artwork){el.art.src=d.artwork;el.art.className='has';}else{el.art.removeAttribute('src');el.art.className='';}
      card.classList.remove('swap');card.classList.add('show');
    }
    if(card.classList.contains('show')){card.classList.add('swap');timer=setTimeout(fill,320);}else fill();
  }
  function poll(){
    fetch('now-playing.json',{cache:'no-store'}).then(function(r){return r.json();}).then(apply).catch(function(){});
  }
  var pollTimer=null;
  function connect(){
    if(!window.EventSource){pollTimer=setInterval(poll,2000);poll();return;}
    var es=new EventSource('events');
    es.onmessage=function(e){try{apply(JSON.parse(e.data));}catch(x){}};
    es.onopen=function(){if(pollTimer){clearInterval(pollTimer);pollTimer=null;}};
    es.onerror=function(){if(!pollTimer)pollTimer=setInterval(poll,3000);};
  }
  connect();
})();
</script></body></html>
)HTML";
}

}  // namespace deckhand::broadcast
