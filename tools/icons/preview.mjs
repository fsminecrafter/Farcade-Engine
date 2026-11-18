// Renders every icon in editor/icons/ to preview-dark/light PNGs (1x and @2x).
// Usage: node tools/icons/preview.mjs   (needs the `playwright` package and Chromium)
import { createRequire } from 'node:module';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
const require = createRequire(import.meta.url);
const { chromium } = require('playwright');

const here = dirname(fileURLToPath(import.meta.url));
const iconDir = join(here, '..', '..', 'editor', 'icons');
const template = readFileSync(join(here, 'preview.html'), 'utf8');
const manifest = JSON.parse(readFileSync(join(iconDir, 'manifest.json'), 'utf8'));

const ICONS = {};
const GROUPS = manifest.map(g => [g.title, g.icons.map(i => i.name)]);
for (const g of manifest) for (const i of g.icons)
  ICONS[i.name] = readFileSync(join(iconDir, i.name + '.svg'), 'utf8').trim();
// rows shown in the accent "selected" color, to judge tinting
const SELECTED = ['pointer', 'move', 'eye', 'folder'];

const browser = await chromium.launch();
for (const theme of ['dark', 'light']) {
  for (const scale of [1, 2]) {
    const ctx = await browser.newContext({ viewport: { width: 1400, height: 900 }, deviceScaleFactor: scale });
    const page = await ctx.newPage();
    const data = JSON.stringify({ ICONS, GROUPS, SELECTED, THEME: theme }).replace(/</g, '\\u003c');
    await page.setContent(template.replace('<script>', `<script>window.__DATA__ = ${data};`));
    const out = join(iconDir, `preview-${theme}${scale === 2 ? '@2x' : ''}.png`);
    await page.screenshot({ path: out, fullPage: true });
    console.log('wrote', out);
    await ctx.close();
  }
}
await browser.close();
