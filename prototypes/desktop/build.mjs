import {readFile,writeFile} from 'node:fs/promises';
const root=new URL('./',import.meta.url);
let html=await readFile(new URL('template.html',root),'utf8');
const theme=JSON.parse(await readFile(new URL('../../resources/theme/theme.json',root),'utf8'));
function resolveColor(binding,colors){
  const source=typeof binding==='string'?binding:binding.source;
  const color=colors[source];
  if(!/^#[\da-f]{6}([\da-f]{2})?$/i.test(color||''))throw new Error('Invalid theme color: '+source);
  if(typeof binding==='string'||color.length===7)return color;
  const base=colors[binding.over];
  if(!/^#[\da-f]{6}$/i.test(base||''))throw new Error('Invalid opaque surface: '+binding.over);
  const alpha=parseInt(color.slice(7),16)/255;
  return '#'+[1,3,5].map(i=>Math.round(parseInt(color.slice(i,i+2),16)*alpha+parseInt(base.slice(i,i+2),16)*(1-alpha)).toString(16).padStart(2,'0')).join('');
}
const palette=Object.entries(theme.themes).map(([mode,values])=>{
  const colors={...values.colors,...values.adaptations};
  const rules=Object.entries(theme.bindings).map(([key,binding])=>'--'+key+':'+resolveColor(binding,colors)+';');
  for(const [key,value] of Object.entries(values.product_colors))rules.push('--'+key+':'+value+';');
  return (mode==='light'?':root':':root[data-theme="dark"]')+'{color-scheme:'+mode+';'+rules.join('')+'}';
}).join('\n');
const themeRules=await readFile(new URL('src/theme.css',root),'utf8');
html=html.replace('/*__THEME__*/',()=>palette+'\n'+themeRules);
const inputs={STYLES:'styles.css',LAUNCHER_STYLES:'launcher.css',CLIPBOARD_STYLES:'clipboard.css',ICONS:'icons.js',DATA:'data.js',MODEL:'model.js',LAUNCHER:'launcher.js',APP:'app.js'};
for(const [key,file] of Object.entries(inputs)){
  const content=await readFile(new URL('src/'+file,root),'utf8');
  html=html.replace('/*__'+key+'__*/',()=>content);
}
const assets={};
for(const [id,file,mime] of [['hopy','hopy.ico','image/x-icon'],['hua','hua.svg','image/svg+xml'],['blacklock','blacklock.ico','image/x-icon'],['reference','launcher-reference.png','image/png']]){
  assets[id]='data:'+mime+';base64,'+(await readFile(new URL('assets/'+file,root))).toString('base64');
}
html=html.replace('/*__LAUNCHER_ASSETS__*/',()=> 'window.ToolsLauncherAssets='+JSON.stringify(assets)+';');
if(/\/\*__[A-Z_]+__\*\//.test(html))throw new Error('Unresolved template marker');
await writeFile(new URL('index.html',root),html,'utf8');
console.log('Built self-contained prototype: '+new URL('index.html',root).pathname);
