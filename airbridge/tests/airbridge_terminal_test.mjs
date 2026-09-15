import test from 'node:test';
import assert from 'node:assert/strict';
import { createTerminalState, resolveTerminalCommand, terminalCompletions, TERMINAL_COMMANDS } from '../web/airbridge-terminal.js';

test('short commands have fixed, unambiguous meanings', () => {
  const expected = { c: 'connect', d: 'disconnect', f: 'file', s: 'sendfile', uf: 'unfile', x: 'cancel', a: 'accept', ab: 'abort', l: 'logs', cl: 'clear', cll: 'clearlog', h: 'help', q: 'quit' };
  for (const [short, name] of Object.entries(expected)) {
    assert.equal(resolveTerminalCommand(short)?.name, name);
    assert.equal(resolveTerminalCommand(name)?.name, name);
  }
  const tokens = TERMINAL_COMMANDS.flatMap(command => [command.short, command.name, ...command.aliases]);
  assert.equal(new Set(tokens).size, tokens.length);
  for (const line of ['con', 'a;disconnect', 'connect now', '/connect', 'c\nd', ':)', '']) assert.equal(resolveTerminalCommand(line), null, line);
});

test('completion offers aliases and full names without changing their execution meaning', () => {
  assert.equal(resolveTerminalCommand('c').name, 'connect');
  assert.ok(terminalCompletions('c').some(command => command.name === 'cancel'));
  assert.equal(terminalCompletions('sf').length, 0);
  assert.deepEqual(terminalCompletions('uf').map(command => command.name), ['unfile']);
  assert.deepEqual(terminalCompletions('con').map(command => command.name), ['connect']);
});

test('mode transitions are explicit and returning to normal does not run a command', () => {
  const terminal = createTerminalState();
  assert.equal(terminal.mode, 'NORMAL');
  for (const mode of ['INSERT', 'NORMAL', 'COMMAND', 'NORMAL']) {
    terminal.enter(mode);
    assert.equal(terminal.mode, mode);
  }
  assert.throws(() => terminal.enter('connect'));
});

test('command history restores the unfinished command after traversing history', () => {
  const terminal = createTerminalState();
  terminal.enter('COMMAND');
  terminal.remember('c'); terminal.remember('a'); terminal.remember('a');
  assert.equal(terminal.recall(-1, 'sendf'), 'a');
  assert.equal(terminal.recall(-1, 'a'), 'c');
  assert.equal(terminal.recall(-1, 'c'), 'c');
  assert.equal(terminal.recall(1, 'c'), 'a');
  assert.equal(terminal.recall(1, 'a'), 'sendf');
  terminal.enter('NORMAL'); terminal.enter('COMMAND');
  assert.equal(terminal.recall(-1, ''), 'a');
});
