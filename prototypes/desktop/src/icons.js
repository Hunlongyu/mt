(() => {
  'use strict';
  const paths = {
    grid:'<rect x="3" y="3" width="7" height="7" rx="1.4"/><rect x="14" y="3" width="7" height="7" rx="1.4"/><rect x="3" y="14" width="7" height="7" rx="1.4"/><rect x="14" y="14" width="7" height="7" rx="1.4"/>',
    layers:'<path d="m12 3 9 5-9 5-9-5 9-5Z"/><path d="m3 12 9 5 9-5M3 16l9 5 9-5"/>',
    clipboard:'<rect x="5" y="4" width="14" height="17" rx="2"/><rect x="9" y="2.5" width="6" height="4" rx="1.2"/><path d="M9 11h6m-6 4h6"/>',
    search:'<circle cx="10.5" cy="10.5" r="6.5"/><path d="m16 16 4.5 4.5"/>',
    settings:'<path d="m9.5 3-.7 2.2-2 .9-2.2-.5-2 3.4 1.5 1.7v2.6l-1.5 1.7 2 3.4 2.2-.5 2 .9.7 2.2h5l.7-2.2 2-.9 2.2.5 2-3.4-1.5-1.7v-2.6l1.5-1.7-2-3.4-2.2.5-2-.9-.7-2.2Z"/><circle cx="12" cy="12" r="3"/>',
    pin:'<path d="M9 3h6m-5 0v6l-4 4v2h12v-2l-4-4V3m-2 12v6"/>',
    close:'<path d="m6 6 12 12M18 6 6 18"/>',
    plus:'<path d="M12 5v14M5 12h14"/>',
    sun:'<circle cx="12" cy="12" r="4"/><path d="M12 2v2m0 16v2M2 12h2m16 0h2M5 5l1.5 1.5m11 11L19 19M5 19l1.5-1.5m11-11L19 5"/>',
    moon:'<path d="M20.8 13A9 9 0 0 1 11 3.2 9 9 0 1 0 20.8 13Z"/>',
    info:'<circle cx="12" cy="12" r="9"/><path d="M12 11v6m0-10v.1"/>',
    reset:'<path d="M3 10a9 9 0 1 1 2.2 8M3 3v7h7"/>',
    star:'<path d="m12 3 2.8 5.7 6.2.9-4.5 4.4 1.1 6.2-5.6-3-5.6 3 1.1-6.2L3 9.6l6.2-.9L12 3Z"/>',
    folder:'<path d="M3 7V5a2 2 0 0 1 2-2h5l3 3h6a2 2 0 0 1 2 2v11a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V7Zm0 1h18"/>',
    code:'<path d="m8 6-6 6 6 6m8-12 6 6-6 6M14 3l-4 18"/>',
    terminal:'<rect x="2.5" y="4" width="19" height="16" rx="2.5"/><path d="m6 9 3 3-3 3m6 1h5"/>',
    browser:'<circle cx="12" cy="12" r="9"/><path d="M3 12h18M12 3c5 5 5 13 0 18-5-5-5-13 0-18Z"/>',
    pen:'<path d="m16 3 5 5-12 12-6 1 1-6L16 3Zm-3 3 5 5M4 15l5 5"/>',
    note:'<rect x="4" y="3" width="16" height="18" rx="2"/><path d="M8 7h8M8 11h8M8 15h5"/>',
    chat:'<path d="M21 11a9 9 0 0 1-9 9c-1.5 0-3-.4-4.2-1L3 21l1.5-5.2A9 9 0 1 1 21 11Z"/><path d="M8 10h8m-8 4h5"/>',
    music:'<path d="M9 18V5l12-2v13M9 8l12-2"/><ellipse cx="6" cy="18" rx="3" ry="3"/><ellipse cx="18" cy="16" rx="3" ry="3"/>',
    mail:'<rect x="3" y="5" width="18" height="14" rx="2"/><path d="m3 6 9 7 9-7"/>',
    calendar:'<rect x="3" y="5" width="18" height="16" rx="2"/><path d="M7 3v4m10-4v4M3 10h18m-14 4h3m4 0h3m-10 3h3"/>',
    palette:'<path d="M12 3a9 9 0 1 0 0 18h1c1 0 1.5-.8 1-1.8-.6-1.3.3-2.7 1.7-2.7h1.8A4.5 4.5 0 0 0 22 12a9 9 0 0 0-10-9Z"/><path d="M7 10h.1M10 6h.1M16 7h.1M18 11h.1"/>',
    image:'<rect x="3" y="3" width="18" height="18" rx="2"/><circle cx="8" cy="8" r="1.5"/><path d="m3 17 5-5 4 4 4-6 5 7"/>',
    text:'<path d="M4 5h16M12 5v15M8 20h8"/>',
    'text-lines':'<path d="M4 6h16M4 12h11M4 18h14"/>',
    file:'<path d="M14 3H6a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V9l-6-6Zm0 0v6h6M8 13h8m-8 4h5"/>',
    link:'<path d="m10 13 4-4m-5-1 2-2a4.2 4.2 0 0 1 6 6l-2 2m0 2-2 2a4.2 4.2 0 0 1-6-6l2-2"/>',
    copy:'<rect x="8" y="8" width="13" height="13" rx="2"/><path d="M16 8V5a2 2 0 0 0-2-2H5a2 2 0 0 0-2 2v9a2 2 0 0 0 2 2h3"/>',
    more:'<circle cx="5" cy="12" r="1"/><circle cx="12" cy="12" r="1"/><circle cx="19" cy="12" r="1"/>',
    trash:'<path d="M3 6h18M9 6V3h6v3M5 6l1 15h12l1-15M10 10v7m4-7v7"/>',
    pause:'<path d="M8 4v16M16 4v16"/>',
    play:'<path d="m7 3 14 9-14 9V3Z"/>',
    eye:'<path d="M2 12s3.8-7 10-7 10 7 10 7-3.8 7-10 7S2 12 2 12Z"/><circle cx="12" cy="12" r="3"/>',
    shield:'<path d="m12 3 8 3v6c0 5-8 9-8 9s-8-4-8-9V6l8-3Z"/><path d="m8 12 3 3 5-6"/>',
    mouse:'<rect x="6" y="2" width="12" height="20" rx="6"/><path d="M12 2v8M6 10h12"/>',
    lock:'<rect x="4" y="10" width="16" height="11" rx="2"/><path d="M8 10V6a4 4 0 0 1 8 0v4M12 14v3"/>',
    keyboard:'<rect x="2" y="5" width="20" height="14" rx="2"/><path d="M6 9h.1m4 0h.1m4 0h.1m4 0h.1M6 13h.1m4 0h.1m4 0h.1m4 0h.1M7 16h10"/>',
    monitor:'<rect x="2" y="3" width="20" height="14" rx="2"/><path d="M8 21h8m-4-4v4"/>',
    'arrow-bend':'<path d="M4 4v6a4 4 0 0 0 4 4h12m-5-5 5 5-5 5"/>',
    up:'<path d="m5 14 7-7 7 7"/>',down:'<path d="m5 10 7 7 7-7"/>',
    check:'<path d="m5 12 4 4L19 6"/>',edit:'<path d="m16 3 5 5-12 12H4v-5L16 3ZM13 6l5 5"/>',
    download:'<path d="M12 3v12m-5-5 5 5 5-5M4 16v5h16v-5"/>',
    document:'<path d="M14 3H5v18h14V8l-5-5Zm0 0v5h5M8 12h8m-8 4h8"/>'
  };
  function icon(name, extra='') {
    return '<svg class="icon '+extra+'" viewBox="0 0 24 24" aria-hidden="true">'+(paths[name]||paths.grid)+'</svg>';
  }
  function hydrate(root=document) {
    root.querySelectorAll('[data-icon]').forEach(el=>{ el.innerHTML=icon(el.dataset.icon); });
  }
  window.ToolsIcons={icon,hydrate};
})();
