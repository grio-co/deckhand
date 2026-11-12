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
