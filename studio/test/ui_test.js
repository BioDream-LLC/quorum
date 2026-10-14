// Browser test of the studio page. Start `dist/quorum-studio --no-browser > /tmp/studio.log`, then:
//   PLAYWRIGHT=<path to the playwright package> node test/ui_test.js
const { chromium } = require(process.env.PLAYWRIGHT || 'playwright');
const fs = require('fs');
const url = fs.readFileSync('/tmp/studio.log', 'utf8').match(/http:\/\/127\.0\.0\.1:\d+\/\?t=[0-9a-f]+/)[0];
const ok = (c, m) => { console.log((c ? 'PASS ' : 'FAIL ') + m); if (!c) process.exitCode = 1; };
(async () => {
  const b = await chromium.launch(); const p = await b.newPage({ viewport: { width: 1500, height: 1100 } });
  const errs = []; p.on('pageerror', e => errs.push(e.message)); p.on('dialog', d => d.accept());
  await p.goto(url);
  await p.selectOption('#examples', 'pump.cb'); await p.waitForFunction(() => document.getElementById('src').value.length > 100);
  ok(await p.locator('#hl .k-observe').count() > 5, 'keywords coloured');
  const lines = await p.evaluate(() => document.getElementById('src').value.split('\n').length);
  ok(await p.evaluate(n => document.getElementById('gutter').textContent.trim().split('\n').length === n, lines), `line numbers (${lines})`);
  await p.click('#src'); await p.keyboard.press('Control+f'); await p.keyboard.type('vib_high');
  ok(await p.inputValue('#find') === 'vib_high' && !(await p.inputValue('#src')).includes('ib_highvib'), 'typing in find stays in the find box');
  ok(await p.evaluate(() => document.activeElement.id) === 'find', 'focus stays in the find box');
  const cnt = await p.textContent('#fcount'); ok(/^\d+ of 7$/.test(cnt), 'find counts matches: ' + cnt);
  await p.waitForSelector('#hl .m-cur'); ok(await p.locator('#hl .m-cur').count() === 1 && await p.locator('#hl .m-find').count() === 6, 'current match and the other 6 highlighted');
  await p.fill('#repl', 'vibration_high'); await p.click('#fall');
  ok(!(await p.inputValue('#src')).includes('vib_high'), 'replace all: ' + await p.textContent('#fcount'));
  await p.keyboard.press('Escape');
  await p.click('#check'); await p.waitForSelector('#verdict.ok', { timeout: 60000 });
  ok(true, 'consistently renamed casebook still passes');
  await p.screenshot({ path: '/tmp/qs/1-pass.png', fullPage: true });
  // break it on a known line
  await p.evaluate(() => { const s = document.getElementById('src'); s.focus(); s.setSelectionRange(s.value.length, s.value.length); });
  await p.keyboard.type('\nclaim bogus  needs no_such_observation\n');
  await p.click('#check'); await p.waitForSelector('#verdict.no', { timeout: 60000 });
  const ebtn = p.locator('#out button.e').first(); ok(await ebtn.count() === 1, 'error is a clickable row');
  const eline = parseInt((await ebtn.locator('b').textContent()).replace('line ', ''), 10);
  ok(await p.locator('#gutter .err').count() >= 1, 'error line marked red in gutter');
  await p.click('#goto'); await p.fill('#goto', '1'); await p.press('#goto', 'Enter');
  await ebtn.click(); const pos = await p.textContent('#pos');
  ok(pos.startsWith('line ' + eline + ','), `jump to error line ${eline}: ${pos}`);
  await p.screenshot({ path: '/tmp/qs/2-error.png', fullPage: true });
  const drift = await p.evaluate(() => { const s = document.getElementById('src'), g = document.getElementById('gutter'), h = document.getElementById('hl');
    s.scrollTop = 1e6; s.dispatchEvent(new Event('scroll')); return [s.scrollTop - g.scrollTop, s.scrollTop - h.scrollTop]; });
  ok(drift[0] === 0 && drift[1] === 0, 'gutter and colouring stay aligned when scrolled to the bottom: ' + drift);
  // worksheet
  await p.setInputFiles('#wsfile', require('path').join(__dirname, '../../casebook/fmea/hazop_worksheet.csv'));
  await p.waitForSelector('#sheetsec', { state: 'visible' }); await p.waitForFunction(() => document.getElementById('todos').children.length > 0);
  const todos = await p.locator('#todos button').count(); ok(todos > 0, `TODO list: ${todos} items`);
  await p.locator('#todos button').nth(1).click();
  ok(await p.evaluate(() => { const s = document.getElementById('src'); return s.value.slice(s.selectionStart, s.selectionEnd); }) === 'TODO', 'click a TODO selects it');
  const rows = await p.locator('#sheet tr').count(); ok(rows > 5, `worksheet grid: ${rows - 1} rows`);
  await p.fill('#sheetfind', 'LT-101'); ok(/^\d+ matching/.test(await p.textContent('#sheetcount')), 'worksheet search: ' + await p.textContent('#sheetcount'));
  await p.fill('#sheetfind', '');
  const cell = p.locator('#sheet tr').nth(4).locator('td').nth(2);
  const before = await cell.textContent(); await cell.click(); await p.keyboard.press('End'); await p.keyboard.type(' EDITED');
  ok(await p.locator('#sheet tr.edited').count() === 1, 'edited row marked');
  const cols = await p.locator('#sheet tr').first().locator('th').count();
  await p.click('#addcol'); ok(await p.locator('#sheet tr').first().locator('th').count() === cols + 1, 'add column');
  await p.locator('#sheet tr').first().locator('th').nth(cols - 1).locator('button').click();
  ok(await p.locator('#sheet tr').first().locator('th').count() === cols, 'delete column');
  await p.click('#sundo'); ok(await p.locator('#sheet tr').first().locator('th').count() === cols + 1, 'undo restores the deleted column');
  await p.click('#sredo'); ok(await p.locator('#sheet tr').first().locator('th').count() === cols, 'redo deletes it again');
  await p.click('#sundo'); await p.click('#sundo');
  ok(await p.locator('#sheet tr').first().locator('th').count() === cols && (await p.locator('#sheet tr').nth(4).locator('td').nth(2).textContent()).includes('EDITED'), 'two undos: column gone, cell edit kept');
  await p.click('#sundo');
  ok(!(await p.locator('#sheet tr').nth(4).locator('td').nth(2).textContent()).includes('EDITED') && await p.locator('#sheet tr.edited').count() === 0, 'third undo reverts the cell edit and its marker');
  await p.click('#sredo');
  await p.click('#reimport'); await p.waitForFunction(() => document.getElementById('loaded').textContent.startsWith('Redrafted'));
  ok((await p.inputValue('#src')).includes('EDITED'), 're-import carries the cell edit into the draft');
  await p.screenshot({ path: '/tmp/qs/3-sheet.png', fullPage: true });
  // two casebooks on one chip
  await p.selectOption('#examples', 'pump.cb'); await p.waitForFunction(() => document.getElementById('loaded').textContent === 'Example: pump.cb');
  await p.selectOption('#extraex', 'tank.cb'); await p.waitForSelector('#extras button');
  await p.click('#check'); await p.waitForSelector('#verdict.ok', { timeout: 60000 });
  const sw = await p.textContent('#sweep');
  ok(sw.includes('DEVICE centrifugal-pump states 16384 asserts 1024 fnv1a 0xe5408b85') && sw.includes('DEVICE tank-409 states 32768 asserts 7680 fnv1a 0x90bc4385'), 'palette: both members, each as alone');
  await p.click('#extras button'); ok(await p.textContent('#extras') === 'nothing else', 'remove a casebook from the chip');
  // learning from records
  await p.setInputFiles('#recfile', require('path').join(__dirname, '../../learn/data/ai4i2020.csv'));
  await p.waitForSelector('#learnbox', { state: 'visible' });
  ok(await p.inputValue('#outcol') === 'Machine failure', 'outcome column guessed: ' + await p.inputValue('#outcol'));
  ok(await p.inputValue('#posval') === '1', 'rarer value offered first as "it happened"');
  await p.click('#learn'); await p.waitForFunction(() => document.getElementById('loaded').textContent.startsWith('Learned'));
  ok(/TWF is 1/.test(await p.textContent('#findtext')), 'learning report flags after-the-event columns');
  for (const c of ['TWF', 'HDF', 'PWF', 'OSF', 'RNF']) await p.check(`#ignorelist input[value="${c}"]`);
  await p.click('#learn'); await p.waitForFunction(() => /^Learned/.test(document.getElementById('loaded').textContent) && !/WARNING/.test(document.getElementById('findtext').textContent));
  await p.click('#check'); await p.waitForSelector('#verdict.ok', { timeout: 60000 });
  ok(true, 'leak-free learned rulebook passes the check and yields an image');
  await p.screenshot({ path: '/tmp/qs-learn.png', fullPage: true });
  // drafting from a manual, against a fake model on this computer that gives one small draft and then nothing more
  const http = require('http');
  const q1 = 'If the thermal switch has opened, the motor is overheated and the pump must be stopped.';
  const q2 = 'If the motor current relay is closed and the discharge flow switch is not closed, the pump is running dry or the discharge is blocked.';
  const q3 = 'The discharge flow switch closes when water is moving in the discharge pipe.';
  const first = JSON.stringify({ items: [
    { kind: 'observe', name: 'thermal_open', description: 'motor winding overheated', quote: q1 },
    { kind: 'observe', name: 'current_on', description: 'motor drawing current', quote: q2 },
    { kind: 'observe', name: 'flow_on', description: 'water moving in the discharge pipe', quote: q3 },
    { kind: 'claim', name: 'overheated', description: 'motor overheated', severity: 8, needs: ['thermal_open'], quote: q1 },
    { kind: 'claim', name: 'dry_run', description: 'running dry or discharge blocked', severity: 7, needs: ['current_on'], quote: q2 },
    { kind: 'attack', from: 'flow_on', attack: 'rebuts', to: ['dry_run'], quote: q2 },
    { kind: 'action', name: 'keep_running', role: 'idle', quote: q1 },
    { kind: 'action', name: 'stop_pump', role: 'safe', when: ['overheated', 'dry_run'], quote: q1 },
    { kind: 'prefer', order: ['stop_pump', 'keep_running'], quote: q1 }] });
  let calls = 0;
  const fake = http.createServer((req, res) => { req.resume(); req.on('end', () => { calls++;
    res.setHeader('Content-Type', 'application/json');
    res.end(JSON.stringify({ response: calls === 1 ? first : '{"items":[]}' })); }); });
  await new Promise(r => fake.listen(0, '127.0.0.1', r));
  await p.setInputFiles('#manfile', require('path').join(__dirname, '../../casebook/fixtures/manual/sump_pump_manual.txt'));
  await p.waitForSelector('#draftbox', { state: 'visible' });
  await p.selectOption('#mserver', 'hosted');
  ok(await p.isVisible('#mkey') && await p.isVisible('#mremote'), 'hosted service asks for a key and for permission');
  await p.click('#draft');
  ok(/Tick the box/.test(await p.textContent('#loaded')), 'hosted drafting refused until the user allows sending the manual');
  await p.selectOption('#mserver', 'local');
  await p.fill('#mendpoint', `http://127.0.0.1:${fake.address().port}`); await p.fill('#mmodel', 'fake');
  await p.click('#draft');
  await p.waitForFunction(() => /^Drafted/.test(document.getElementById('loaded').textContent), null, { timeout: 120000 });
  ok((await p.inputValue('#src')).includes('UNAPPROVED DRAFT'), 'the draft is loaded and marked unapproved');
  ok(/REVIEW/.test(await p.textContent('#findtext')), 'the draft review is shown');
  ok(/sentence pass/.test(await p.textContent('#draftlog')), 'progress is shown while drafting');
  ok(await p.isVisible('#approvebox'), 'the approval box appears for an unapproved draft');
  await p.click('#check'); await p.waitForFunction(() => /UNAPPROVED DRAFT/.test(document.getElementById('out').textContent), null, { timeout: 60000 });
  ok(!(await p.isVisible('#verdict.ok')), 'an unapproved draft gets no chip image');
  await p.click('#approve'); ok(/Enter your name/.test(await p.textContent('#loaded')), 'approval needs a name');
  await p.fill('#approver', 'A. Engineer'); await p.click('#approve');
  await p.waitForFunction(() => /^Approved/.test(document.getElementById('loaded').textContent));
  ok(!(await p.inputValue('#src')).includes('UNAPPROVED') && !(await p.isVisible('#approvebox')), 'approval removes the marker and the box');
  await p.click('#check'); await p.waitForSelector('#verdict.ok', { timeout: 60000 });
  ok(true, 'the approved draft passes the full check and yields an image');
  await p.screenshot({ path: '/tmp/qs-draft.png', fullPage: true });
  fake.close();
  ok(errs.length === 0, 'no page errors ' + errs.join(' | '));
  await b.close();
})().catch(e => { console.log('FAIL ' + e.message); process.exitCode = 1; });
