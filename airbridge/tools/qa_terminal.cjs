#!/usr/bin/env node
// Run with Playwright installed (or NODE_PATH pointing at its node_modules).
// Start the canonical 8081 server and persistent CDP Chrome from AGENTS.md first.
// This script attaches to that browser; it never creates an ephemeral browser.
const { chromium } = require('playwright');
const assert = require('node:assert/strict');
const { createHash } = require('node:crypto');
const fs = require('node:fs/promises');

async function main() {
  const browser = await chromium.connectOverCDP(process.env.AIRBRIDGE_CDP_URL || 'http://localhost:9222');
  const context = browser.contexts()[0];
  const evidence = `/private/tmp/airbridge-vim-qa-${Date.now()}`;
  await fs.mkdir(evidence, { recursive: true });
  const room = `vim-qa-${Date.now()}`;
  const errors = [];
  const pages = [];
  const command = async (page, value) => {
    await page.keyboard.press('Escape');
    await page.locator('#transcript').press(':');
    await page.locator('#commandInput').fill(value);
    await page.locator('#commandInput').press('Enter');
  };
  const waitText = (page, id, text) => page.waitForFunction(({ id, text }) => document.getElementById(id).textContent.includes(text), { id, text });
  for (const endpoint of ['usb', 'ble']) {
    const page = await context.newPage();
    pages.push(page);
    page.on('pageerror', error => errors.push(`${endpoint}: ${error.message}`));
    await page.setViewportSize({ width: 1360, height: 900 });
    await page.goto(`http://127.0.0.1:8081/chat-${endpoint}.html?mock=1&mockPeer=${room}`);
    await waitText(page, 'terminalMode', 'NORMAL');
    await page.screenshot({ path: `${evidence}/${endpoint}-normal.png` });
    await command(page, 'h');
    assert.equal(await page.locator('#terminalOutput').isVisible(), true);
    await page.screenshot({ path: `${evidence}/${endpoint}-help.png` });
    await command(page, 'q');
    assert.equal(await page.locator('#terminalOutput').isVisible(), false);
    await page.locator('#transcript').press('i');
    await page.locator('#textInput').fill('draft /path :help');
    await page.locator('#textInput').evaluate(input => input.setSelectionRange(2, 5, 'backward'));
    await command(page, 'h');
    await page.locator('#transcript').press('i');
    assert.deepEqual(await page.locator('#textInput').evaluate(input => [input.value, input.selectionStart, input.selectionEnd]), ['draft /path :help', 2, 5]);
    await command(page, 'q');
    await command(page, 'c');
  }
  await Promise.all(pages.map(page => page.waitForFunction(() => /^\d{6}$/.test(document.getElementById('sasCode').textContent))));
  assert.equal(await pages[0].locator('#sasCode').textContent(), await pages[1].locator('#sasCode').textContent());
  for (const page of pages) {
    const code = await page.locator('#sasCode').textContent();
    assert.equal(await page.locator('.prompt-context').isVisible(), true);
    assert.ok((await page.locator('.prompt-context').textContent()).includes(`SAS ${code} pending`));
    assert.equal(await page.locator('#terminalFeedback').isVisible(), true);
    assert.equal(await page.locator('#terminalFeedback').textContent(), `verify peer: ${code}`);
  }
  await pages[0].screenshot({ path: `${evidence}/usb-code-confirmation.png` });
  await pages[0].setViewportSize({ width: 375, height: 844 });
  await pages[0].screenshot({ path: `${evidence}/usb-code-confirmation-narrow.png` });
  assert.equal(await pages[0].locator('#terminalFeedback').isVisible(), true);
  await pages[0].setViewportSize({ width: 1360, height: 900 });
  for (const page of pages) await command(page, 'a');
  await Promise.all(pages.map(page => waitText(page, 'cryptoState', 'verified')));

  for (let index = 0; index < pages.length; index++) {
    const page = pages[index], peer = pages[1 - index];
    const bytes = Buffer.from(`verified attachment ${index}\n`);
    await page.locator('#fileInput').setInputFiles({ name: `receipt-${index}.txt`, mimeType: 'text/plain', buffer: bytes });
    assert.equal(await page.locator('#fileChip').isVisible(), true);
    assert.equal(await page.locator('#panelState').getAttribute('data-phase'), 'Ready');
    await page.locator('#transcript').press('i');
    await page.locator('#textInput').fill(`/path :connect literal message ${index}`);
    await page.locator('#textInput').press('Enter');
    await waitText(peer, 'transcript', `/path :connect literal message ${index}`);
    assert.equal(await page.locator('#fileChip').isVisible(), true);
    await command(page, 's');
    const download = peer.locator(`.peer a[download="receipt-${index}.txt"]`);
    await download.waitFor();
    const received = await download.evaluate(async link => Array.from(new Uint8Array(await (await fetch(link.href)).arrayBuffer())));
    assert.equal(createHash('sha256').update(Buffer.from(received)).digest('hex'), createHash('sha256').update(bytes).digest('hex'));
    await command(page, 'q');
    assert.equal(await page.locator('#cryptoState').textContent(), 'verified');
    await page.locator('#transcript').press('g');
    await page.locator('#transcript').press('g');
    await page.locator('#transcript').press('j');
    await page.locator('#transcript').press('G');
    assert.equal(await page.locator('#transcript .message:last-child').getAttribute('aria-current'), 'true');
    await page.screenshot({ path: `${evidence}/${index}-verified-chat.png` });
  }

  for (const page of pages) {
    await page.keyboard.press('Escape');
    await page.locator('#transcript').press(':');
    await page.locator('#commandInput').fill('c');
    await page.locator('#commandInput').press('ArrowDown');
    await page.locator('#commandInput').press('Tab');
    assert.equal(await page.locator('#commandInput').inputValue(), 'cancel');
    await page.keyboard.press('Escape');
    for (const [width, height] of [[375, 900], [844, 390]]) {
      await page.setViewportSize({ width, height });
      await page.locator('#transcript').press('i');
      const fits = await page.evaluate(() => {
        const input = document.getElementById('textInput').getBoundingClientRect();
        return document.documentElement.scrollWidth <= innerWidth && input.top >= 0 && input.bottom <= innerHeight;
      });
      assert.equal(fits, true, 'composer must remain visible without horizontal overflow');
      await page.screenshot({ path: `${evidence}/${await page.locator('body').getAttribute('data-endpoint')}-${width}.png` });
      await page.keyboard.press('Escape');
    }
  }

  const harness = await context.newPage();
  await harness.goto('http://127.0.0.1:8081/protocol-harness.html');
  await harness.waitForFunction(() => typeof window.runAllProtocolTests === 'function');
  const outcomes = await harness.evaluate(() => window.runAllProtocolTests());
  await fs.writeFile(`${evidence}/protocol-results.json`, JSON.stringify(outcomes, null, 2));
  assert.deepEqual(outcomes.filter(result => !result.ok), [], 'protocol harness failures');
  assert.deepEqual(errors, [], 'browser JavaScript errors');
  console.log(JSON.stringify({ passed: true, protocolTests: outcomes.length, evidence }));
  // Keep the tabs and persistent browser available for inspection.
}

main().then(() => process.exit(0), error => { console.error(error); process.exit(1); });
