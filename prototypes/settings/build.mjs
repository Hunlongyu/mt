import { readFile, writeFile, mkdir } from 'node:fs/promises';
import { createHash } from 'node:crypto';
const root = new URL('./', import.meta.url);
const read = path => readFile(new URL(path, root), 'utf8');
const source = JSON.parse(await read('../../resources/theme/theme.json'));
function color(binding, colors) {
  const key = typeof binding === 'string' ? binding : binding.source;
  const value = colors[key];
  if (!/^#[\da-f]{6}([\da-f]{2})?$/i.test(value || '')) throw Error(`Invalid theme color: ${key}`);
  if (typeof binding === 'string' || value.length === 7) return value;
  const base = colors[binding.over];
  if (!/^#[\da-f]{6}$/i.test(base || '')) throw Error(`Invalid base color: ${binding.over}`);
  const alpha = parseInt(value.slice(7), 16) / 255;
  return '#' + [1, 3, 5].map(i => Math.round(parseInt(value.slice(i, i + 2), 16) * alpha + parseInt(base.slice(i, i + 2), 16) * (1 - alpha)).toString(16).padStart(2, '0')).join('');
}
const palette = Object.entries(source.themes).map(([mode, theme]) => {
  const colors = { ...theme.colors, ...theme.adaptations };
  const rules = Object.entries(source.bindings).map(([key, binding]) => `--${key}:${color(binding, colors)};`);
  for (const [key, value] of Object.entries(theme.product_colors)) rules.push(`--${key}:${value};`);
  const selector = mode === 'light' ? ':root,.sample-light' : ':root[data-theme="dark"],.sample-dark';
  return `${selector}{color-scheme:${mode};${rules.join('')}}`;
}).join('\n');
const iconNames = ['settings','palette','layout-grid','copy','mouse-pointer-2','monitor-off','info','x','check','sun','moon','monitor','chevron-down','chevron-right','arrow-left','arrow-right','arrow-up','arrow-down','arrow-down-left','arrow-down-right','arrow-up-left','arrow-up-right','rotate-ccw','plus','pencil','trash-2','folder','upload','keyboard','hard-drive','power','external-link','shield-check'];
const icons = {};
const manifest = [];
for (const name of iconNames) {
  const svg = await read(`../../resources/icons/lucide/${name}.svg`);
  if (!svg.includes('<svg') || !svg.includes('lucide')) throw Error(`Invalid Lucide asset: ${name}`);
  icons[name] = svg.slice(svg.indexOf('<svg')).replace('<svg ', '<svg aria-hidden="true" focusable="false" ');
  manifest.push({ name, source: `https://unpkg.com/lucide-static@0.460.0/icons/${name}.svg`, sha256: createHash('sha256').update(svg).digest('hex') });
}
let html = await read('template.html');
const inserts = {
  THEME: palette + '\n' + await read('../desktop/src/theme.css'),
  STYLES: await read('src/styles.css'),
  ICONS: `window.MT_ICONS=${JSON.stringify(icons)};window.MT_LICENSE=${JSON.stringify(await read('../../resources/icons/lucide/LICENSE.txt'))};`,
  APP: await read('src/app.js')
};
for (const [name, content] of Object.entries(inserts)) html = html.replace(`/*__${name}__*/`, () => content);
if (/\/\*__[A-Z_]+__\*\//.test(html)) throw Error('Unresolved template marker');
await writeFile(new URL('index.html', root), html);
await mkdir(new URL('assets/', root), { recursive: true });
await writeFile(new URL('assets/icon-manifest.json', root), JSON.stringify(manifest, null, 2) + '\n');
console.log(`Built offline settings prototype (${Buffer.byteLength(html)} bytes)`);
