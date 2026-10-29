(() => {
  'use strict';
  function create({store,icon,esc,dispatch,toast,editItem,confirm,showSettings,hide,isOverview}) {
    const root=document.querySelector('#launcher-panel'),grid=root.querySelector('#app-grid'),tabs=root.querySelector('#launcher-categories');
    const popup=root.querySelector('#launcher-search-popup'),search=root.querySelector('#launcher-search'),title=root.querySelector('#launcher-title');
    const menu=document.querySelector('#launcher-menu'),status=root.querySelector('#launcher-page-status');
    const state={category:'development',page:0,query:'',searchVisible:false,selectedSlot:null,keyboard:false,dragId:null,menuContext:null,menuReturn:null};
    let hoverTimer,wheelAt=0;
    const categories=()=>ToolsData.categories;
    const page=()=>store.launcherPage(state);
    const appById=id=>store.get().apps.find(a=>a.id===id);
    const slotByIndex=slot=>page().slots.find(x=>x.slot===slot);
    root.style.setProperty('--reference-sprite','url("'+ToolsLauncherAssets.reference+'")');
    function appIcon(app) {
      if(Number.isInteger(app.referenceSlot)){
        const x=29+(app.referenceSlot%4)*76,y=33+Math.floor(app.referenceSlot/4)*66;
        return `<span class="launcher-app-icon reference-icon" style="--sprite-x:-${x}px;--sprite-y:-${y}px" aria-hidden="true"></span>`;
      }
      if(app.asset&&ToolsLauncherAssets[app.asset])return `<span class="launcher-app-icon asset-icon"><img src="${ToolsLauncherAssets[app.asset]}" alt="" draggable="false"></span>`;
      return `<span class="launcher-app-icon" style="--app-color:${esc(app.color||'#398fe3')}">${icon(app.icon||'grid')}</span>`;
    }
    function render() {
      const current=page();state.page=current.page;
      grid.innerHTML=current.slots.map(({slot,item})=>`<button class="launcher-slot ${item?'has-item':'empty-slot'} ${state.keyboard&&state.selectedSlot===slot?'keyboard-selected':''}" tabindex="-1" data-launch-slot="${slot}" ${item?'data-launch-id="'+esc(item.id)+'"':''} draggable="${!!item&&!current.filtering}" aria-label="${item?'打开 '+esc(item.name):'空槽 '+(slot+1)}">${item?appIcon(item)+'<span class="launcher-app-name"><span>'+esc(item.name)+'</span></span>':''}</button>`).join('');
      tabs.innerHTML=categories().map(c=>`<button data-launch-category="${c.id}" aria-pressed="${state.category===c.id}" aria-disabled="${current.filtering}" title="${c.label}">${esc(c.label)}</button>`).join('');
      const pinned=store.get().preferences.launcher.keepOpen,pin=root.querySelector('[data-launcher-action="pin"]');
      pin.setAttribute('aria-pressed',String(pinned));pin.setAttribute('aria-label',pinned?'取消保持打开':'保持打开');pin.title=pinned?'取消保持打开':'保持打开';
      popup.hidden=!state.searchVisible;search.value=state.query;
      status.textContent=(current.filtering?'搜索结果':categories().find(c=>c.id===state.category).label)+'，第 '+(current.page+1)+' / '+current.pages+' 页，'+current.total+' 个项目';
      title.textContent='MT';
    }
    function select(slot,focus=true) {
      state.selectedSlot=slot;state.keyboard=true;
      grid.querySelectorAll('[data-launch-slot]').forEach(button=>{
        const active=Number(button.dataset.launchSlot)===slot;
        button.classList.toggle('keyboard-selected',active);
        if(active&&focus)button.focus({preventScroll:true});
      });
    }
    function closeMenu(restore=false) {
      menu.hidden=true;
      if(restore&&state.menuReturn?.isConnected)state.menuReturn.focus({preventScroll:true});
    }
    function openApp(id) {
      const app=appById(id);if(!app)return;
      if(!store.get().preferences.launcher.enabled){toast('启动面板功能已关闭，可在设置中开启。');return;}
      dispatch({type:'app.open',id});toast('模拟打开：'+app.name);
      const p=store.get().preferences.launcher;
      if(!isOverview()&&p.hideAfterOpen&&!p.keepOpen){hide();clearSearch();}
    }
    function clearSearch() {state.query='';state.searchVisible=false;state.page=0;state.keyboard=false;state.selectedSlot=null;render();}
    function showSearch(initial) {
      state.searchVisible=true;
      if(initial!==undefined){state.query=initial;state.page=0;}
      const current=page(),first=current.slots.find(x=>x.item);
      state.selectedSlot=state.query&&first?first.slot:null;state.keyboard=!!state.query;
      render();search.focus();search.setSelectionRange?.(search.value.length,search.value.length);
    }
    function chooseCategory(category,focus=true) {
      if(!categories().some(c=>c.id===category))return;
      state.category=category;clearSearch();
      if(focus)root.focus({preventScroll:true});
    }
    function cycleCategory(direction) {
      if(state.query.trim())return;
      const list=categories(),index=list.findIndex(c=>c.id===state.category);
      chooseCategory(list[(index+direction+list.length)%list.length].id);
    }
    function flip(direction) {
      const current=page(),next=Math.max(0,Math.min(current.pages-1,current.page+direction));
      if(next===current.page)return;
      state.page=next;state.selectedSlot=null;state.keyboard=false;render();root.focus({preventScroll:true});
    }
    function itemSaved(id) {
      const app=appById(id);if(!app)return;
      state.category=app.category;state.query='';state.searchVisible=false;state.page=Math.floor(app.slot/28);
      state.selectedSlot=app.slot;state.keyboard=true;render();select(app.slot);
    }
    function edit(id=null,slot=state.selectedSlot) {
      closeMenu();const app=appById(id);
      editItem(id,{category:app?.category||state.category,slot:app?.slot??slot});
    }
    function remove(id) {
      const app=appById(id);if(!app)return;
      confirm('删除“'+app.name+'”？','只从启动面板移除，原位置会保留为空槽。','删除项目',()=>{dispatch({type:'app.remove',id});state.selectedSlot=null;render();root.focus();});
    }
    function menuEntry(command,label,key='',options={}) {
      return `<button role="menuitem" data-launch-command="${command}" ${options.category?'data-target-category="'+esc(options.category)+'"':''} ${options.disabled?'disabled':''} ${options.danger?'class="danger"':''}><span>${label}</span>${key?'<span class="menu-key">'+key+'</span>':''}</button>`;
    }
    function showMenu(kind,context,anchor,x,y) {
      closeMenu();state.menuContext={kind,...context};state.menuReturn=anchor;
      const current=page();
      if(kind==='item'){
        const app=appById(context.id);if(!app)return;
        menu.innerHTML=menuEntry('open','打开','Enter')+menuEntry('edit','属性','F2')+'<hr>'+
          menuEntry('previous-slot','向前移动', '',{disabled:current.filtering||app.slot===0})+
          menuEntry('next-slot','向后移动','',{disabled:current.filtering})+
          '<div class="menu-caption">移动到分类</div>'+categories().map(c=>menuEntry('move-category',esc(c.label),'',{category:c.id,disabled:c.id===app.category})).join('')+'<hr>'+menuEntry('delete','删除','Delete',{danger:true});
      }else if(kind==='slot'){
        menu.innerHTML=menuEntry('add','注册项目','Insert',{disabled:current.filtering})+menuEntry('insert-slot','插入空槽','',{disabled:current.filtering});
      }else{
        menu.innerHTML=menuEntry('search','搜索全部分类','Ctrl+F')+menuEntry('add','注册项目','Insert',{disabled:current.filtering})+'<hr>'+
          menuEntry('previous-page','上一页','PageUp',{disabled:current.page===0})+menuEntry('next-page','下一页','PageDown',{disabled:current.page===current.pages-1})+'<hr>'+
          menuEntry('pin',store.get().preferences.launcher.keepOpen?'取消保持打开':'保持打开','Ctrl+Space')+menuEntry('settings','设置')+menuEntry('hide','收起面板','Esc');
      }
      menu.hidden=false;
      const rect=anchor?.getBoundingClientRect();
      menu.style.left=Math.max(8,Math.min(x??rect?.left??0,innerWidth-menu.offsetWidth-8))+'px';
      menu.style.top=Math.max(8,Math.min(y??rect?.bottom??0,innerHeight-menu.offsetHeight-8))+'px';
      menu.querySelector('button:not(:disabled)')?.focus();
    }
    function move(id,category,slot) {
      const result=dispatch({type:'app.place',id,category,slot});
      if(result.ok){itemSaved(id);toast('已调整项目位置');}
    }
    menu.addEventListener('click',event=>{
      const button=event.target.closest('[data-launch-command]');if(!button)return;
      const {id,slot}=state.menuContext||{},command=button.dataset.launchCommand;closeMenu();
      const app=appById(id);
      if(command==='open')openApp(id);
      if(command==='edit')edit(id);
      if(command==='delete')remove(id);
      if(command==='add')edit(null,slot??state.selectedSlot);
      if(command==='insert-slot'){dispatch({type:'app.insert-slot',category:state.category,slot});render();}
      if(command==='previous-slot'&&app)move(id,app.category,app.slot-1);
      if(command==='next-slot'&&app)move(id,app.category,app.slot+1);
      if(command==='move-category'&&app)move(id,button.dataset.targetCategory,undefined);
      if(command==='search')showSearch();
      if(command==='previous-page')flip(-1);
      if(command==='next-page')flip(1);
      if(command==='pin'){dispatch({type:'launcher.pin'});render();root.focus();}
      if(command==='settings')showSettings('launcher');
      if(command==='hide'){hide();clearSearch();}
    });
    root.addEventListener('click',event=>{
      const category=event.target.closest('[data-launch-category]'),cell=event.target.closest('[data-launch-slot]'),control=event.target.closest('[data-launcher-action]');
      closeMenu();
      if(category){if(!page().filtering)chooseCategory(category.dataset.launchCategory);return;}
      if(cell){state.selectedSlot=Number(cell.dataset.launchSlot);state.keyboard=false;if(cell.dataset.launchId)openApp(cell.dataset.launchId);return;}
      if(control){
        if(control.dataset.launcherAction==='menu')showMenu('main',{},control);
        if(control.dataset.launcherAction==='pin'){dispatch({type:'launcher.pin'});render();root.querySelector('[data-launcher-action="pin"]').focus();}
        if(control.dataset.launcherAction==='hide'){hide();clearSearch();}
      }
    });
    root.addEventListener('contextmenu',event=>{
      event.preventDefault();
      const cell=event.target.closest('[data-launch-slot]');
      if(cell){state.selectedSlot=Number(cell.dataset.launchSlot);showMenu(cell.dataset.launchId?'item':'slot',{id:cell.dataset.launchId,slot:Number(cell.dataset.launchSlot)},cell,event.clientX,event.clientY);}
      else showMenu('main',{},root,event.clientX,event.clientY);
    });
    root.addEventListener('pointerover',event=>{
      clearTimeout(hoverTimer);const item=event.target.closest('[data-launch-id]');
      if(!item||state.dragId){title.textContent='MT';return;}
      const name=appById(item.dataset.launchId)?.name;
      if(name){item.title=name;hoverTimer=setTimeout(()=>{title.textContent=name;},150);}
    });
    root.addEventListener('pointerout',event=>{
      if(event.relatedTarget&&event.target.closest('[data-launch-id]')?.contains(event.relatedTarget))return;
      clearTimeout(hoverTimer);title.textContent='MT';
    });
    search.addEventListener('input',()=>{
      state.query=search.value;state.page=0;state.keyboard=!!state.query;
      state.selectedSlot=page().slots.find(x=>x.item)?.slot??null;render();
    });
    root.addEventListener('wheel',event=>{
      if(state.query.trim()||Math.abs(event.deltaY)<3)return;
      event.preventDefault();
      if(performance.now()-wheelAt<180)return;wheelAt=performance.now();
      cycleCategory(event.deltaY>0?1:-1);
    },{passive:false});
    function endDrag() {state.dragId=null;root.querySelectorAll('.dragging,.drop-target').forEach(x=>x.classList.remove('dragging','drop-target'));}
    grid.addEventListener('dragstart',event=>{
      const cell=event.target.closest('[data-launch-id]');
      if(!cell||page().filtering){event.preventDefault();return;}
      state.dragId=cell.dataset.launchId;event.dataTransfer.setData('text/plain',state.dragId);event.dataTransfer.effectAllowed='move';cell.classList.add('dragging');
    });
    root.addEventListener('dragover',event=>{
      const target=event.target.closest('[data-launch-slot],[data-launch-category]');
      if(!state.dragId||!target||page().filtering)return;event.preventDefault();event.dataTransfer.dropEffect='move';
      root.querySelectorAll('.drop-target').forEach(x=>x.classList.remove('drop-target'));target.classList.add('drop-target');
    });
    root.addEventListener('drop',event=>{
      const target=event.target.closest('[data-launch-slot],[data-launch-category]');
      if(!state.dragId||!target||page().filtering)return;
      event.preventDefault();const id=state.dragId;endDrag();
      move(id,target.dataset.launchCategory||state.category,target.dataset.launchSlot===undefined?undefined:Number(target.dataset.launchSlot));
    });
    root.addEventListener('dragend',endDrag);
    function navigate(key,fromSearch) {
      const current=page();if(!current.total&&current.filtering)return;
      const low=current.page*28,high=low+27;
      let selected=state.selectedSlot;
      if(selected===null)selected=current.slots.find(x=>x.item)?.slot??low;
      else if(key==='ArrowLeft'&&selected%4>0)selected--;
      else if(key==='ArrowRight'&&selected%4<3)selected++;
      else if(key==='ArrowUp')selected=Math.max(0,selected-4);
      else if(key==='ArrowDown')selected=Math.min(current.pages*28-1,selected+4);
      if(selected<low||selected>high){state.page=Math.floor(selected/28);render();}
      select(selected,!fromSearch);
    }
    document.addEventListener('keydown',event=>{
      if(document.querySelector('dialog[open]'))return;
      if(!menu.hidden){
        if(event.key==='Escape'){event.preventDefault();event.stopImmediatePropagation();closeMenu(true);}
        if(event.key==='ArrowDown'||event.key==='ArrowUp'){
          event.preventDefault();event.stopImmediatePropagation();
          const buttons=Array.from(menu.querySelectorAll('button:not(:disabled)')),index=buttons.indexOf(document.activeElement);
          buttons[(index+(event.key==='ArrowDown'?1:-1)+buttons.length)%buttons.length].focus();
        }
        return;
      }
      if(!root.contains(event.target)||root.hidden)return;
      const fromSearch=event.target===search,typing=event.target.matches('input,textarea,select');
      if(event.isComposing||event.key==='Process')return;
      const consume=()=>{event.preventDefault();event.stopImmediatePropagation();};
      if(event.ctrlKey&&['f','k'].includes(event.key.toLowerCase())){consume();showSearch();return;}
      if(event.ctrlKey&&event.code==='Space'){consume();dispatch({type:'launcher.pin'});render();root.focus();return;}
      if(event.key==='Escape'){
        consume();closeMenu();
        if(state.query){state.query='';state.page=0;state.keyboard=false;state.selectedSlot=null;render();search.focus();}
        else{hide();clearSearch();}
        return;
      }
      if(event.key==='Tab'&&!event.ctrlKey&&!event.altKey){consume();cycleCategory(event.shiftKey?-1:1);return;}
      if(['ArrowLeft','ArrowRight','ArrowUp','ArrowDown'].includes(event.key)){
        if(fromSearch&&(event.key==='ArrowLeft'||event.key==='ArrowRight'))return;
        consume();navigate(event.key,fromSearch);return;
      }
      if(event.key==='PageDown'||event.key==='PageUp'){consume();flip(event.key==='PageDown'?1:-1);return;}
      const item=slotByIndex(state.selectedSlot)?.item;
      if(event.key==='Enter'&&(fromSearch||event.target===root||event.target.matches('[data-launch-slot]'))){consume();if(item)openApp(item.id);return;}
      if(event.key==='Insert'&&!typing&&!page().filtering){consume();edit(null);return;}
      if(event.key==='F2'&&!typing&&item){consume();edit(item.id);return;}
      if(event.key==='Delete'&&!typing&&item){consume();remove(item.id);return;}
      if((event.shiftKey&&event.key==='F10')||event.key==='ContextMenu'){
        consume();const cell=event.target.closest('[data-launch-slot]');
        showMenu(cell?(cell.dataset.launchId?'item':'slot'):'main',{id:cell?.dataset.launchId,slot:cell?Number(cell.dataset.launchSlot):state.selectedSlot},cell||root);return;
      }
      if(!typing&&!event.ctrlKey&&!event.altKey&&!event.metaKey&&event.key.length===1&&event.key!==' '){consume();showSearch(event.key);}
    },true);
    document.addEventListener('pointerdown',event=>{if(!menu.contains(event.target)&&!event.target.closest('[data-launcher-action="menu"]'))closeMenu();});
    window.addEventListener('resize',()=>closeMenu());
    return {render,openApp,itemSaved,clearSearch,focus:()=>root.focus({preventScroll:true}),selection:()=>({category:state.category,slot:state.selectedSlot}),reset:()=>{state.category='development';clearSearch();},closeMenu};
  }
  window.ToolsLauncher={create};
})();
