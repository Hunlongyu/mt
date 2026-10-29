(() => {
  'use strict';
  const $=selector=>document.querySelector(selector);
  const $$=selector=>Array.from(document.querySelectorAll(selector));
  const {icon,hydrate}=ToolsIcons;
  const {copy}=ToolsModel;
  const esc=value=>String(value??'').replace(/[&<>"']/g,ch=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[ch]));
  let storage;
  try { storage=window.localStorage; } catch { storage={getItem:()=>null,setItem:()=>{throw new Error('Unavailable');}}; }
  const store=ToolsModel.createStore(storage);
  const ui={view:'launcher',filter:'all',clipQuery:'',clipSearchOpen:false,clipSearchReturn:null,selectedClip:'clip-code',hidden:new Set(),section:'general',draft:null,password:'',editing:null,itemContext:null,preview:null,menuReturn:null};
  let launcher;
  let toastTimer,confirmAction;
  const state=()=>store.get();
  const appById=id=>state().apps.find(x=>x.id===id);
  const clipById=id=>state().records.find(x=>x.id===id);
  function toast(message) {
    $('#toast').textContent=message;$('#toast').hidden=false;
    clearTimeout(toastTimer);toastTimer=setTimeout(()=>{$('#toast').hidden=true;},2800);
  }
  function dispatch(action) {
    const result=store.dispatch(action);
    if(!result.ok&&result.error)toast(result.error);
    if(result.persisted===false)toast('当前浏览器无法保存，修改仅在本次预览中保留。');
    return result;
  }
  function applyTheme(theme) {
    document.documentElement.dataset.theme=theme;
    $$('[data-theme-choice]').forEach(b=>b.setAttribute('aria-pressed',String(b.dataset.themeChoice===theme)));
  }
  function renderVisibility() {
    $('#workspace').dataset.view=ui.view;
    for(const id of ['launcher','clipboard'])$('#'+id+'-panel').hidden=ui.hidden.has(id)||(ui.view!=='overview'&&ui.view!==id);
    $('#empty-stage').hidden=!['launcher','clipboard'].every(id=>$('#'+id+'-panel').hidden);
    $$('.view-switch [data-view]').forEach(b=>b.setAttribute('aria-pressed',String(b.dataset.view===ui.view)));
  }
  function setView(view) {
    closeMenu();ui.view=view;ui.hidden.clear();renderVisibility();
    if(view==='launcher')launcher.focus();
    if(view==='clipboard')ui.clipSearchOpen?$('#clipboard-search').focus():focusClipSelection();
  }
  function renderLauncher() { launcher.render(); }
  const typeIcons={plain:'text',code:'text',rich:'text',link:'text',image:'image',files:'document',secret:'text'};
  const clipFilterLabels={all:'全部',text:'文本',image:'图片',files:'文件',favorite:'收藏'};
  function recordBody(r) {
    if(r.sensitive&&state().preferences.clipboard.sensitiveMask)return '<p class="clip-excerpt">••••••••••••　内容已隐藏</p>';
    if(r.type==='image')return `<div class="clip-media"><img class="clip-art" src="${esc(r.image)}" alt="几何构成的绿色拱门和暖色圆日"><div class="clip-media-copy"><strong>${esc(r.title)}</strong><span>${esc(r.meta||'图片')}</span></div></div>`;
    if(r.type==='files')return `<div class="clip-media"><span class="clip-file-icon">${icon('folder')}</span><div class="clip-media-copy"><strong>${esc(r.text.split('\n')[0])}</strong><span>${esc(r.meta||r.text.split('\n').length+' 个文件')}</span></div></div>`;
    return `<p class="clip-excerpt ${r.format==='code'?'clip-code':''}">${esc(r.text)}</p>`;
  }
  function renderClipboardToolbar() {
    $('#clipboard-browse-bar').hidden=ui.clipSearchOpen;
    $('#clipboard-search-wrap').hidden=!ui.clipSearchOpen;
    $('#clipboard-search-toggle').setAttribute('aria-expanded',String(ui.clipSearchOpen));
    $$('[data-clip-filter]').forEach(button=>{
      const selected=button.dataset.clipFilter===ui.filter;
      button.setAttribute('aria-pressed',String(selected));
      button.title=clipFilterLabels[button.dataset.clipFilter]+(selected?'（再次点击取消过滤）':'');
    });
    const scope=clipFilterLabels[ui.filter];
    $('#clipboard-search').placeholder='搜索'+scope+'记录';
    $('#clipboard-search').setAttribute('aria-label','搜索剪贴板记录（'+scope+'）');
    $('#clipboard-search').title=ui.filter==='all'?'搜索全部记录':'仅搜索'+scope+'记录；收起搜索可调整过滤';
  }
  function renderClipboard() {
    const s=state(),records=store.records({filter:ui.filter,query:ui.clipQuery});
    if(!records.some(x=>x.id===ui.selectedClip))ui.selectedClip=records[0]?.id||null;
    renderClipboardToolbar();
    $('#clipboard-results').textContent=clipFilterLabels[ui.filter]+'，'+records.length+' 条记录';
    $('#capture-status').hidden=!s.paused&&s.preferences.clipboard.enabled;
    $('#capture-status').innerHTML=s.preferences.clipboard.enabled?`${icon('pause')}已暂停记录<button data-action="pause-clipboard">继续</button>`:`${icon('info')}剪贴板功能已关闭<button data-action="settings" data-section="clipboard">设置</button>`;
    $('#clip-list').innerHTML=records.map((r,index)=>{
      return `<article class="clip-card ${r.id===ui.selectedClip?'selected':''}" tabindex="0" data-clip-id="${esc(r.id)}" aria-label="${esc(r.title)}" aria-current="${r.id===ui.selectedClip}">
        ${recordBody(r)}
        <div class="clip-quick-actions">
          <button class="icon-button ${r.favorite?'active':''}" data-clip-action="favorite" data-id="${esc(r.id)}" aria-label="${r.favorite?'取消收藏':'收藏'} ${esc(r.title)}" aria-pressed="${r.favorite}" title="${r.favorite?'取消收藏':'收藏'}">${icon('star')}</button>
          <button class="icon-button ${r.pinned?'active':''}" data-clip-action="pin" data-id="${esc(r.id)}" aria-label="${r.pinned?'取消置顶':'置顶'} ${esc(r.title)}" aria-pressed="${r.pinned}" title="${r.pinned?'取消置顶':'置顶'}">${icon('pin')}</button>
          <button class="icon-button" data-clip-action="remove" data-id="${esc(r.id)}" aria-label="删除 ${esc(r.title)}" title="删除">${icon('trash')}</button>
        </div>
        <div class="clip-card-meta"><span class="clip-number" aria-label="序号 ${index+1}">#${index+1}</span><span class="clip-time">${esc(r.time)}</span><span class="clip-type" title="${r.type==='image'?'图片':r.type==='files'?'文件':'文本'}">${icon(typeIcons[r.format]||'text')}</span></div></article>`;
    }).join('')||`<div class="empty-state">${icon(ui.filter==='favorite'?'star':'search')}<strong>${ui.clipQuery?'没有找到匹配记录':ui.filter==='favorite'?'还没有收藏':'这里还没有记录'}</strong><p>${ui.clipQuery?'换个关键词，或试试其他类型。':'收藏或置顶的内容会一直保留。'}</p>${ui.clipQuery?'<button class="text-button" data-action="clear-clip-search">清除搜索</button>':''}</div>`;
  }
  function render() {applyTheme(state().preferences.general.theme);renderLauncher();renderClipboard();renderVisibility();}
  function selectClip(id,focus=false) {
    ui.selectedClip=id;
    $$('#clip-list .clip-card').forEach(card=>{
      const selected=card.dataset.clipId===id;card.classList.toggle('selected',selected);card.setAttribute('aria-current',String(selected));
      if(selected&&focus){card.focus({preventScroll:true});card.scrollIntoView({block:'nearest'});}
    });
  }
  function focusClipSelection() {
    if(ui.selectedClip)selectClip(ui.selectedClip,true);else $('#clipboard-panel').focus();
  }
  function showClipSearch() {
    if(!ui.clipSearchOpen)ui.clipSearchReturn={id:ui.selectedClip,scroll:$('#clip-list').scrollTop};
    ui.clipSearchOpen=true;renderClipboardToolbar();animateClipToolbar(true);$('#clipboard-search').focus();$('#clipboard-search').select();
  }
  function hideClipSearch(restoreFocus=true) {
    if(!ui.clipSearchOpen)return;
    const previous=ui.clipSearchReturn;ui.clipSearchOpen=false;ui.clipSearchReturn=null;ui.clipQuery='';
    $('#clipboard-search').value='';if(previous)ui.selectedClip=previous.id;
    renderClipboard();animateClipToolbar(false);if(previous)$('#clip-list').scrollTop=previous.scroll;
    if(restoreFocus)focusClipSelection();
  }
  function animateClipToolbar(opening) {
    const bar=$(opening?'#clipboard-search-wrap':'#clipboard-browse-bar');
    bar.getAnimations().forEach(animation=>animation.cancel());
    if(window.matchMedia('(prefers-reduced-motion: reduce)').matches)return;
    bar.animate([{clipPath:opening?'inset(0 0 0 100%)':'inset(0 100% 0 0)'},{clipPath:'inset(0)'}],{duration:180,easing:'cubic-bezier(.22,1,.36,1)'});
  }
  function hideClipboard() {
    hideClipSearch(false);ui.hidden.add('clipboard');renderVisibility();$('.view-switch [data-view="clipboard"]').focus();
  }
  function pasteClip(id) {
    const r=clipById(id);if(!r)return;
    if(!state().preferences.clipboard.enabled){toast('剪贴板功能已关闭，可在设置中开启。');return;}
    $('#paste-target').value=r.text.replaceAll('\n','  ');
    toast(r.type==='text'?'已回贴到试验区':r.type==='image'?'已将图片名称放入试验区':'已将文件名放入试验区');
    if($('#preview-dialog').open)$('#preview-dialog').close();
    if(ui.view!=='overview'&&state().preferences.clipboard.hideAfterPaste)hideClipboard();
  }
  async function copyClip(id) {
    const r=clipById(id);if(!r)return;
    try {
      await navigator.clipboard.writeText(r.text);
      toast(r.type==='text'?'已复制文本':r.type==='image'?'已复制图片名称':'已复制文件名');
    } catch {
      $('#paste-target').value=r.text;toast('浏览器未允许复制，内容已放入试验区，可手动复制。');
    }
  }
  function previewClip(id) {
    const r=clipById(id);if(!r)return;
    ui.preview=id;$('#preview-title').textContent=r.title;$('#preview-type-icon').innerHTML=icon(typeIcons[r.format]||'text');
    $('#preview-meta').textContent=r.meta||r.time;
    $('#preview-content').innerHTML=r.sensitive&&state().preferences.clipboard.sensitiveMask?'<div class="secret-preview">••••••••</div>':r.type==='image'?`<img src="${esc(r.image)}" alt="几何构成的绿色拱门和暖色圆日">`:`<pre>${esc(r.text)}</pre>`;
    $('[data-action="preview-copy"]').textContent=r.type==='text'?'复制':'复制名称';
    $('[data-action="preview-paste"]').textContent=r.type==='text'?'回贴':'回贴名称';
    $('#preview-dialog').showModal();
  }
  function confirm(title,description,label,action) {
    closeMenu();$('#confirm-title').textContent=title;$('#confirm-description').textContent=description;
    $('#confirm-accept').textContent=label;confirmAction=action;$('#confirm-dialog').showModal();
  }
  function openItem(id=null,context=launcher.selection()) {
    closeMenu();ui.editing=id;ui.itemContext=context;const app=appById(id);
    $('#item-dialog-title').textContent=app?'编辑项目':'添加项目';$('#item-error').textContent='';
    $('#item-name').value=app?.name||'';$('#item-target').value=app?.target||'';
    $('#item-category').innerHTML=ToolsData.categories.filter(x=>x.id!=='all').map(c=>`<option value="${c.id}">${c.label}</option>`).join('');
    $('#item-category').value=app?.category||context.category;
    $('#item-type').value=app?.type||'app';$('#item-dialog').showModal();$('#item-name').focus();
  }
  function menuItem(action,label,iconName,id,danger=false) {return `<button role="menuitem" class="${danger?'danger':''}" data-menu-action="${action}" data-id="${esc(id)}">${icon(iconName)}${label}</button>`;}
  function closeMenu(restore=false) {
    $('#context-menu').hidden=true;
    $('#clipboard-menu-toggle').setAttribute('aria-expanded','false');
    if(restore&&ui.menuReturn?.isConnected)ui.menuReturn.focus();
  }
  function showMenu(kind,id,x,y,trigger) {
    const menu=$('#context-menu');ui.menuReturn=trigger;
      const r=clipById(id);if(!r)return;
      selectClip(id);
      menu.innerHTML=menuItem('paste',r.type==='text'?'回贴':'回贴名称','arrow-bend',id)+menuItem('copy',r.type==='text'?'复制':'复制名称','copy',id)+menuItem('preview','预览内容','eye',id)+'<hr>'+menuItem('favorite',r.favorite?'取消收藏':'收藏','star',id)+menuItem('pin',r.pinned?'取消置顶':'置顶','pin',id)+'<hr>'+menuItem('remove-clip','删除记录','trash',id,true);
    menu.hidden=false;
    menu.style.left=Math.max(8,Math.min(x,innerWidth-menu.offsetWidth-8))+'px';
    menu.style.top=Math.max(8,Math.min(y,innerHeight-menu.offsetHeight-8))+'px';
    menu.querySelector('button').focus();
  }
  function showClipboardMenu(trigger) {
    const menu=$('#context-menu'),rect=trigger.getBoundingClientRect();ui.menuReturn=trigger;
    const paused=state().paused;
    menu.innerHTML=`<button role="menuitem" data-action="pause-clipboard">${icon(paused?'play':'pause')}${paused?'继续记录':'暂停记录'}</button><button role="menuitem" data-action="settings" data-section="clipboard">${icon('settings')}剪贴板设置</button><hr><button role="menuitem" class="danger" data-action="clear-history">${icon('trash')}清空普通历史</button>`;
    menu.hidden=false;trigger.setAttribute('aria-expanded','true');
    menu.style.left=Math.max(8,Math.min(rect.right-menu.offsetWidth,innerWidth-menu.offsetWidth-8))+'px';
    menu.style.top=Math.max(8,Math.min(rect.bottom+4,innerHeight-menu.offsetHeight-8))+'px';
    menu.querySelector('button').focus();
  }
  // Settings are a draft transaction; cancel restores the persisted theme and data.
  const settingId=key=>'pref-'+key.replace('.','-');
  function row(title,description,control,extra='') {return `<div class="setting-row ${extra}"><div class="setting-copy"><strong>${title}</strong>${description?'<p>'+description+'</p>':''}</div>${control}</div>`;}
  function switchRow(section,key,title,description='') {
    const path=section+'.'+key;
    return row(title,description,`<label class="switch"><input type="checkbox" data-pref="${path}" aria-label="${title}" ${ui.draft[section][key]?'checked':''}><span class="switch-track"></span></label>`);
  }
  function inputRow(section,key,title,description='',type='text',extra='') {
    const path=section+'.'+key;
    return row(title,description,`<input class="setting-input ${key==='shortcut'?'shortcut-input':type==='number'?'number':''}" type="${type}" id="${settingId(path)}" data-pref="${path}" aria-label="${title}" value="${esc(ui.draft[section][key])}" ${extra}>`);
  }
  function selectRow(section,key,title,description,choices) {
    return row(title,description,`<select class="setting-select" data-pref="${section+'.'+key}" aria-label="${title}">${choices.map(([id,label])=>`<option value="${id}" ${ui.draft[section][key]===id?'selected':''}>${label}</option>`).join('')}</select>`);
  }
  function section(title,body) {return `<section class="settings-section"><h4>${title}</h4><div class="setting-group">${body}</div></section>`;}
  function tip(message) {return `<p class="settings-tip">${icon('info')}<span>${message}</span></p>`;}
  function renderSettings() {
    const meta=ToolsData.sections.find(s=>s.id===ui.section),p=ui.draft;
    $('#settings-navigation').innerHTML='<div class="settings-nav-label">偏好设置</div>'+ToolsData.sections.map(s=>`<button data-settings-section="${s.id}" aria-current="${s.id===ui.section?'page':'false'}">${icon(s.icon)}${s.label}</button>`).join('');
    let body='';
    if(ui.section==='general'){
      body=section('启动与运行',switchRow('general','autostart','开机启动','登录 Windows 后自动运行。')+switchRow('general','startHidden','启动时仅显示托盘图标','需要时再唤出面板。'));
      body+=section('外观',row('界面主题','参考 VS Code 的统一灰阶，所有面板保持一致。',`<div class="theme-options"><button class="theme-option" data-draft-theme="light" aria-pressed="${p.general.theme==='light'}">${icon('sun')}浅色</button><button class="theme-option" data-draft-theme="dark" aria-pressed="${p.general.theme==='dark'}">${icon('moon')}深色</button></div>`,'theme-row'));
      body+=tip('所有功能共用一个托盘图标。关闭面板后，已启用的功能继续在后台工作。');
    }else if(ui.section==='launcher'){
      body=section('唤出面板',switchRow('launcher','enabled','启用启动面板')+inputRow('launcher','shortcut','全局快捷键','按组合键快速打开面板。','text','maxlength="30"')+switchRow('launcher','edge','屏幕边缘唤出','鼠标停留在屏幕边缘时显示。')+selectRow('launcher','edgeSide','唤出边缘','选择最顺手的一侧。',[['left','左侧'],['right','右侧'],['top','顶部'],['bottom','底部']]));
      body+=section('打开行为',switchRow('launcher','keepOpen','保持面板打开','固定面板，方便连续打开多个项目。')+switchRow('launcher','hideAfterOpen','打开项目后收起','“保持面板打开”开启时不会收起。'));
    }else if(ui.section==='clipboard'){
      body=section('唤出与回贴',switchRow('clipboard','enabled','启用剪贴板')+inputRow('clipboard','shortcut','全局快捷键','','text','maxlength="30"')+switchRow('clipboard','nearCaret','在输入位置附近显示')+switchRow('clipboard','hideAfterPaste','回贴后收起面板'));
      body+=section('记录与隐私',inputRow('clipboard','history','普通历史记录上限','收藏与置顶不计入上限。','number','min="20" max="5000" step="1"')+switchRow('clipboard','sensitiveMask','隐藏敏感内容','列表和预览中以圆点遮住内容。'));
      body+=tip('清空普通历史时会保留收藏和置顶。原型中的敏感内容为虚构示例。');
    }else if(ui.section==='gestures'){
      body=section('手势操作',switchRow('gestures','enabled','启用鼠标手势')+selectRow('gestures','trigger','触发按键','按住此键划动，再松开执行。',[['right','鼠标右键'],['middle','鼠标中键'],['x1','侧键 1'],['x2','侧键 2']])+switchRow('gestures','showTrail','显示手势轨迹')+switchRow('gestures','ignoreFullscreen','全屏应用中暂停','减少游戏或演示时的误触。'));
      const directions=['←','→','↑','↓','↓ →','↓ ←','↑ →','↑ ←'];
      body+=section('手势映射',`<table class="gesture-table"><thead><tr><th>方向</th><th>执行操作</th><th><span class="sr-only">管理</span></th></tr></thead><tbody>${p.gestures.rules.map((rule,index)=>`<tr><td><select data-rule="${index}" data-rule-field="direction" aria-label="手势 ${index+1} 方向">${directions.map(d=>`<option ${d===rule.direction?'selected':''}>${d}</option>`).join('')}</select></td><td><select data-rule="${index}" data-rule-field="action" aria-label="手势 ${index+1} 操作">${ToolsData.gestureActions.map(a=>`<option value="${a.id}" ${a.id===rule.action?'selected':''}>${a.label}</option>`).join('')}</select></td><td><button class="icon-button" data-remove-rule="${index}" aria-label="删除手势 ${index+1}">${icon('trash')}</button></td></tr>`).join('')}</tbody></table><button class="add-rule" data-action="add-rule">${icon('plus')}添加手势</button>`);
      body+=section('排除应用',row('暂停使用手势的应用','填写程序名，多个程序用分号分隔。',`<input class="setting-input" data-pref="gestures.excluded" aria-label="排除应用" value="${esc(p.gestures.excluded)}" placeholder="Game.exe; Presentation.exe" maxlength="300">`,'stack'));
    }else if(ui.section==='lock'){
      body=section('启用与唤出',switchRow('lock','enabled','启用黑屏锁定')+inputRow('lock','shortcut','全局快捷键','','text','maxlength="30"')+switchRow('lock','allScreens','覆盖所有屏幕','多个显示器同时显示黑色遮挡。'));
      body+=section('解锁与电源',switchRow('lock','passwordMode','使用密码解锁','关闭时，按 Esc 退出遮挡。')+(p.lock.passwordMode?row('解锁密码','原型仅校验输入，不保存密码。',`<input class="setting-input" type="password" data-password aria-label="解锁密码" autocomplete="new-password" placeholder="字母或数字" maxlength="32" value="${esc(ui.password)}">`):'')+switchRow('lock','preventSleep','遮挡期间阻止自动休眠','让正在进行的任务保持运行。'));
      body+=tip('黑屏遮挡不等同于 Windows 安全锁定。需要保护登录会话时，请使用 Win + L。');
    }
    $('#settings-content').innerHTML=`<header class="settings-page-heading"><h3>${meta.label}</h3><p>${meta.description}</p></header>`+body;
  }
  function openSettings(sectionId='general') {
    ui.section=sectionId;ui.draft=copy(state().preferences);ui.password='';
    $('#settings-error').textContent='';renderSettings();$('#settings-content').scrollTop=0;$('#settings-dialog').showModal();
  }
  function cancelSettings() {$('#settings-dialog').close();ui.draft=null;ui.password='';applyTheme(state().preferences.general.theme);}
  function saveSettings() {
    let error=ToolsModel.validatePreferences(ui.draft);
    const directions=ui.draft.gestures.rules.map(r=>r.direction);
    if(!error&&new Set(directions).size!==directions.length)error='手势方向重复，请为每个方向保留一条映射。';
    if(!error&&ui.draft.lock.passwordMode&&!state().preferences.lock.passwordMode&&!ui.password)error='启用密码模式前，请填写一个示例密码。';
    if(!error&&ui.draft.lock.passwordMode&&ui.password&&!/^[A-Za-z0-9]{1,32}$/.test(ui.password))error='解锁密码仅支持 1–32 位字母或数字。';
    if(error){$('#settings-error').textContent=error;return;}
    const result=dispatch({type:'preferences.save',preferences:ui.draft});
    if(result.ok){$('#settings-dialog').close();ui.draft=null;ui.password='';render();toast('设置已保存到当前原型');}
  }
  function handleAction(action,button) {
    switch(action){
      case 'help':$('#help-dialog').showModal();break;
      case 'settings':openSettings(button.dataset.section);break;
      case 'cancel-settings':cancelSettings();break;
      case 'save-settings':saveSettings();break;
      case 'hide-launcher':ui.hidden.add('launcher');renderVisibility();break;
      case 'hide-clipboard':hideClipboard();break;
      case 'show-clip-search':showClipSearch();break;
      case 'hide-clip-search':hideClipSearch();break;
      case 'clipboard-menu':showClipboardMenu(button);break;
      case 'pause-clipboard':dispatch({type:'capture.toggle'});renderClipboard();focusClipSelection();break;
      case 'clear-paste':$('#paste-target').value='';$('#paste-target').focus();break;
      case 'clear-clip-search':ui.clipQuery='';$('#clipboard-search').value='';renderClipboard();$('#clipboard-search').focus();break;
      case 'add-app':openItem();break;
      case 'preview-copy':copyClip(ui.preview);break;
      case 'preview-paste':pasteClip(ui.preview);break;
      case 'clear-history':confirm('清空普通历史？','收藏和置顶的记录会保留，其他示例记录将被移除。','清空历史',()=>{dispatch({type:'clip.clear'});renderClipboard();toast('已清空普通历史，收藏与置顶已保留');});break;
      case 'reset-demo':confirm('恢复初始示例？','恢复示例项目、剪贴板记录和偏好设置。只影响当前原型。','恢复示例',()=>{dispatch({type:'demo.reset'});launcher.reset();ui.filter='all';ui.clipQuery='';ui.clipSearchOpen=false;ui.clipSearchReturn=null;$('#clipboard-search').value='';$('#paste-target').value='';setView('launcher');render();toast('已恢复初始示例');});break;
      case 'add-rule':{
        const direction=['←','→','↑','↓','↓ →','↓ ←','↑ →','↑ ←'].find(d=>!ui.draft.gestures.rules.some(r=>r.direction===d));
        if(!direction){$('#settings-error').textContent='当前 8 种方向已全部配置。';break;}
        ui.draft.gestures.rules.push({direction,action:'refresh'});renderSettings();break;
      }
    }
  }
  document.addEventListener('click',event=>{
    const button=event.target.closest('button');
    if(!event.target.closest('#context-menu'))closeMenu();
    if(button){
      if(button.dataset.close){$('#'+button.dataset.close).close();return;}
      if(button.dataset.view){setView(button.dataset.view);return;}
      if(button.dataset.clipFilter){
        ui.filter=ui.filter===button.dataset.clipFilter?'all':button.dataset.clipFilter;
        renderClipboard();$('#clip-list').scrollTop=0;return;
      }
      if(button.dataset.themeChoice){dispatch({type:'theme.set',theme:button.dataset.themeChoice});applyTheme(button.dataset.themeChoice);return;}
      if(button.dataset.action){if(!$('#context-menu').hidden)closeMenu(true);handleAction(button.dataset.action,button);return;}
      if(button.dataset.clipAction){
        const id=button.dataset.id,action=button.dataset.clipAction;
        if(action==='favorite'){
          dispatch({type:'clip.favorite',id});renderClipboard();
          const replacement=$$('[data-clip-action="favorite"]').find(b=>b.dataset.id===id);
          if(replacement)replacement.focus();else focusClipSelection();
        }
        if(action==='pin'){dispatch({type:'clip.pin',id});renderClipboard();focusClipSelection();}
        if(action==='remove')confirm('删除这条记录？','记录将从示例列表移除。','删除记录',()=>{dispatch({type:'clip.remove',id});renderClipboard();focusClipSelection();});
        if(action==='preview')previewClip(id);
        if(action==='menu'){const rect=button.getBoundingClientRect();showMenu('clip',id,rect.right,rect.bottom,button);}
        return;
      }
      if(button.dataset.menuAction){
        const action=button.dataset.menuAction,id=button.dataset.id;closeMenu();
        if(action==='preview')previewClip(id);
        if(action==='copy')copyClip(id);
        if(action==='paste')pasteClip(id);
        if(action==='pin'||action==='favorite'){dispatch({type:'clip.'+action,id});renderClipboard();focusClipSelection();}
        if(action==='remove-clip')confirm('删除这条记录？','记录将从示例列表移除。','删除记录',()=>{dispatch({type:'clip.remove',id});renderClipboard();focusClipSelection();});
        return;
      }
      if(button.dataset.settingsSection){ui.section=button.dataset.settingsSection;$('#settings-error').textContent='';renderSettings();$('#settings-content').scrollTop=0;$$('[data-settings-section]').find(b=>b.dataset.settingsSection===ui.section).focus();return;}
      if(button.dataset.draftTheme){ui.draft.general.theme=button.dataset.draftTheme;applyTheme(button.dataset.draftTheme);renderSettings();$$('[data-draft-theme]').find(b=>b.dataset.draftTheme===ui.draft.general.theme).focus();return;}
      if(button.dataset.removeRule!==undefined){ui.draft.gestures.rules.splice(Number(button.dataset.removeRule),1);renderSettings();return;}
    }
    const card=event.target.closest('[data-clip-id]');if(card)selectClip(card.dataset.clipId);
  });
  $('#confirm-accept').addEventListener('click',()=>{$('#confirm-dialog').close();const action=confirmAction;confirmAction=null;action?.();});
  $('#clipboard-search').addEventListener('input',event=>{ui.clipQuery=event.target.value;renderClipboard();});
  $('#settings-content').addEventListener('input',event=>{
    const input=event.target;
    if(input.matches('[data-password]')){ui.password=input.value;return;}
    if(!input.dataset.pref||!ui.draft)return;
    const [section,key]=input.dataset.pref.split('.');
    ui.draft[section][key]=input.type==='checkbox'?input.checked:input.type==='number'?Number(input.value):input.value;
    $('#settings-error').textContent='';
  });
  $('#settings-content').addEventListener('change',event=>{
    const input=event.target;
    if(input.dataset.rule!==undefined){ui.draft.gestures.rules[Number(input.dataset.rule)][input.dataset.ruleField]=input.value;$('#settings-error').textContent='';}
    if(input.dataset.pref==='lock.passwordMode')renderSettings();
  });
  $('#settings-dialog').addEventListener('cancel',event=>{event.preventDefault();cancelSettings();});
  $('#item-form').addEventListener('submit',event=>{
    event.preventDefault();const old=appById(ui.editing),type=$('#item-type').value;
    const item={name:$('#item-name').value.trim(),target:$('#item-target').value.trim(),category:$('#item-category').value,type,description:type==='folder'?'文件夹':type==='link'?'网址':'应用',icon:old?.icon||(type==='folder'?'folder':type==='link'?'link':'grid'),color:old?.color||'#447b65',tint:old?.tint||'#e8f3ec'};
    const slot=ui.itemContext?.category===item.category?ui.itemContext?.slot:undefined;
    const result=store.dispatch({type:'app.save',id:ui.editing,item,slot});
    if(!result.ok){$('#item-error').textContent=result.error;return;}
    $('#item-dialog').close();launcher.itemSaved(result.id);
    toast(result.persisted===false?'项目已添加，本次预览结束后不会保留。':ui.editing?'项目已更新':'项目已添加');
  });
  document.addEventListener('contextmenu',event=>{
    const clip=event.target.closest('[data-clip-id]');
    if(!clip)return;event.preventDefault();
    showMenu('clip',clip.dataset.clipId,event.clientX,event.clientY,clip);
  });
  $('#clip-list').addEventListener('dblclick',event=>{
    if(event.target.closest('button'))return;
    const card=event.target.closest('[data-clip-id]');if(card)pasteClip(card.dataset.clipId);
  });
  document.addEventListener('keydown',event=>{
    if($('#settings-dialog').open||$$('dialog[open]').length)return;
    const target=event.target,isInput=target.matches('input,textarea,select');
    if(!$('#context-menu').hidden){
      const buttons=Array.from($('#context-menu').querySelectorAll('button')),index=buttons.indexOf(document.activeElement);
      if(event.key==='Escape'){event.preventDefault();closeMenu(true);}
      if(event.key==='ArrowDown'||event.key==='ArrowUp'){event.preventDefault();buttons[(index+(event.key==='ArrowDown'?1:-1)+buttons.length)%buttons.length].focus();}
      return;
    }
    const inClipboard=!!target.closest('#clipboard-panel');
    if((event.key==='/'&&!isInput)||(event.ctrlKey&&!event.altKey&&event.key.toLowerCase()==='f'&&(inClipboard||ui.view==='clipboard'))){
      event.preventDefault();ui.hidden.delete('clipboard');setView(ui.view==='launcher'?'clipboard':ui.view);showClipSearch();return;
    }
    if(event.key==='Escape'&&inClipboard&&ui.clipSearchOpen){event.preventDefault();hideClipSearch();return;}
    if(event.key==='Enter'&&target.id==='clipboard-search'){event.preventDefault();pasteClip(ui.selectedClip);return;}
    if(target.id==='clipboard-search'&&(event.key==='ArrowUp'||event.key==='ArrowDown')){
      event.preventDefault();const records=store.records({filter:ui.filter,query:ui.clipQuery}),index=records.findIndex(r=>r.id===ui.selectedClip);
      const record=records[Math.max(0,Math.min(records.length-1,index+(event.key==='ArrowDown'?1:-1)))];
      if(record)selectClip(record.id);return;
    }
    if(event.key==='Escape'&&inClipboard&&!isInput){event.preventDefault();hideClipboard();return;}
    if(isInput)return;
    const card=target.closest('[data-clip-id]');
    if(card&&!target.closest('button')){
      if(event.key==='Enter'){event.preventDefault();pasteClip(card.dataset.clipId);}
      if(event.key===' '){event.preventDefault();previewClip(card.dataset.clipId);}
      if(event.key==='ArrowUp'||event.key==='ArrowDown'){
        event.preventDefault();const records=store.records({filter:ui.filter,query:ui.clipQuery}),index=records.findIndex(r=>r.id===card.dataset.clipId);
        selectClip(records[Math.max(0,Math.min(records.length-1,index+(event.key==='ArrowDown'?1:-1)))].id,true);
      }
    }
    if((event.shiftKey&&event.key==='F10')||event.key==='ContextMenu'){
      if(card){event.preventDefault();const rect=target.getBoundingClientRect();showMenu('clip',card.dataset.clipId,rect.left,rect.bottom,target);}
    }
  });
  window.addEventListener('resize',()=>closeMenu());
  window.addEventListener('scroll',()=>closeMenu(),true);
  launcher=ToolsLauncher.create({store,icon,esc,dispatch,toast,editItem:openItem,confirm,showSettings:openSettings,isOverview:()=>ui.view==='overview',hide:()=>{ui.hidden.add('launcher');renderVisibility();$('.view-switch [data-view="launcher"]').focus();}});
  hydrate();render();launcher.focus();
})();
