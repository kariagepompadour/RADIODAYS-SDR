'use strict';
const wifiToggleButton=document.querySelector('[data-action="wifi-toggle"]');
if(wifiToggleButton)wifiToggleButton.onclick=()=>command('wifi-toggle');
addEventListener('keydown',event=>{
  if(event.target.matches('input,select')||event.key.toLowerCase()!=='x')return;
  event.preventDefault();
  command('wifi-toggle');
});
