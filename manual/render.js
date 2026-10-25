const { chromium } = require('playwright');
(async () => {
  const b = await chromium.launch();
  const p = await b.newPage();
  await p.goto('file://' + process.argv[2], { waitUntil: 'load' });
  await p.pdf({ path: process.argv[3], format: 'A4', printBackground: true, preferCSSPageSize: true,
    displayHeaderFooter: true, headerTemplate: '<div></div>',
    footerTemplate: '<div style="width:100%;font-family:Inter,sans-serif;font-size:7.5pt;color:#5a5f69;padding:0 17mm;display:flex;justify-content:space-between"><span>KeySampler for the S2400 DSP Card</span><span><span class="pageNumber"></span> / <span class="totalPages"></span></span></div>',
    margin: { top: '18mm', bottom: '20mm', left: '17mm', right: '17mm' } });
  await b.close();
})();
