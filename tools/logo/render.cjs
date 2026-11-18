// Renders an SVG or an HTML page to a PNG with the preinstalled Chromium (Playwright).
// node tools/logo/render.cjs <in.svg|in.html> <out.png> <width> <height> [scale]
const { chromium } = require('playwright');
const fs = require('fs'); const url = require('url');
(async () => {
  const [,, input, out, w, h, scale] = process.argv;
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: +w, height: +h }, deviceScaleFactor: +(scale || 1) });
  if (input.endsWith('.svg')) {
    const svg = fs.readFileSync(input, 'utf8');
    await page.setContent(`<html><body style="margin:0;background:transparent">${svg.replace('<svg ', `<svg style="width:${w}px;height:${h}px;display:block" `)}</body></html>`);
  } else {
    await page.goto(url.pathToFileURL(input).href);
    await page.waitForTimeout(300);
  }
  await page.screenshot({ path: out, omitBackground: true });
  await browser.close();
})();
