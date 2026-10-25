const { chromium } = require('playwright');
(async () => {
  const b = await chromium.launch(); const p = await b.newPage();
  for (const [src, out, w, h] of [['mask_a3.html','KeySampler_PCR-M30_Mask_A3.pdf','420mm','297mm'],['mask_a4.html','KeySampler_PCR-M30_Mask_2xA4.pdf','297mm','210mm']]) {
    await p.goto('file://' + process.cwd() + '/' + src, { waitUntil: 'load' });
    await p.pdf({ path: out, width: w, height: h, printBackground: true, margin: { top: 0, bottom: 0, left: 0, right: 0 } });
  }
  await b.close();
})();
