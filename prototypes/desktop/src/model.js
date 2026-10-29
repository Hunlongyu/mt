(() => {
  'use strict';
  const copy=value=>JSON.parse(JSON.stringify(value));
  const KEY='tools.desktop.prototype.v2';
  const LEGACY_KEY='tools.desktop.prototype.v1';
  const categoryIds=()=>ToolsData.categories.map(x=>x.id);
  const validSlot=value=>Number.isInteger(value)&&value>=0&&value<10000;
  function normalizeApps(entries) {
    const occupied=new Map();
    return entries.filter(x=>x&&typeof x.id==='string'&&typeof x.name==='string'&&typeof x.target==='string').map(input=>{
      const item=copy(input),legacy={frequent:'office',design:'office',files:'favorites',custom:'common'};
      item.category=legacy[item.category]||item.category;
      if(!categoryIds().includes(item.category))item.category='office';
      if(!occupied.has(item.category))occupied.set(item.category,new Set());
      const slots=occupied.get(item.category);let slot=validSlot(item.slot)?item.slot:0;
      while(slots.has(slot))slot++;item.slot=slot;slots.add(slot);return item;
    });
  }
  function launcherPage(apps,{category='development',page=0,query=''}={}) {
    const term=query.trim().toLocaleLowerCase();
    const matches=apps.filter(a=>term?[a.name,a.description,a.target].join(' ').toLocaleLowerCase().includes(term):a.category===category).sort((a,b)=>term?a.name.localeCompare(b.name):a.slot-b.slot);
    const count=term?matches.length:matches.reduce((max,a)=>Math.max(max,a.slot+1),0),capacity=28;
    const pages=Math.max(1,Math.ceil(count/capacity)),current=Math.max(0,Math.min(page,pages-1));
    const slots=Array.from({length:capacity},(_,i)=>{const slot=current*capacity+i;return {slot,item:copy((term?matches[slot]:matches.find(a=>a.slot===slot))||null)};});
    return {slots,page:current,pages,filtering:!!term,total:matches.length};
  }
  function normalizeShortcut(value) {
    const parts=String(value||'').split('+').map(x=>x.trim().toUpperCase()).filter(Boolean);
    const mods=['CTRL','ALT','SHIFT','WIN'];
    if(parts.length<2||!parts.slice(0,-1).every(x=>mods.includes(x))||new Set(parts).size!==parts.length) return null;
    const key=parts.at(-1);
    if(!/^(SPACE|[A-Z0-9]|F([1-9]|1[0-9]|2[0-4]))$/.test(key)) return null;
    return mods.filter(x=>parts.includes(x)).join('+')+'+'+key;
  }
  function validatePreferences(p) {
    if(!['light','dark'].includes(p.general.theme)) return '请选择浅色或深色主题。';
    const seen=new Set();
    for(const [id,label] of [['launcher','启动面板'],['clipboard','剪贴板'],['lock','黑屏锁定']]){
      const normalized=normalizeShortcut(p[id].shortcut);
      if(!normalized) return label+'快捷键格式不正确，例如 Alt+C。';
      if(p[id].enabled&&seen.has(normalized)) return '快捷键重复，请为每项功能设置不同组合。';
      if(p[id].enabled)seen.add(normalized);
    }
    if(!Number.isInteger(Number(p.clipboard.history))||p.clipboard.history<20||p.clipboard.history>5000) return '普通历史记录数量需为 20–5000 的整数。';
    if(!Array.isArray(p.gestures.rules)||!p.gestures.rules.every(rule=>rule&&['←','→','↑','↓','↓ →','↓ ←','↑ →','↑ ←'].includes(rule.direction)&&ToolsData.gestureActions.some(x=>x.id===rule.action)))return '手势映射格式不正确。';
    return null;
  }
  function createStore(storage) {
    const fresh=()=>({apps:copy(ToolsData.apps),records:copy(ToolsData.records),preferences:copy(ToolsData.preferences),recent:['vscode','files','notes'],paused:false});
    let state=fresh();
    try {
      const saved=JSON.parse(storage.getItem(KEY)||storage.getItem(LEGACY_KEY)||'null');
      if(saved&&[1,2].includes(saved.version)&&Array.isArray(saved.apps)&&Array.isArray(saved.records)){
        const preferences=copy(ToolsData.preferences);
        for(const section of Object.keys(preferences)){
          const input=saved.preferences?.[section];
          if(!input||typeof input!=='object')continue;
          for(const key of Object.keys(preferences[section])){
            if(typeof input[key]===typeof preferences[section][key])preferences[section][key]=copy(input[key]);
          }
        }
        if(!validatePreferences(preferences)){
          let apps=normalizeApps(saved.apps);
          if(saved.version===1){
            apps.filter(x=>x.category==='development').forEach(x=>x.slot+=28);
            apps.push(...copy(ToolsData.apps.filter(x=>(x.category==='common'||Number.isInteger(x.referenceSlot))&&!apps.some(a=>a.id===x.id))));
          }
          preferences.launcher.columns=4;preferences.launcher.rows=7;
          state={...state,apps,records:saved.records.filter(x=>x&&typeof x.id==='string'&&typeof x.text==='string'&&['text','image','files'].includes(x.type)),preferences,recent:Array.isArray(saved.recent)?saved.recent.filter(x=>typeof x==='string').slice(0,3):state.recent};
        }
      }
    } catch { /* Missing/blocked/corrupt local storage keeps the demo usable. */ }
    function persist() {
      try{storage.setItem(KEY,JSON.stringify({version:2,apps:state.apps,records:state.records,preferences:state.preferences,recent:state.recent}));return true;}catch{return false;}
    }
    function extent(category,excludeId) {return state.apps.filter(a=>a.category===category&&a.id!==excludeId).reduce((max,a)=>Math.max(max,a.slot+1),0);}
    function insert(item,category,slot) {
      let free=slot;while(state.apps.some(a=>a.id!==item.id&&a.category===category&&a.slot===free))free++;
      if(!validSlot(free))return false;
      state.apps.filter(a=>a.id!==item.id&&a.category===category&&a.slot>=slot&&a.slot<free).forEach(a=>a.slot++);
      item.category=category;item.slot=slot;return true;
    }
    return {
      get:()=>copy(state),
      launcherPage:options=>launcherPage(state.apps,options),
      apps:({category='all',query=''}={})=>state.apps.filter(x=>(category==='all'||x.category===category)&&[x.name,x.description,x.target].join(' ').toLocaleLowerCase().includes(query.toLocaleLowerCase().trim())).map(copy),
      records:({filter='all',query=''}={})=>state.records.filter(x=>(filter==='all'||(filter==='favorite'?x.favorite:x.type===filter))&&(!x.sensitive?[x.title,x.text].join(' '):[x.title].join(' ')).toLocaleLowerCase().includes(query.toLocaleLowerCase().trim())).sort((a,b)=>Number(b.pinned)-Number(a.pinned)).map(copy),
      dispatch(action) {
        let result={ok:true};
        const app=()=>state.apps.find(x=>x.id===action.id);
        const clip=()=>state.records.find(x=>x.id===action.id);
        switch(action.type){
          case 'app.save': {
            const item=action.item;
            if(!item.name.trim()||!item.target.trim())return {ok:false,error:'请填写名称和目标。'};
            if(/^(javascript|data|vbscript):/i.test(item.target.trim()))return {ok:false,error:'请使用应用路径、文件夹或 http(s) 网址。'};
            const existing=app();
            if(!categoryIds().includes(item.category))return {ok:false,error:'请选择有效分类。'};
            if(existing){
              if(existing.category!==item.category&&!insert(existing,item.category,extent(item.category,existing.id)))return {ok:false,error:'目标分类已满。'};
              const slot=existing.slot;Object.assign(existing,copy(item),{id:action.id,slot});
              result.id=existing.id;
            }else{
              const created={...copy(item),id:'app-'+Date.now().toString(36)+'-'+Math.random().toString(36).slice(2,6)};
              const slot=validSlot(action.slot)?action.slot:extent(item.category);
              if(!insert(created,item.category,slot))return {ok:false,error:'目标分类已满。'};
              state.apps.push(created);result.id=created.id;
            }
            break;
          }
          case 'app.remove': state.apps=state.apps.filter(x=>x.id!==action.id);state.recent=state.recent.filter(x=>x!==action.id);break;
          case 'app.place': {
            const moved=app();if(!moved)return {ok:false};
            const category=action.category||moved.category,slot=action.slot===undefined?extent(category,moved.id):action.slot;
            if(!categoryIds().includes(category)||!validSlot(slot))return {ok:false,error:'目标位置无效。'};
            if(category===moved.category){
              const from=moved.slot;
              if(state.apps.some(a=>a.id!==moved.id&&a.category===category&&a.slot===slot)){
                state.apps.filter(a=>a.id!==moved.id&&a.category===category).forEach(a=>{
                  if(from<slot&&a.slot>from&&a.slot<=slot)a.slot--;
                  else if(from>slot&&a.slot>=slot&&a.slot<from)a.slot++;
                });
              }
              moved.slot=slot;
            }else if(!insert(moved,category,slot))return {ok:false,error:'目标分类已满。'};
            break;
          }
          case 'app.insert-slot': {
            const entries=state.apps.filter(a=>a.category===action.category&&a.slot>=action.slot);
            if(!validSlot(action.slot)||entries.some(a=>a.slot>=9999))return {ok:false,error:'无法在此插入空槽。'};
            entries.forEach(a=>a.slot++);break;
          }
          case 'app.open': if(!app())return {ok:false};state.recent=[action.id,...state.recent.filter(x=>x!==action.id)].slice(0,3);break;
          case 'clip.favorite': if(clip())clip().favorite=!clip().favorite;break;
          case 'clip.pin': if(clip())clip().pinned=!clip().pinned;break;
          case 'clip.remove': state.records=state.records.filter(x=>x.id!==action.id);break;
          case 'clip.clear': state.records=state.records.filter(x=>x.favorite||x.pinned);break;
          case 'capture.toggle': state.paused=!state.paused;return result;
          case 'preferences.save': {
            const error=validatePreferences(action.preferences);
            if(error)return {ok:false,error};
            state.preferences=copy(action.preferences);break;
          }
          case 'theme.set': if(['light','dark'].includes(action.theme))state.preferences.general.theme=action.theme;break;
          case 'launcher.pin': state.preferences.launcher.keepOpen=!state.preferences.launcher.keepOpen;break;
          case 'demo.reset':state=fresh();break;
          default:return {ok:false,error:'未知操作'};
        }
        result.persisted=persist();
        return result;
      }
    };
  }
  window.ToolsModel={createStore,validatePreferences,normalizeShortcut,launcherPage,copy};
})();
