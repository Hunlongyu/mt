(() => {
  'use strict';
  const $ = (selector, root = document) => root.querySelector(selector);
  const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];
  const clone = value => JSON.parse(JSON.stringify(value));
  const esc = value => String(value).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  const icon = name => window.MT_ICONS[name] || '';
  const STORAGE_KEY = 'mt.settings.prototype.v1';
  const defaults = {
    theme: 'light', startup: false, startHidden: true,
    launcherEnabled: true, launcherHotkey: 'Alt+Space', launcherPinned: false, launcherColumns: 4, launcherRows: 7,
    launcherOpacity: 95, launcherEdgeEnabled: false, launcherEdgeZones: ['left'], launcherEdgeMode: 'outer',
    launcherEdgeSuppressFullscreen: true, launcherEdgeBlocklist: '', launcherEdgeAllowlist: '',
    clipboardEnabled: true, clipboardHotkey: 'Alt+C', clipboardPaused: false, clipboardOpacity: 95,
    pasteOnConfirm: true, clipboardSuppressFullscreen: true, hoverPreview: true, spacePreview: true,
    previewSide: 'left', clipboardPlacement: 'cursor', openKey: 'O', openMouseButton: 'middle',
    historyLimit: 500, storageLimit: 1000, maskSensitive: true,
    gesturesEnabled: false, gestureTrigger: 'right', gestureThreshold: 20, gestureTimeout: 1000,
    gestureTrail: true, gestureShowName: true, disableGesturesFullscreen: true, excludedProcesses: '', gestureAllowlist: '',
    rules: [
      { direction: '4', action: 'back', process: '' }, { direction: '6', action: 'forward', process: '' },
      { direction: '8', action: 'top', process: '' }, { direction: '2', action: 'bottom', process: '' },
      { direction: '26', action: 'close', process: '' }, { direction: '1', action: 'minimize', process: '' },
      { direction: '9', action: 'maximize', process: '' }, { direction: '3', action: 'delete', process: '' },
      { direction: '7', action: 'escape', process: '' }, { direction: '39', action: 'refresh', process: '' },
      { direction: '86', action: 'exeDir', process: '' }, { direction: '26', action: 'closeTab', process: 'chrome.exe' },
      { direction: '26', action: 'closeTab', process: 'msedge.exe' }
    ],
    lockEnabled: false, lockPaused: false, lockHotkey: 'Alt+L', requirePassword: false
  };
  let storageAvailable = true;
  let saved = clone(defaults);
  try {
    const stored = JSON.parse(localStorage.getItem(STORAGE_KEY));
    if (stored && typeof stored === 'object' && stored.version === 1 && stored.settings) {
      for (const [key, value] of Object.entries(defaults)) {
        const candidate = stored.settings[key];
        if (Array.isArray(value) ? Array.isArray(candidate) : typeof candidate === typeof value) saved[key] = clone(candidate);
      }
      saved.rules = saved.rules.filter(rule => rule && typeof rule.direction === 'string' && typeof rule.action === 'string' && typeof rule.process === 'string');
    }
  } catch { storageAvailable = false; }
  let draft = clone(saved);
  let page = 'appearance';
  let subpage = { launcher: 'behavior', clipboard: 'behavior', gestures: 'rules' };
  let ruleFilter = 'all';
  let samplePassword = ''; // Ephemeral input only. Never included in draft, storage or diagnostics.
  let passwordConfigured = false;
  let sampleHistoryCount = 128;
  let selectedShortcut = '';
  let toastTimer;
  let dialogHandler = null;
  let dialogReturnFocus = null;
  const systemTheme = window.matchMedia('(prefers-color-scheme: dark)');
  const actionLabel = name => ({
    back: '后退', forward: '前进', top: '滚动到顶部', bottom: '滚动到底部',
    close: '关闭窗口', minimize: '最小化窗口', maximize: '切换最大化',
    delete: '删除', escape: '取消', refresh: '刷新', exeDir: '打开程序目录',
    closeTab: '关闭标签页', key: '发送快捷键', run: '打开程序或文件'
  }[name] || name);
  const pageInfo = () => ({
    general: ['常规', '启动方式与数据位置。', 'settings'],
    appearance: ['外观', '让 MT 更符合你的使用习惯。', 'palette'],
    launcher: ['启动面板', '快捷打开应用、文件和常用位置。', 'layout-grid'],
    clipboard: ['剪贴板', '找回复制过的内容，快速粘贴。', 'copy'],
    gestures: ['鼠标手势', '按住鼠标键移动，松开执行动作。', 'mouse-pointer-2'],
    lock: ['黑屏遮挡', '临时遮挡全部屏幕，并保持唤醒。', 'monitor-off'],
    about: ['关于 MT', 'my tool · 让日常操作更顺手', 'info']
  });
  const moduleKey = id => ({ launcher: 'launcherEnabled', clipboard: 'clipboardEnabled', gestures: 'gesturesEnabled', lock: 'lockEnabled' }[id]);
  const dirty = () => JSON.stringify(draft) !== JSON.stringify(saved) || Boolean(samplePassword);
  function applyTheme() {
    const selected = ['light', 'dark', 'auto'].includes(draft.theme) ? draft.theme : 'light';
    document.documentElement.dataset.theme = selected === 'auto' ? (systemTheme.matches ? 'dark' : 'light') : selected;
    document.documentElement.lang = 'zh-CN';
    document.title = 'MT · 设置界面原型';
  }
  const toggle = (key, label) => `<button type="button" class="switch" role="switch" data-toggle="${key}" aria-checked="${Boolean(draft[key])}" aria-label="${esc(label)}"></button>`;
  const options = (key, values) => `<span class="select-wrap"><select data-setting="${key}" id="${key}">${values.map(([value, label]) => `<option value="${esc(value)}"${String(draft[key]) === String(value) ? ' selected' : ''}>${esc(label)}</option>`).join('')}</select>${icon('chevron-down')}</span>`;
  const number = (key, min, max, unit = '', step = 1) => `<input id="${key}" data-setting="${key}" type="number" min="${min}" max="${max}" step="${step}" value="${draft[key]}" inputmode="numeric">${unit ? `<span class="number-unit">${unit}</span>` : ''}`;
  const keys = value => String(value).split('+').map(key => `<kbd>${esc(key)}</kbd>`).join('<span class="key-plus">+</span>');
  const shortcut = (key, label) => `<button type="button" class="button shortcut" data-shortcut="${key}" aria-label="${esc(label)}: ${esc(draft[key])}"><span>${keys(draft[key])}</span>${icon('pencil')}</button>`;
  const row = (key, label, description, control, extra = '') => `<div class="setting-row"><div class="setting-copy"><label class="setting-label"${key ? ` for="${key}"` : ''}>${label}</label>${description ? `<p class="setting-description">${description}</p>` : ''}${key ? `<div class="field-error" id="error-${key}" role="alert"></div>` : ''}</div><div class="setting-control ${extra}">${control}</div></div>`;
  const switchRow = (key, label, description = '') => row('', label, description, toggle(key, label));
  const selectRow = (key, label, description, values) => row(key, label, description, options(key, values));
  const shortcutRow = (key, label, description) => row('', label, description, shortcut(key, label));
  const section = (label, content, note = '') => `<section class="section"><div class="section-heading"><h2>${label}</h2>${note ? `<span class="section-note">${note}</span>` : ''}</div>${content}</section>`;
  const card = content => `<div class="card">${content}</div>`;
  const footnote = text => `<div class="card-footnote">${text}</div>`;
  const button = (action, text, name = '', className = '') => `<button type="button" class="button ${className}" data-action="${action}">${name ? icon(name) : ''}${text}</button>`;
  const textArea = (key, label, placeholder) => `<label class="stack-label" for="${key}">${label}<textarea id="${key}" data-setting="${key}" placeholder="${esc(placeholder)}">${esc(draft[key])}</textarea><span class="field-error" id="error-${key}" role="alert"></span></label>`;
  function renderGeneral() {
    return section('启动与运行', card(
        switchRow('startup', '开机启动', '登录 Windows 后自动启动 MT。') +
        switchRow('startHidden', '启动时隐藏面板', '在系统托盘中运行，使用快捷键唤起。')
      )) + section('数据位置', card(`<div class="path-row"><span class="setting-label">${'设置与功能数据'}</span><div class="path-label">.\\data\\mt\\</div></div>` + footnote('路径仅为原型示例。旧工具的数据导入位于对应功能页。'))) +
      `<div class="hint-row">${icon('keyboard')}<span>${'快捷键可在对应功能中修改；更改后统一保存。'}</span></div>`;
  }
  function miniature() {
    return '<div class="mini-window" aria-hidden="true"><div class="mini-top"><i></i><i></i><i></i></div><div class="mini-body"><div class="mini-nav"><i></i><i></i><i></i><i></i></div><div class="mini-main"><b></b><i></i><i></i><i></i></div></div></div>';
  }
  function renderAppearance() {
    const themeCards = [['light', 'sun', '浅色'], ['dark', 'moon', '深色'], ['auto', 'monitor', '跟随系统']].map(([value, symbol, label]) =>
      `<button type="button" class="theme-option" role="radio" data-theme-value="${value}" aria-checked="${draft.theme === value}" aria-label="${label}"><div class="theme-swatch ${value === 'auto' ? 'auto-swatch' : `sample-${value}`}">${value === 'auto' ? `<div class="auto-half sample-light">${miniature()}</div><div class="auto-half sample-dark">${miniature()}</div>` : miniature()}</div><div class="theme-label">${icon(symbol)}<span>${label}</span><i class="theme-radio" aria-hidden="true"></i></div></button>`).join('');
    const slider = (key, min, label) => `<div class="range-wrap"><input type="range" data-setting="${key}" id="${key}" min="${min}" max="100" value="${draft[key]}" aria-label="${label}"><output for="${key}" id="output-${key}" class="range-value">${draft[key]}%</output></div>`;
    return section('主题', `<div class="theme-grid" role="radiogroup" aria-label="${'选择主题'}">${themeCards}</div>`, '切换后即时预览') +
      section('面板不透明度', card(
        row('launcherOpacity', '启动面板', '面板及附属搜索框使用相同不透明度。', slider('launcherOpacity', 30, '启动面板不透明度')) +
        row('clipboardOpacity', '剪贴板', '调低后可以看到面板后方的内容。', slider('clipboardOpacity', 60, '剪贴板不透明度')) +
        footnote('100% 为完全不透明。设置窗口始终保持不透明。')
      )) + section('字体与图标', card(`<div class="appearance-example"><div><div class="font-example">${'清晰、熟悉，恰到好处。'}</div><div class="font-explain">${'跟随 Windows 系统字体 · Lucide 线性图标'}</div></div><div class="sample-caption"><span></span>${'全应用统一'}</div></div>`));
  }
  function renderLauncher() {
    if (subpage.launcher === 'layout') {
      const columns = Math.max(3, Math.min(20, Number(draft.launcherColumns) || 4));
      const rows = Math.max(3, Math.min(20, Number(draft.launcherRows) || 7));
      return section('网格布局', card(`<div class="layout-preview"><div class="grid-monitor" aria-hidden="true"><div class="grid-monitor-header"></div><div class="grid-preview" id="grid-preview" style="grid-template-columns:repeat(${columns},1fr);grid-template-rows:repeat(${rows},1fr)">${'<i></i>'.repeat(columns * rows)}</div><div class="grid-monitor-footer"><i></i><i></i><i></i><i></i></div></div><div><div class="grid-summary" id="grid-summary">${columns} × ${rows}</div><p id="grid-capacity">${`每页 ${columns * rows} 个位置`}</p><p>${'固定空槽 · 底部分类'}</p></div></div><div class="two-fields"><label for="launcherColumns">${'列数'}${number('launcherColumns', 3, 20)}</label><label for="launcherRows">${'行数'}${number('launcherRows', 3, 20)}</label></div>` + footnote('行列数均可设为 3–20，默认 4 × 7；面板尺寸随之变化。'))) +
        `<div class="hint-row">${icon('info')}<span>${'更改列数时保留条目的行列位置，被裁掉的列从下一行顺延；行数只影响每页容量，空槽会保留。'}</span></div>`;
    }
    if (subpage.launcher === 'edge') {
      const zones = [['topLeft', '左上角'], ['top', '上边'], ['topRight', '右上角'], ['left', '左边'], ['', ''], ['right', '右边'], ['bottomLeft', '左下角'], ['bottom', '下边'], ['bottomRight', '右下角']];
      return section('屏幕边缘唤起', card(switchRow('launcherEdgeEnabled', '鼠标停留时唤起', '将指针移到选中的边缘或角落。') + `<div ${!draft.launcherEdgeEnabled ? 'class="disabled-content"' : ''}><div class="edge-picker" role="group" aria-label="${'触发位置'}">${zones.map(([key, label]) => key ? `<button type="button" data-zone="${key}" aria-pressed="${draft.launcherEdgeZones.includes(key)}"${!draft.launcherEdgeEnabled ? ' disabled' : ''}>${label}</button>` : `<div class="edge-center">${icon('monitor')}</div>`).join('')}</div></div>` + footnote('可选择多个位置；唤起不会关闭已经打开的面板。'))) +
        section('触发范围', card(selectRow('launcherEdgeMode', '多显示器', '', [['outer', '仅桌面外边缘'], ['every', '每个显示器的边缘']]) + switchRow('launcherEdgeSuppressFullscreen', '全屏时不响应', '避免游戏或演示期间意外弹出。'))) +
        section('程序规则', card(`<div class="list-fields">${textArea('launcherEdgeBlocklist', '禁用程序', 'game.exe')}${textArea('launcherEdgeAllowlist', '仅允许这些程序', '留空表示不限，每行一个程序名')}</div>` + footnote('白名单非空时只在列表内的程序中唤起；同名项白名单优先。')));
    }
    return section('唤起与显示', card(
      shortcutRow('launcherHotkey', '唤起快捷键', '在任意应用中打开启动面板。') +
      switchRow('launcherPinned', '保持面板打开', '切换到其他窗口时，面板仍然可见。')
    )) + section('启动项数据', card(row('', '从 HLaunch 导入', '合并同名分类，跳过完全相同的启动项。', button('import-launcher', '导入…', 'upload')) + footnote('导入时只读原始数据，不改变 HLaunch 中的内容。'))) +
      `<div class="hint-row">${icon('keyboard')}<span>${'面板中直接输入或按 Ctrl + F 搜索；右键空槽可注册新项目。'}</span></div>`;
  }
  function renderClipboard() {
    if (subpage.clipboard === 'preview') return section('内容预览', card(
      switchRow('hoverPreview', '鼠标悬停预览', '指针停留在记录上时显示完整内容。') +
      switchRow('spacePreview', '按住空格预览') +
      selectRow('previewSide', '优先显示位置', '空间不足时自动切换到另一侧。', [['left', '面板左侧'], ['right', '面板右侧']])
    )) + section('打开内容', card(
      selectRow('openKey', '打开按键', '打开链接，或在资源管理器中定位文件。', [['', '关闭'], ...'ABCEGIJKLMNOPQRUVWXYZ'.split('').map(value => [value, value])]) +
      selectRow('openMouseButton', '鼠标打开', '', [['middle', '中键'], ['right', '右键'], ['none', '关闭']]) +
      (draft.openMouseButton === 'right' ? footnote('选择右键后不再弹出右键菜单，可按 Shift + F10 打开菜单。') : '')
    ));
    if (subpage.clipboard === 'history') return section('历史容量', card(
      row('historyLimit', '面板显示条数', '限制面板中显示的普通记录数量。', number('historyLimit', 1, 100000, '条')) +
      row('storageLimit', '存储上限', '不能小于面板显示条数。', number('storageLimit', 1, 100000, '条')) +
      footnote('收藏和置顶记录始终显示，也不占用以上名额。')
    )) + section('隐私', card(switchRow('maskSensitive', '遮蔽敏感内容', '遮蔽被系统标记为敏感的剪贴板内容。'))) +
      section('历史数据', card(
        row('', '从 Hopy 导入', '将原有历史合并到 MT，保留源数据。', button('import-clipboard', '导入…', 'upload')) +
        row('', '清空历史记录', '包含收藏和置顶记录，清空前会再次确认。', button('clear-history', '清空…', 'trash-2', 'danger'))
      ));
    return section('唤起与位置', card(
      shortcutRow('clipboardHotkey', '唤起快捷键', '打开剪贴板历史面板。') +
      switchRow('clipboardSuppressFullscreen', '全屏时不响应快捷键', '全屏应用中不唤起面板。') +
      selectRow('clipboardPlacement', '无输入光标时', '有输入光标时，优先在光标附近显示。', [['cursor', '显示在鼠标位置'], ['center', '显示在屏幕中央']])
    )) + section('记录与粘贴', card(
      switchRow('pasteOnConfirm', '确认后立即粘贴', '关闭后只复制到剪贴板，由你手动粘贴。') +
      switchRow('clipboardPaused', '暂停记录新内容', '已有历史仍可查看和使用。')
    ));
  }
  const directionNames = { '1': 'arrow-down-left', '2': 'arrow-down', '3': 'arrow-down-right', '4': 'arrow-left', '6': 'arrow-right', '7': 'arrow-up-left', '8': 'arrow-up', '9': 'arrow-up-right' };
  const directionText = { '1': '↙', '2': '↓', '3': '↘', '4': '←', '6': '→', '7': '↖', '8': '↑', '9': '↗' };
  const arrows = value => `<span class="direction-icons" role="img" aria-label="${esc([...value].map(d => directionText[d] || d).join(' '))}">${[...value].map(d => icon(directionNames[d])).join('')}</span>`;
  function renderGestures() {
    if (subpage.gestures === 'behavior') return section('触发方式', card(
      selectRow('gestureTrigger', '触发鼠标键', '按住并移动后松开，普通点击仍保留。', [['right', '右键'], ['middle', '中键'], ['x1', '侧键 M4'], ['x2', '侧键 M5']]) +
      row('gestureThreshold', '方向分段阈值', '移动达到此距离后识别为一个方向。', number('gestureThreshold', 5, 200, 'px')) +
      row('gestureTimeout', '停顿取消', '0 表示不超时；也可按 Esc 取消。', number('gestureTimeout', 0, 10000, 'ms', 200))
    )) + section('轨迹反馈', card(
      switchRow('gestureTrail', '显示手势轨迹', '亮蓝色轨迹、起点圆点和末端方向箭头。') +
      switchRow('gestureShowName', '显示动作名称', '在当前屏幕底部显示即将执行的动作。')
    ));
    if (subpage.gestures === 'scope') return section('生效范围', card(switchRow('disableGesturesFullscreen', '全屏程序中禁用', '避免影响游戏、视频和演示。'))) +
      section('程序规则', card(`<div class="list-fields">${textArea('excludedProcesses', '禁用程序', '如 game.exe，每行一个程序名')}${textArea('gestureAllowlist', '仅允许这些程序', '留空表示不限')}</div>` + footnote('只填写程序文件名。应用专属手势在“手势规则”中添加。')));
    const visibleRules = draft.rules.map((rule, index) => ({ ...rule, index })).filter(rule => ruleFilter === 'all' || (ruleFilter === 'global' ? !rule.process : Boolean(rule.process)));
    return `<div class="gesture-example">${arrows('26')}<div><strong>${'一笔，完成一个动作'}</strong><p>${'例如按住右键，向下再向右，松开关闭窗口。'}</p></div></div>` +
      `<div class="rule-toolbar"><span class="select-wrap"><select id="rule-filter" aria-label="${'筛选手势规则'}"><option value="all"${ruleFilter === 'all' ? ' selected' : ''}>${'全部规则'}</option><option value="global"${ruleFilter === 'global' ? ' selected' : ''}>${'全局规则'}</option><option value="app"${ruleFilter === 'app' ? ' selected' : ''}>${'应用专属'}</option></select>${icon('chevron-down')}</span>${button('add-rule', '添加手势', 'plus')}</div>` +
      card(visibleRules.length ? `<table class="rule-table"><thead><tr><th>${'手势'}</th><th>${'执行动作 / 适用程序'}</th><th><span class="rule-count">${`${visibleRules.length} 条`}</span></th></tr></thead><tbody>${visibleRules.map(rule => `<tr><td>${arrows(rule.direction)}</td><td><span class="rule-action-label">${esc(actionLabel(rule.action))}${rule.value ? ` · ${esc(rule.value)}` : ''}</span><div class="rule-scope">${esc(rule.process || '全局')}</div></td><td><div class="rule-actions"><button type="button" class="icon-button" data-edit-rule="${rule.index}" aria-label="${'编辑'} ${esc([...rule.direction].map(d => directionText[d]).join(''))} ${esc(rule.process)}">${icon('pencil')}</button><button type="button" class="icon-button danger" data-delete-rule="${rule.index}" aria-label="${'删除'} ${esc([...rule.direction].map(d => directionText[d]).join(''))} ${esc(rule.process)}">${icon('trash-2')}</button></div></td></tr>`).join('')}</tbody></table>` : `<div class="empty-state">${'暂无规则，添加一个手势开始使用。'}</div>`) +
      `<div class="hint">${'应用专属规则优先于相同方向的全局规则。'}</div>`;
  }
  function renderLock() {
    return section('启动与暂停', card(
      shortcutRow('lockHotkey', '锁定快捷键', '开启遮挡；简单模式下也用于解锁。') +
      switchRow('lockPaused', '暂停黑屏功能', '暂时停用快捷键与托盘入口。')
    )) + section('解锁方式', `<div class="mode-grid" role="radiogroup" aria-label="${'解锁方式'}"><button type="button" class="mode-card" data-lock-mode="simple" role="radio" aria-checked="${!draft.requirePassword}">${icon('keyboard')}<div><strong>${'简单解锁'}</strong><p>${'按 Enter 或再次按锁定快捷键。'}</p></div></button><button type="button" class="mode-card" data-lock-mode="password" role="radio" aria-checked="${draft.requirePassword}">${icon('shield-check')}<div><strong>${'密码解锁'}</strong><p>${'输入密码后按 Enter 解锁。'}</p></div></button></div>` +
      (draft.requirePassword ? `<div class="card" style="margin-top:12px"><div class="password-field"><label for="sample-password">${'示例密码'}</label><input id="sample-password" type="password" autocomplete="new-password" maxlength="128" placeholder="${esc((passwordConfigured ? '留空保留本次示例状态' : '1–128 位英文字母或数字'))}" value="${esc(samplePassword)}"></div>${footnote('密码模式下，锁定快捷键不会解锁；Esc 清空输入，Backspace 删除一位。原型不会保存密码。')}<div class="field-error" id="error-password" style="padding:0 18px 12px" role="alert"></div></div>` : `<p class="hint">${'先松开锁定时按住的按键，再执行解锁。'}</p>`)) +
      section('遮挡期间', card(`<div class="read-only-row">${icon('monitor')}<span>${'覆盖全部显示器'}</span><span>${'始终启用'}</span></div><div class="read-only-row">${icon('power')}<span>${'保持系统和显示器唤醒'}</span><span>${'始终启用'}</span></div>`)) +
      `<div class="hint-row">${icon('info')}<span>${'黑屏遮挡不等同于 Windows 账户锁定。应急时可按 Ctrl + Alt + Del；MT 将解除遮挡。'}</span></div>`;
  }
  function renderAbout() {
    return `<div class="about-hero"><div class="about-logo">MT</div><div><h2>my tool</h2><p>${'版本 0.1.0 · 开发预览'}</p></div></div><p class="about-description">${'一个轻巧的入口，安放常用的工具。<br>启动面板、剪贴板、鼠标手势与黑屏遮挡。'}</p>` +
      section('开源许可', card(row('', 'Lucide', '统一线性图标 · ISC 许可证', button('license', '查看许可', 'external-link')))) +
      `<div class="hint-row">${icon('info')}<span>${'当前为设置界面 HTML 原型。界面操作仅修改本浏览器的示例状态。'}</span></div>`;
  }
  function renderNav() {
    const info = pageInfo();
    const item = id => `<button type="button" class="nav-item" data-page="${id}" ${page === id ? 'aria-current="page"' : ''} title="${info[id][0]}">${icon(info[id][2])}<span>${info[id][0]}</span></button>`;
    $('#navigation').innerHTML = `<div class="nav-group-label">${'偏好设置'}</div>${item('general')}${item('appearance')}<div class="nav-group-label">${'功能'}</div>${['launcher', 'clipboard', 'gestures', 'lock'].map(item).join('')}`;
    $('#about-button').innerHTML = `${icon('info')}<span>${'关于 MT'}</span><span class="version">0.1.0</span>`;
    $('#about-button').title = '关于 MT';
    if (page === 'about') $('#about-button').setAttribute('aria-current', 'page'); else $('#about-button').removeAttribute('aria-current');
  }
  function renderChrome() {
    $('#window-title').textContent = 'MT · 设置';
    $('#review-label').textContent = '设置界面';
    $('#prototype-label').textContent = '交互原型 · 02';
    $('#prototype-note').textContent = '仅操作浏览器中的示例设置';
    $('#reset-demo').innerHTML = icon('rotate-ccw') + '重置演示';
    $('#close-settings').innerHTML = icon('x');
    $('#close-settings').ariaLabel = '关闭设置';
    $('#dialog-close').innerHTML = icon('x');
    $('#dialog-close').ariaLabel = '关闭弹窗';
    $('#cancel-settings').textContent = '取消';
    $('#save-settings').textContent = '保存设置';
    $('#review-caption').innerHTML = `<span>${'设计提案 · 共用现有主题与 Lucide 图标'}</span><span>${'可切换分类、修改设置并预览保存 / 取消'}</span>`;
    $('#closed-label').textContent = '设置窗口已关闭';
    $('#reopen-settings').textContent = '重新打开设置';
  }
  function updateSaveState() {
    const changed = dirty();
    $('#save-state').className = `save-state${changed ? ' dirty' : ''}`;
    $('#save-state').innerHTML = changed ? '有未保存的更改' : `${icon('check')}${'所有更改已保存'}`;
    if (!storageAvailable && !changed) $('#save-state').textContent = '当前仅在内存中保存';
    $('#save-settings').disabled = !changed;
  }
  function renderPage(preserveScroll = false) {
    const oldScroll = $('#page-scroll').scrollTop;
    applyTheme();
    renderNav();
    renderChrome();
    const info = pageInfo()[page];
    $('#page-title').textContent = info[0];
    $('#page-description').textContent = info[1];
    const enabledKey = moduleKey(page);
    $('#page-action').innerHTML = enabledKey ? `<div class="module-status"><span>${draft[enabledKey] ? '已启用' : '已停用'}</span>${toggle(enabledKey, '启用' + info[0])}</div>` : '';
    const tabs = {
      launcher: [['behavior', '常规'], ['layout', '面板布局'], ['edge', '边缘唤起']],
      clipboard: [['behavior', '常规'], ['preview', '预览与打开'], ['history', '历史数据']],
      gestures: [['rules', '手势规则'], ['behavior', '触发与反馈'], ['scope', '生效范围']]
    }[page];
    $('#page-tabs').hidden = !tabs;
    $('#page-tabs').innerHTML = tabs ? tabs.map(([id, title]) => `<button type="button" role="tab" aria-selected="${subpage[page] === id}" tabindex="${subpage[page] === id ? 0 : -1}" data-subpage="${id}" id="tab-${id}" aria-controls="page-content">${title}</button>`).join('') : '';
    if (tabs) { $('#page-content').setAttribute('role', 'tabpanel'); $('#page-content').setAttribute('aria-labelledby', `tab-${subpage[page]}`); }
    else { $('#page-content').removeAttribute('role'); $('#page-content').removeAttribute('aria-labelledby'); }
    $('#page-content').innerHTML = ({ general: renderGeneral, appearance: renderAppearance, launcher: renderLauncher, clipboard: renderClipboard, gestures: renderGestures, lock: renderLock, about: renderAbout })[page]();
    for (const field of $$('input[data-setting][type=number]', $('#page-content'))) {
      if (field.labels?.[0]) field.setAttribute('aria-label', field.labels[0].textContent.trim());
      if (!$(`#error-${field.id}`)) {
        const error = document.createElement('span');
        error.id = `error-${field.id}`; error.className = 'field-error'; error.setAttribute('role', 'alert');
        field.insertAdjacentElement('afterend', error);
      }
    }
    $('#page-scroll').scrollTop = preserveScroll ? oldScroll : 0;
    updateSaveState();
  }
  function toast(message) {
    clearTimeout(toastTimer);
    $('#toast').innerHTML = icon('check') + esc(message);
    $('#toast').hidden = false;
    toastTimer = setTimeout(() => { $('#toast').hidden = true; }, 3200);
  }
  function dialog({ title, body, confirm = '确定', danger = false, onConfirm, hideCancel = false }) {
    dialogReturnFocus = document.activeElement;
    $('#dialog-title').textContent = title;
    $('#dialog-body').innerHTML = body;
    $('#dialog-cancel').textContent = '取消';
    $('#dialog-cancel').hidden = hideCancel;
    $('#dialog-confirm').textContent = confirm;
    $('#dialog-confirm').className = `button primary${danger ? ' danger' : ''}`;
    const extra = $('#dialog-extra');
    if (extra) extra.remove();
    dialogHandler = onConfirm;
    $('#dialog').showModal();
    if (danger) $('#dialog-cancel').focus();
  }
  function closeDialog() {
    $('#dialog').close();
    dialogHandler = null;
    selectedShortcut = '';
    $('#dialog-body').innerHTML = '';
    if (dialogReturnFocus?.isConnected) dialogReturnFocus.focus();
    else if (!$('#settings-window').hidden) ($(`[data-page="${page}"]`) || $('#about-button')).focus();
  }
  const showDialogError = message => {
    let target = $('#dialog-error');
    if (!target) { target = document.createElement('p'); target.id = 'dialog-error'; target.className = 'dialog-error'; target.setAttribute('role', 'alert'); $('#dialog-body').append(target); }
    target.textContent = message;
  };
  function normalizedShortcut(value) {
    const parts = String(value).split('+').map(part => part.trim()).filter(Boolean);
    const aliases = { ctrl: 'Ctrl', control: 'Ctrl', alt: 'Alt', shift: 'Shift', win: 'Win', meta: 'Win', space: 'Space', enter: 'Enter', escape: 'Esc', esc: 'Esc', tab: 'Tab', backspace: 'Backspace', delete: 'Delete', insert: 'Insert', home: 'Home', end: 'End', pageup: 'PageUp', pagedown: 'PageDown', left: 'Left', right: 'Right', up: 'Up', down: 'Down' };
    const canonical = parts.map(part => aliases[part.toLowerCase()] || part.toUpperCase());
    const mods = ['Ctrl', 'Alt', 'Shift', 'Win'].filter(key => canonical.includes(key));
    const key = canonical.filter(part => !['Ctrl', 'Alt', 'Shift', 'Win'].includes(part));
    if (key.length !== 1 || !/^(?:[A-Z0-9]|F(?:[1-9]|1[0-9]|2[0-4])|Space|Enter|Esc|Tab|Backspace|Delete|Insert|Home|End|PageUp|PageDown|Left|Right|Up|Down)$/.test(key[0])) return null;
    return [...mods, key[0]].join('+');
  }
  function editShortcut(key) {
    selectedShortcut = key;
    dialog({ title: '设置快捷键', body: `<p>${'点击下方区域后按下组合键，或直接编辑快捷键文本。'}</p><div class="shortcut-recorder" id="shortcut-recorder" tabindex="0" role="button" aria-label="${'按下快捷键'}">${keys(draft[key])}</div><label class="stack-label" for="shortcut-text">${'快捷键'}<input type="text" id="shortcut-text" value="${esc(draft[key])}" autocomplete="off" spellcheck="false"></label><div class="dialog-error" id="dialog-error" role="alert"></div>`, confirm: '使用此快捷键', onConfirm: () => {
      const value = normalizedShortcut($('#shortcut-text').value);
      if (!value || (!value.includes('+') && !/^F\d+$/.test(value))) { showDialogError('请使用修饰键组合，如 Alt+C，或 F1–F24。'); return false; }
      const fields = ['launcherHotkey', 'clipboardHotkey', 'lockHotkey'];
      const conflict = fields.find(field => field !== key && normalizedShortcut(draft[field]) === value);
      if (conflict) { showDialogError('此快捷键已被其他功能使用，请换一个。'); return false; }
      draft[key] = value;
      renderPage(true);
      return true;
    } });
    $('#shortcut-recorder').focus();
  }
  function editRule(index = -1) {
    const current = index < 0 ? { direction: '', action: 'back', process: '', value: '' } : draft.rules[index];
    const actions = ['back', 'forward', 'top', 'bottom', 'close', 'minimize', 'maximize', 'delete', 'escape', 'refresh', 'exeDir', 'closeTab', 'key', 'run'];
    dialog({ title: index < 0 ? '添加手势' : '编辑手势', body: `<label class="stack-label" for="rule-direction">${'方向顺序'}<div class="directions-editor"><input type="text" id="rule-direction" maxlength="30" value="${esc(current.direction)}" placeholder="${'例如 26，表示 ↓ →'}" autocomplete="off"><span id="rule-direction-preview">${arrows(current.direction)}</span></div></label><div class="direction-pad">${['7','8','9','4','','6','1','2','3'].map(d => d ? `<button type="button" data-add-direction="${d}" aria-label="${directionText[d]}">${icon(directionNames[d])}</button>` : `<button type="button" data-clear-direction aria-label="${'清空方向'}">${icon('rotate-ccw')}</button>`).join('')}</div><p class="direction-help">${'点击箭头依次添加方向，也可输入对应数字。'}</p><label class="stack-label" for="rule-action">${'执行动作'}<span class="select-wrap"><select id="rule-action">${actions.map(action => `<option value="${action}"${action === current.action ? ' selected' : ''}>${actionLabel(action)}</option>`).join('')}</select>${icon('chevron-down')}</span></label><label class="stack-label" for="rule-value" id="rule-value-label"${['key','run'].includes(current.action) ? '' : ' hidden'}><span id="rule-value-title">${current.action === 'run' ? '打开目标' : '快捷键'}</span><input id="rule-value" type="text" value="${esc(current.value || '')}" placeholder="${current.action === 'run' ? 'C:\\Apps\\app.exe' : 'Ctrl+W'}"></label><label class="stack-label" for="rule-process">${'适用程序'}<input type="text" id="rule-process" value="${esc(current.process)}" placeholder="${'留空为全局，例如 chrome.exe'}"></label><div id="dialog-error" class="dialog-error" role="alert"></div>`, confirm: '保存手势', onConfirm: () => {
      const direction = $('#rule-direction').value.trim().replace(/\s/g, '');
      const action = $('#rule-action').value;
      const process = $('#rule-process').value.trim().toLowerCase();
      let value = $('#rule-value').value.trim();
      if (!/^[12346789]{1,30}$/.test(direction)) { showDialogError('请输入 1–30 个有效方向数字：1、2、3、4、6、7、8、9。'); return false; }
      if (/([12346789])\1/.test(direction)) { showDialogError('相邻方向不能重复。'); return false; }
      if (process && /[\\/:*?"<>|]/.test(process)) { showDialogError('请填写程序文件名，不包含路径。'); return false; }
      if (draft.rules.some((rule, i) => i !== index && rule.direction === direction && rule.process.toLowerCase() === process)) { showDialogError('同一程序内已存在此方向的手势。'); return false; }
      if (action === 'key') { value = normalizedShortcut(value); if (!value) { showDialogError('请填写有效的快捷键，例如 Ctrl+W。'); return false; } }
      if (action === 'run' && (!value || /^(javascript|data|vbscript):/i.test(value))) { showDialogError('请填写程序、文件路径或有效网址。'); return false; }
      const rule = { direction, action, process };
      if (['key', 'run'].includes(action)) rule.value = value;
      if (index < 0) draft.rules.push(rule); else draft.rules[index] = rule;
      renderPage(true);
      return true;
    } });
  }
  function fieldIssue(key) {
    const ranges = { launcherColumns: [3,20], launcherRows: [3,20], historyLimit: [1,100000], storageLimit: [1,100000], gestureThreshold: [5,200], gestureTimeout: [0,10000] };
    if (ranges[key]) {
      const [min,max] = ranges[key];
      if (!Number.isInteger(draft[key]) || draft[key] < min || draft[key] > max) return `请输入 ${min}–${max} 之间的整数。`;
    }
    if (key === 'storageLimit' && draft.storageLimit < draft.historyLimit) return '存储上限不能小于面板显示条数。';
    if (key === 'gestureTimeout' && draft.gestureTimeout > 0 && draft.gestureTimeout < 200) return '请设为 0，或 200–10000 毫秒。';
    if (['launcherEdgeBlocklist','launcherEdgeAllowlist','excludedProcesses','gestureAllowlist'].includes(key)) {
      const names = draft[key].split(/[;；,，\r\n]+/).map(x => x.trim()).filter(Boolean);
      if (names.length > 64 || names.some(name => name.length > 260 || /[\\/:*?"<>|]/.test(name))) return '最多填写 64 个程序文件名，不包含路径。';
    }
    return '';
  }
  function showFieldIssue(key) {
    const message = fieldIssue(key);
    const field = $(`#${key}`);
    const target = $(`#error-${key}`);
    if (target) target.textContent = message;
    if (field) { field.setAttribute('aria-invalid', Boolean(message)); if (target) field.setAttribute('aria-describedby', target.id); }
    return message;
  }
  function validateDraft() {
    const fields = {
      launcherColumns: ['launcher','layout'], launcherRows: ['launcher','layout'], historyLimit: ['clipboard','history'], storageLimit: ['clipboard','history'],
      gestureThreshold: ['gestures','behavior'], gestureTimeout: ['gestures','behavior'], launcherEdgeBlocklist: ['launcher','edge'], launcherEdgeAllowlist: ['launcher','edge'],
      excludedProcesses: ['gestures','scope'], gestureAllowlist: ['gestures','scope']
    };
    for (const [key,[target,tab]] of Object.entries(fields)) if (fieldIssue(key)) {
      page = target; subpage[target] = tab; renderPage(); showFieldIssue(key); $(`#${key}`).focus(); return false;
    }
    if (draft.launcherEdgeEnabled && draft.launcherEdgeZones.length === 0) {
      page = 'launcher'; subpage.launcher = 'edge'; renderPage(); toast('请至少选择一个屏幕边缘或角落。'); return false;
    }
    if (draft.requirePassword && ((!passwordConfigured && !samplePassword) || (samplePassword && !/^[A-Za-z0-9]{1,128}$/.test(samplePassword)))) {
      page = 'lock'; renderPage(); $('#error-password').textContent = '请输入 1–128 位英文字母或数字作为示例密码。'; $('#sample-password').setAttribute('aria-invalid','true'); $('#sample-password').focus(); return false;
    }
    return true;
  }
  function saveSettings(closeAfter = false) {
    if (!validateDraft()) return false;
    if (samplePassword) passwordConfigured = true;
    samplePassword = '';
    saved = clone(draft);
    // Password mode may be previewed for this session. No password or derived credential is persisted.
    const persisted = clone(saved); persisted.requirePassword = false;
    try { localStorage.setItem(STORAGE_KEY, JSON.stringify({ version: 1, settings: persisted })); storageAvailable = true; }
    catch { storageAvailable = false; }
    renderPage(true);
    if (closeAfter) closeWindow();
    toast(storageAvailable ? '示例设置已保存' : '浏览器存储不可用，已在本次会话中保存');
    return true;
  }
  function closeWindow() {
    samplePassword = '';
    $('#settings-window').hidden = true;
    $('#review-caption').hidden = true;
    $('#closed-state').hidden = false;
    $('#reopen-settings').focus();
  }
  function cancelSettings() {
    draft = clone(saved); samplePassword = ''; renderPage(); closeWindow();
  }
  function requestClose() {
    if (!dirty()) { closeWindow(); return; }
    dialog({ title: '保存更改？', body: `<p>${'你的设置尚未保存。保存后关闭，或放弃本次更改。'}</p>`, confirm: '保存并关闭', onConfirm: () => {
      closeDialog(); saveSettings(true); return false;
    } });
    const discard = document.createElement('button'); discard.type = 'button'; discard.id = 'dialog-extra'; discard.className = 'button'; discard.style.marginRight = 'auto'; discard.textContent = '放弃更改';
    discard.addEventListener('click', () => { closeDialog(); cancelSettings(); });
    $('.dialog-footer').prepend(discard);
  }
  const actions = {
    'add-rule': () => editRule(),
    'import-launcher': () => importDemo('HLaunch'),
    'import-clipboard': () => importDemo('Hopy'),
    'clear-history': () => dialog({ title: '清空全部历史？', body: `<p>${`即将清空示例中的 ${sampleHistoryCount} 条记录，包括收藏和置顶。此操作不会影响系统剪贴板或旧工具。`}</p>`, confirm: '清空示例记录', danger: true, onConfirm: () => { sampleHistoryCount = 0; toast('示例历史已清空'); return true; } }),
    license: () => dialog({ title: 'Lucide · ISC 许可证', body: `<p>lucide-static 0.460.0</p><p style="white-space:pre-wrap">${esc(window.MT_LICENSE)}</p>`, confirm: '关闭', hideCancel: true, onConfirm: () => true })
  };
  function importDemo(source) {
    const detail = source === 'HLaunch' ? '8 个启动项 · 2 个分类' : '24 条历史 · 含 3 条收藏';
    dialog({ title: `从 ${source} 导入`, body: `<p>${'这是导入流程演示，使用内置示例，不会读取本机文件。'}</p><div class="card"><div class="path-row"><div class="setting-label">${source} ${'示例数据'}</div><div class="hint">${detail}</div></div>${footnote('只读复制并合并，原始数据保持不变。')}</div>`, confirm: '模拟导入', onConfirm: () => { if (source === 'Hopy') sampleHistoryCount += 24; toast(`${source} 示例导入完成`); return true; } });
  }
  document.addEventListener('click', event => {
    const target = event.target.closest('button');
    if (!target || target.disabled) return;
    if (target.dataset.page) { page = target.dataset.page; renderPage(); $(`[data-page="${page}"]`).focus(); }
    else if (target.dataset.subpage) { subpage[page] = target.dataset.subpage; renderPage(); $(`[data-subpage="${subpage[page]}"]`).focus(); }
    else if (target.dataset.toggle) {
      const key = target.dataset.toggle; draft[key] = !draft[key];
      if (key === 'launcherEdgeEnabled' || key === moduleKey(page)) { renderPage(true); $(`[data-toggle="${key}"]`)?.focus(); }
      else { target.setAttribute('aria-checked', Boolean(draft[key])); updateSaveState(); }
    } else if (target.dataset.themeValue) { draft.theme = target.dataset.themeValue; renderPage(true); $(`[data-theme-value="${draft.theme}"]`).focus(); }
    else if (target.dataset.shortcut) editShortcut(target.dataset.shortcut);
    else if (target.dataset.zone) {
      const zone = target.dataset.zone; const selected = draft.launcherEdgeZones.includes(zone);
      draft.launcherEdgeZones = selected ? draft.launcherEdgeZones.filter(value => value !== zone) : [...draft.launcherEdgeZones, zone];
      target.setAttribute('aria-pressed', !selected); updateSaveState();
    } else if (target.dataset.lockMode) { draft.requirePassword = target.dataset.lockMode === 'password'; if (!draft.requirePassword) samplePassword = ''; renderPage(true); $(`[data-lock-mode="${target.dataset.lockMode}"]`).focus(); }
    else if (target.dataset.editRule !== undefined) editRule(Number(target.dataset.editRule));
    else if (target.dataset.deleteRule !== undefined) {
      const index = Number(target.dataset.deleteRule), rule = draft.rules[index];
      dialog({ title: '删除这条手势？', body: `<p>${esc(actionLabel(rule.action))} · ${esc(rule.process || '全局')}</p>${arrows(rule.direction)}`, confirm: '删除手势', danger: true, onConfirm: () => { draft.rules.splice(index, 1); renderPage(true); return true; } });
    } else if (target.dataset.addDirection) {
      const field = $('#rule-direction'); if (field.value.length < 30) field.value += target.dataset.addDirection;
      $('#rule-direction-preview').innerHTML = arrows(field.value);
    } else if (target.hasAttribute('data-clear-direction')) { $('#rule-direction').value = ''; $('#rule-direction-preview').innerHTML = ''; }
    else if (target.dataset.action) actions[target.dataset.action]?.();
  });
  document.addEventListener('input', event => {
    const target = event.target;
    const key = target.dataset.setting;
    if (key) {
      draft[key] = ['number','range'].includes(target.type) ? (target.value === '' ? null : Number(target.value)) : target.value;
      if (target.type === 'range') $(`#output-${key}`).textContent = `${target.value}%`;
      if (key === 'launcherColumns' || key === 'launcherRows') {
        const columns = Math.max(3, Math.min(20, Number(draft.launcherColumns) || 4)), rows = Math.max(3, Math.min(20, Number(draft.launcherRows) || 7));
        $('#grid-preview').style.gridTemplateColumns = `repeat(${columns},1fr)`; $('#grid-preview').style.gridTemplateRows = `repeat(${rows},1fr)`;
        $('#grid-preview').innerHTML = '<i></i>'.repeat(columns * rows); $('#grid-summary').textContent = `${columns} × ${rows}`; $('#grid-capacity').textContent = `每页 ${columns * rows} 个位置`;
      }
      if (key === 'historyLimit' && draft.historyLimit > draft.storageLimit && draft.historyLimit <= 100000) { draft.storageLimit = draft.historyLimit; $('#storageLimit').value = draft.storageLimit; showFieldIssue('storageLimit'); }
      updateSaveState();
    }
    if (target.id === 'sample-password') { samplePassword = target.value; updateSaveState(); }
    if (target.id === 'rule-direction') $('#rule-direction-preview').innerHTML = arrows(target.value);
    if (target.id === 'shortcut-text') $('#shortcut-recorder').innerHTML = keys(target.value);
  });
  document.addEventListener('change', event => {
    const target = event.target, key = target.dataset.setting;
    if (key) {
      if (target.tagName === 'SELECT') draft[key] = target.value;
      if (key === 'openMouseButton') { renderPage(true); $(`#${key}`)?.focus(); } else updateSaveState();
    }
    if (target.id === 'rule-filter') { ruleFilter = target.value; renderPage(true); $('#rule-filter').focus(); }
    if (target.id === 'rule-action') { const action = target.value; $('#rule-value-label').hidden = !['key','run'].includes(action); $('#rule-value-title').textContent = action === 'run' ? '打开目标' : '快捷键'; $('#rule-value').placeholder = action === 'run' ? 'C:\\Apps\\app.exe' : 'Ctrl+W'; }
  });
  document.addEventListener('focusout', event => { if (event.target.dataset.setting) showFieldIssue(event.target.dataset.setting); });
  document.addEventListener('keydown', event => {
    if (event.target.id === 'shortcut-recorder') {
      if (event.key === 'Tab') return;
      event.preventDefault();
      if (['Control','Alt','Shift','Meta'].includes(event.key)) return;
      if (event.key === 'Escape') { closeDialog(); return; }
      const modifiers = [event.ctrlKey && 'Ctrl', event.altKey && 'Alt', event.shiftKey && 'Shift', event.metaKey && 'Win'].filter(Boolean);
      const named = { ' ': 'Space', ArrowLeft: 'Left', ArrowRight: 'Right', ArrowUp: 'Up', ArrowDown: 'Down' };
      const value = [...modifiers, named[event.key] || event.key].join('+');
      const normalized = normalizedShortcut(value);
      if (normalized) { $('#shortcut-text').value = normalized; $('#shortcut-recorder').innerHTML = keys(normalized); }
      return;
    }
    const tab = event.target.closest('[role=tab]');
    const radio = event.target.closest('[role=radio]');
    if ((tab || radio) && ['ArrowLeft','ArrowRight','Home','End'].includes(event.key)) {
      event.preventDefault(); const list = tab ? $$('[role=tab]', $('#page-tabs')) : $$('[role=radio]', radio.closest('[role=radiogroup]'));
      let index = list.indexOf(tab || radio); index = event.key === 'Home' ? 0 : event.key === 'End' ? list.length - 1 : (index + (event.key === 'ArrowRight' ? 1 : -1) + list.length) % list.length; list[index].click();
    }
    if (event.key === 'Escape' && !$('#dialog').open && !$('#settings-window').hidden) { event.preventDefault(); requestClose(); }
  });
  $('#dialog-form').addEventListener('submit', event => { event.preventDefault(); if (!dialogHandler || dialogHandler() !== false) closeDialog(); });
  $('#dialog-close').addEventListener('click', closeDialog);
  $('#dialog-cancel').addEventListener('click', closeDialog);
  $('#dialog').addEventListener('cancel', event => { event.preventDefault(); closeDialog(); });
  $('#save-settings').addEventListener('click', () => saveSettings());
  $('#cancel-settings').addEventListener('click', cancelSettings);
  $('#close-settings').addEventListener('click', requestClose);
  $('#about-button').addEventListener('click', () => { page = 'about'; renderPage(); $('#about-button').focus(); });
  $('#reopen-settings').addEventListener('click', () => { draft = clone(saved); $('#settings-window').hidden = false; $('#review-caption').hidden = false; $('#closed-state').hidden = true; renderPage(); $(`[data-page="${page}"]`)?.focus(); });
  $('#reset-demo').addEventListener('click', () => dialog({ title: '重置演示？', body: `<p>${'恢复原型的初始示例设置。仅影响本浏览器内的原型数据。'}</p>`, confirm: '重置演示', onConfirm: () => {
    saved = clone(defaults); draft = clone(defaults); samplePassword = ''; passwordConfigured = false; sampleHistoryCount = 128; ruleFilter = 'all'; page = 'appearance'; subpage = { launcher:'behavior', clipboard:'behavior', gestures:'rules' };
    try { localStorage.removeItem(STORAGE_KEY); storageAvailable = true; } catch { storageAvailable = false; }
    $('#settings-window').hidden = false; $('#closed-state').hidden = true; $('#review-caption').hidden = false; renderPage(); toast('已恢复初始示例'); return true;
  } }));
  systemTheme.addEventListener('change', () => { if (draft.theme === 'auto') applyTheme(); });
  renderPage();
})();
