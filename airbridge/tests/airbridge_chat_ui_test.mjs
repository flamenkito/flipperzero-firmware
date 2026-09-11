import test from 'node:test';
import assert from 'node:assert/strict';
import { attachmentDisplayName, attachmentDownloadName } from '../web/airbridge-ui.js';

for (const [input, expected] of [
  ['../folder\\report\u202e.txt','report.txt'], ['normal.txt','normal.txt'],
  ['CON','attachment-CON'], ['LPT9.txt','attachment-LPT9.txt'], ['..','attachment'],
  ['','attachment'], ['file?.txt','file_.txt'], ['folder/file.txt. ','file.txt'],
  ['file\u0000\n.txt','file.txt'], ['画像.txt','画像.txt'],
]) {
  test(`safe download basename for ${JSON.stringify(input)}`, () => {
    // Given a name received as untrusted metadata.
    // When the browser download filename is derived.
    const actual=attachmentDownloadName(input);
    // Then paths, reserved names and control characters cannot affect the save location.
    assert.equal(actual,expected);
  });
}
test('display name retains path context independently of the download basename', () => {
  // Given a deceptive direction override inside a path.
  const input='../folder\\report\u202e.txt';
  // When a display name is produced.
  const actual=attachmentDisplayName(input);
  // Then human-readable path context survives without the direction control.
  assert.equal(actual,input);
});
