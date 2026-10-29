(() => {
  'use strict';
  const artwork='<svg xmlns="http://www.w3.org/2000/svg" width="720" height="360" viewBox="0 0 720 360"><rect width="720" height="360" fill="#e5e7df"/><path d="M0 310 720 100v260H0Z" fill="#c5d4cb"/><circle cx="540" cy="95" r="57" fill="#d7a783"/><path d="M70 360V188a115 115 0 0 1 230 0v172Z" fill="#427a70"/><path d="M128 360V193a57 57 0 0 1 114 0v167Z" fill="#e8eee5"/><rect x="380" y="195" width="190" height="165" rx="4" fill="#a4b9b2"/><path d="m345 285 106-88 108 88Z" fill="#ecebdf"/><text x="620" y="325" font-family="Arial,sans-serif" font-size="12" letter-spacing="3" fill="#45655b">STILL</text></svg>';
  const apps=[
    ['vscode','VS Code','development','code','#2878b5','#e6f2fd','代码编辑器','C:\\Apps\\Code.exe'],
    ['browser','浏览器','frequent','browser','#447b65','#e8f3ec','浏览器','https://example.com'],
    ['files','文件管理器','frequent','folder','#ac7a29','#fff2d6','文件夹','C:\\Users\\Demo\\Documents'],
    ['terminal','终端','development','terminal','#55606c','#e9edf2','命令行','wt.exe'],
    ['notes','笔记','frequent','note','#746a9b','#eeeaf7','笔记应用','C:\\Apps\\Notes.exe'],
    ['chat','聊天','frequent','chat','#438957','#e8f5e9','通讯应用','C:\\Apps\\Chat.exe'],
    ['design','设计工具','design','pen','#9d6468','#f9e9eb','设计应用','C:\\Apps\\Design.exe'],
    ['mail','邮件','frequent','mail','#487eac','#e9f2fa','邮件应用','C:\\Apps\\Mail.exe'],
    ['calendar','日历','frequent','calendar','#bc6b57','#ffede6','日程应用','C:\\Apps\\Calendar.exe'],
    ['music','音乐','frequent','music','#aa6789','#f9eaf3','音乐应用','C:\\Apps\\Music.exe'],
    ['colors','调色板','design','palette','#a37a32','#fcf0da','配色工具','C:\\Apps\\Palette.exe'],
    ['images','图片','design','image','#538f88','#e5f4f1','图片应用','C:\\Apps\\Photos.exe'],
    ['projects','项目目录','files','folder','#7d889e','#edf0f6','工作文件夹','E:\\Projects'],
    ['documents','工作文档','files','document','#5a83ac','#e9f2fc','文档目录','E:\\Documents'],
    ['downloads','下载','files','download','#7b8e65','#eef5e6','下载目录','C:\\Users\\Demo\\Downloads'],
    ['repository','代码仓库','development','code','#677380','#edf0f3','代码托管','https://example.com/repository'],
    ['readme','开发手册','development','document','#7273a5','#eeedfa','开发文档','https://example.com/guide']
  ].map(([id,name,category,icon,color,tint,description,target])=>({id,name,category:category==='development'?'development':category==='files'?'favorites':'office',icon,color,tint,description,target,type:category==='files'?'folder':target.startsWith('http')?'link':'app'}));
  const categorySlots={};
  apps.forEach(app=>{app.slot=(categorySlots[app.category]||0)+(app.category==='development'?28:0);categorySlots[app.category]=(categorySlots[app.category]||0)+1;});
  // Supplementary common-tool examples. All targets stay synthetic.
  const customApps=[
    ['extract','Extract','folder','#b58a44',14],['host','host','monitor','#398fe3',15],
    ['ico-low','ico-low','layers','#62a4e2',16],['ico-maker','ico-maker','layers','#62a4e2',17],
    ['qspin-ip','qspin_ip','arrow-bend','#187cca',18],['hopy','hopy','clipboard','#399d80',19,'hopy'],
    ['hua','hua','mouse','#07c160',20,'hua'],['qspin-log','qspin-log','text','#3779df',21],
    ['quota-tray','quota-tray','browser','#52a7a9',22],['pm-lite','pm-lite','calendar','#3d9edf',23],
    ['blacklock','BlackLock','monitor','#1b1b1b',24,'blacklock'],['eod','eod','link','#08a89c',25],
    ['portable','Portable','download','#849b9d',26],['work-summary','AI 工作总结','document','#6b9e5c',27]
  ].map(([id,name,icon,color,slot,asset])=>({id,name,category:'common',icon,color,tint:color,slot,asset:asset||null,description:'示例工具',target:'E:\\Demo\\'+id+'.exe',type:'app'}));
  apps.push(...customApps);
  // Names, icon crops and empty slots are transcribed from the user's supplied panel.
  const developmentNames=['Notepad3','Notepad4','Notepad--','skylark','Sublime',null,'Dadroit','BCompare',null,'dbx','XTerminal','OSS2','Wireshark','Reqable','studio',null,'RisohEditor','DBG_vpp','A_demo','apimonitor','scomm','sscom5.13.1','mbclient','xTools','procexp64','SystemInformer','TMX64','Procmon64'];
  apps.push(...developmentNames.flatMap((name,slot)=>name?[{id:'reference-dev-'+slot,name,category:'development',slot,referenceSlot:slot,icon:'code',color:'#398fe3',tint:'#398fe3',description:'截图中的示例项目',target:'E:\\Demo\\'+name+'.exe',type:'app'}]:[]));
  const records=[
    {id:'clip-code',type:'text',format:'code',title:'本地构建命令',text:'cmake --build --preset win-static-release',time:'刚刚',pinned:true,favorite:false},
    {id:'clip-note',type:'text',format:'plain',title:'让常用的工具，回到顺手的位置。',text:'让常用的工具，回到顺手的位置。\n更少的打扰，更专注的桌面。',time:'2 分钟前',pinned:false,favorite:true},
    {id:'clip-link',type:'text',format:'link',title:'界面设计参考',text:'https://example.com/design/desktop',time:'5 分钟前',pinned:false,favorite:false},
    {id:'clip-image',type:'image',format:'image',title:'留白与秩序.png',text:'留白与秩序.png',image:'data:image/svg+xml;charset=utf-8,'+encodeURIComponent(artwork),time:'8 分钟前',pinned:false,favorite:false,meta:'720 × 360 · 42 KB'},
    {id:'clip-files',type:'files',format:'files',title:'产品原型 · 3 个文件',text:'交互说明.pdf\n界面草图.fig\n图标资源.svg',time:'12 分钟前',pinned:false,favorite:false,meta:'3 个文件 · 2.4 MB'},
    {id:'clip-rich',type:'text',format:'rich',title:'下一次迭代',text:'下一次迭代\n· 统一两个面板的快捷操作\n· 补齐浅色与深色状态\n· 优化设置项的分组',time:'18 分钟前',pinned:false,favorite:false},
    {id:'clip-secret',type:'text',format:'secret',title:'敏感内容',text:'DEMO-ONLY-SECRET',time:'25 分钟前',sensitive:true,pinned:false,favorite:false},
    {id:'clip-email',type:'text',format:'plain',title:'联系邮箱',text:'hello@example.com',time:'32 分钟前',pinned:false,favorite:false}
  ];
  const preferences={
    general:{autostart:true,startHidden:true,theme:'light'},
    launcher:{enabled:true,shortcut:'Alt+Space',edge:false,edgeSide:'left',keepOpen:false,hideAfterOpen:true,columns:4,rows:7},
    clipboard:{enabled:true,shortcut:'Alt+C',history:100,keepFavorites:true,nearCaret:true,hideAfterPaste:true,sensitiveMask:true},
    gestures:{enabled:true,trigger:'right',showTrail:true,ignoreFullscreen:true,excluded:'GameViewer.exe',rules:[{direction:'←',action:'back'},{direction:'→',action:'forward'},{direction:'↓ →',action:'close'},{direction:'↑',action:'top'}]},
    lock:{enabled:true,shortcut:'Alt+L',passwordMode:false,preventSleep:true,allScreens:true}
  };
  window.ToolsData={
    apps,records,preferences,
    categories:[{id:'common',label:'常用',icon:'grid'},{id:'office',label:'办公',icon:'folder'},{id:'development',label:'开发',icon:'code'},{id:'favorites',label:'收藏',icon:'star'}],
    sections:[{id:'general',label:'常规',icon:'settings',description:'让工具按照你的习惯工作。'},{id:'launcher',label:'启动面板',icon:'grid',description:'常用应用、文件和网址，随时触手可及。'},{id:'clipboard',label:'剪贴板',icon:'clipboard',description:'留住复制过的内容，在需要时轻松找回。'},{id:'gestures',label:'鼠标手势',icon:'mouse',description:'按住鼠标，轻轻一划，完成常用操作。'},{id:'lock',label:'黑屏锁定',icon:'lock',description:'临时遮住屏幕，让后台任务继续运行。'}],
    gestureActions:[{id:'back',label:'后退'},{id:'forward',label:'前进'},{id:'close',label:'关闭窗口 / 标签页'},{id:'top',label:'滚动到顶部'},{id:'refresh',label:'刷新'},{id:'minimize',label:'最小化窗口'}]
  };
})();
