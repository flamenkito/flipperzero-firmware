// The terminal owns local input modes. Message text never enters the command parser.
export const TERMINAL_COMMANDS = Object.freeze([
  { name: 'connect', short: 'c', description: 'connect this endpoint' },
  { name: 'disconnect', short: 'd', description: 'disconnect; received downloads expire' },
  { name: 'file', short: 'f', description: 'choose an attachment', aliases: ['attach'] },
  { name: 'sendfile', short: 's', description: 'send the selected attachment' },
  { name: 'unfile', short: 'uf', description: 'remove the selected attachment' },
  { name: 'cancel', short: 'x', description: 'cancel the active transfer' },
  { name: 'accept', short: 'a', description: 'confirm the matching six-digit code' },
  { name: 'abort', short: 'ab', description: 'reject the code and end verification' },
  { name: 'logs', short: 'l', description: 'show or hide diagnostics' },
  { name: 'clear', short: 'cl', description: 'clear transcript and its downloads' },
  { name: 'clearlog', short: 'cll', description: 'clear diagnostics' },
  { name: 'help', short: 'h', description: 'open the help split' },
  { name: 'quit', short: 'q', description: 'close help or logs; keep the connection' },
].map(command => Object.freeze({ ...command, aliases: Object.freeze(command.aliases || []) })));

export function resolveTerminalCommand(line) {
  const token = line.trim().toLowerCase();
  return TERMINAL_COMMANDS.find(command => [command.name, command.short, ...command.aliases].includes(token)) ?? null;
}

export function terminalCompletions(prefix) {
  const token = prefix.toLowerCase();
  if (!/^[a-z]+$/.test(token)) return [];
  return TERMINAL_COMMANDS.filter(command => [command.name, command.short, ...command.aliases].some(name => name.startsWith(token)));
}

export function createTerminalState() {
  let mode = 'NORMAL';
  const history = [];
  let index = 0, scratch = '';
  return {
    get mode() { return mode; },
    enter(next) {
      if (!['NORMAL', 'INSERT', 'COMMAND'].includes(next)) throw new Error('Unknown terminal mode');
      mode = next;
      index = history.length;
    },
    remember(line) {
      if (line && line !== history.at(-1)) history.push(line);
      if (history.length > 50) history.shift();
      index = history.length;
    },
    recall(direction, current) {
      if (index === history.length) scratch = current;
      index = Math.max(0, Math.min(history.length, index + direction));
      return index === history.length ? scratch : history[index];
    },
  };
}

export function createTerminalConsole({ form, input, executeCommand, sendMessage }) {
  const doc = form.ownerDocument;
  const workspace = form.closest('.chat-workspace') || form;
  const transcript = doc.getElementById('transcript');
  const state = createTerminalState();
  const listeners = new AbortController();
  const listen = (target, event, callback) => target.addEventListener(event, callback, { signal: listeners.signal });
  const make = (tag, className, text = '') => {
    const element = doc.createElement(tag);
    element.className = className;
    element.textContent = text;
    return element;
  };
  const button = (className, text, label, action) => {
    const element = make('button', className, text);
    element.type = 'button';
    element.setAttribute('aria-label', label);
    listen(element, 'click', action);
    return element;
  };

  const outputWindow = make('section', 'terminal-output-window');
  outputWindow.hidden = true;
  const output = make('div', 'terminal-output');
  output.id = 'terminalOutput';
  output.setAttribute('role', 'region');
  output.setAttribute('aria-label', 'AirBridge help');
  output.tabIndex = 0;
  const outputStatus = make('div', 'split-statusline');
  outputStatus.append(make('span', '', 'airbridge-help [readonly]'), button('', ':q close', 'Close help', () => closeOutput()));
  outputWindow.append(output, outputStatus);

  const draft = make('div', 'draft-line');
  const draftNumber = make('span', 'draft-gutter', '1');
  draftNumber.setAttribute('aria-hidden', 'true');
  input.before(draft);
  draft.append(draftNumber, input);

  const statusline = make('div', 'terminal-statusline');
  statusline.id = 'terminalStatusline';
  statusline.setAttribute('aria-label', 'Buffer status');
  const modeLabel = button('terminal-mode', 'NORMAL', 'Enter insert mode', () => enterMode(state.mode === 'NORMAL' ? 'INSERT' : 'NORMAL'));
  modeLabel.id = 'terminalMode';
  const name = button('terminal-buffer-name', `airbridge://${doc.body.dataset.endpoint}`, 'Enter command mode', () => enterMode('COMMAND'));
  name.title = ': command';
  const modified = make('span', 'terminal-modified', '[+]');
  modified.setAttribute('aria-label', 'Unsent message draft');
  const ruler = make('span', 'terminal-ruler');
  ruler.id = 'terminalRuler';
  ruler.setAttribute('aria-label', 'Buffer position');
  statusline.append(modeLabel, name, modified, ruler);

  const commandline = make('div', 'terminal-commandline');
  const prompt = make('span', 'term-prompt', ':');
  prompt.setAttribute('aria-hidden', 'true');
  const commandInput = make('input', 'terminal-command-input');
  commandInput.id = 'commandInput';
  commandInput.type = 'text';
  commandInput.autocomplete = 'off';
  commandInput.spellcheck = false;
  commandInput.setAttribute('autocapitalize', 'off');
  commandInput.setAttribute('role', 'combobox');
  commandInput.setAttribute('aria-label', 'Ex command');
  commandInput.setAttribute('aria-autocomplete', 'list');
  commandInput.setAttribute('aria-controls', 'termSuggestions');
  const hint = make('span', 'terminal-hint');
  hint.id = 'terminalHint';
  const feedback = make('span', 'terminal-feedback');
  feedback.id = 'terminalFeedback';
  feedback.tabIndex = 0;
  feedback.setAttribute('role', 'status');
  feedback.setAttribute('aria-live', 'polite');
  const escapeButton = button('terminal-escape', 'Esc', 'Return to normal mode', () => enterMode('NORMAL'));
  commandline.append(prompt, commandInput, hint, feedback, escapeButton);
  commandInput.setAttribute('aria-describedby', hint.id);
  input.setAttribute('aria-describedby', hint.id);
  input.spellcheck = false;
  form.append(statusline);
  const logWindow = workspace.querySelector('.diagnostic-rail');
  if (logWindow) form.append(logWindow);
  form.append(outputWindow, commandline);

  const suggestions = make('div', 'term-suggestions');
  suggestions.id = 'termSuggestions';
  suggestions.setAttribute('role', 'listbox');
  suggestions.setAttribute('aria-label', 'Command completion');
  form.append(suggestions);
  let matches = [], selected = 0;
  let draftSelection = [0, 0, 'none'];
  let currentRow = null, pendingG = 0;

  function rows() { return Array.from(transcript.querySelectorAll('.message')); }

  function updateRuler() {
    const bufferRows = rows();
    if (!currentRow?.isConnected) currentRow = bufferRows.at(-1) ?? null;
    const index = bufferRows.indexOf(currentRow);
    for (const row of bufferRows) {
      row.classList.toggle('buffer-current', row === currentRow);
      if (row === currentRow) {
        row.dataset.cursorChar = row.querySelector('.bubble')?.textContent.charAt(0) || ' ';
        row.setAttribute('aria-current', 'true');
      } else row.removeAttribute('aria-current');
    }
    const count = bufferRows.filter(row => !row.classList.contains('empty')).length;
    draftNumber.textContent = String(count + 1);
    const all = transcript.scrollHeight <= transcript.clientHeight + 2;
    const end = transcript.scrollHeight - transcript.clientHeight - transcript.scrollTop <= 2;
    const position = all ? 'All' : end ? 'Bot' : transcript.scrollTop < 2 ? 'Top' : `${Math.round(100 * transcript.scrollTop / (transcript.scrollHeight - transcript.clientHeight))}%`;
    const column = state.mode === 'INSERT' ? (input.selectionStart || 0) + 1 : 1;
    ruler.textContent = `${state.mode === 'INSERT' ? count + 1 : index + 1},${column}  ${position}`;
    modified.hidden = !input.value;
    draft.hidden = state.mode !== 'INSERT' && !input.value;
  }

  function moveBuffer(direction) {
    const bufferRows = rows();
    const current = Math.max(0, bufferRows.indexOf(currentRow));
    const next = direction === 'first' ? 0 : direction === 'last' ? bufferRows.length - 1 : Math.max(0, Math.min(bufferRows.length - 1, current + direction));
    currentRow = bufferRows[next] ?? null;
    currentRow?.scrollIntoView({ block: 'nearest' });
    if (direction === 'first') transcript.scrollTop = 0;
    if (direction === 'last') transcript.scrollTop = transcript.scrollHeight;
    updateRuler();
  }

  function writeOutput(message, level = '') {
    feedback.className = `terminal-feedback ${level}`;
    feedback.textContent = message;
    feedback.hidden = false;
    if (state.mode !== 'COMMAND') hint.hidden = true;
  }

  function showOutput(lines) {
    output.replaceChildren(...lines.map(line => make('div', '', line || ' ')));
    outputWindow.hidden = false;
    output.scrollTop = 0;
  }

  function closeOutput(focus = true) {
    outputWindow.hidden = true;
    if (focus) enterMode('NORMAL');
  }

  function dismissSuggestions() {
    suggestions.hidden = true;
    commandInput.setAttribute('aria-expanded', 'false');
    commandInput.removeAttribute('aria-activedescendant');
  }

  function renderSuggestions() {
    if (!matches.length) { dismissSuggestions(); return; }
    suggestions.replaceChildren(...matches.map((command, index) => {
      const row = make('div', 'term-suggestion');
      row.id = `term-suggestion-${index}`;
      row.setAttribute('role', 'option');
      row.setAttribute('aria-selected', String(index === selected));
      row.append(make('span', 'term-suggestion-name', command.name));
      row.setAttribute('aria-label', `${command.name}: ${command.description}`);
      return row;
    }));
    suggestions.hidden = false;
    commandInput.setAttribute('aria-expanded', 'true');
    commandInput.setAttribute('aria-activedescendant', `term-suggestion-${selected}`);
    suggestions.children[selected]?.scrollIntoView({ block: 'nearest' });
  }

  function complete() {
    if (!matches.length) return;
    commandInput.value = matches[selected].name;
    commandInput.setSelectionRange(commandInput.value.length, commandInput.value.length);
    dismissSuggestions();
  }

  function enterMode(mode, focus = true) {
    if (state.mode === 'INSERT') draftSelection = [input.selectionStart, input.selectionEnd, input.selectionDirection];
    state.enter(mode);
    pendingG = 0;
    form.dataset.mode = workspace.dataset.mode = mode;
    modeLabel.textContent = mode;
    modeLabel.setAttribute('aria-label', mode === 'NORMAL' ? 'Enter insert mode' : 'Return to normal mode');
    input.readOnly = mode !== 'INSERT';
    if (mode === 'INSERT') feedback.textContent = '';
    commandInput.hidden = prompt.hidden = mode !== 'COMMAND';
    commandInput.value = '';
    feedback.hidden = mode === 'COMMAND' || !feedback.textContent;
    escapeButton.hidden = mode === 'NORMAL';
    if (mode === 'NORMAL') hint.textContent = '';
    else hint.textContent = mode === 'INSERT' ? '-- INSERT --  Enter sends' : 'Enter runs; Tab completes; Esc exits.';
    hint.hidden = mode === 'COMMAND' || !feedback.hidden;
    input.placeholder = mode === 'INSERT' ? 'Message to your peer' : '';
    dismissSuggestions();
    if (focus) {
      const target = mode === 'COMMAND' ? commandInput : mode === 'INSERT' ? input : !outputWindow.hidden ? output : doc.querySelector('.diagnostic-rail.logs-visible #log') || transcript;
      // Unhide the draft before moving focus into the native editing input.
      if (mode === 'INSERT') draft.hidden = false;
      target.focus({ preventScroll: true });
      if (mode === 'INSERT') input.setSelectionRange(...draftSelection);
    }
    updateRuler();
  }

  listen(input, 'click', () => {
    if (state.mode === 'NORMAL') {
      draftSelection = [input.selectionStart, input.selectionEnd, input.selectionDirection];
      enterMode('INSERT');
    }
  });
  listen(input, 'input', () => { feedback.textContent = ''; feedback.hidden = true; hint.hidden = false; updateRuler(); });
  listen(input, 'keyup', updateRuler);
  listen(input, 'select', updateRuler);
  listen(doc.defaultView, 'resize', updateRuler);
  listen(workspace, 'focusin', event => {
    if (event.target.closest('.terminal-output-window')) workspace.dataset.window = 'help';
    else if (event.target.closest('.diagnostic-rail')) workspace.dataset.window = 'log';
    else if (event.target === transcript || event.target === input) workspace.dataset.window = 'chat';
  });
  listen(transcript, 'scroll', updateRuler);
  listen(transcript, 'click', event => {
    if (state.mode !== 'NORMAL' || event.target.closest('a, button, summary')) return;
    currentRow = event.target.closest('.message') || currentRow;
    transcript.focus({ preventScroll: true });
    updateRuler();
  });
  listen(commandInput, 'input', () => {
    feedback.hidden = true;
    matches = terminalCompletions(commandInput.value);
    selected = 0;
    renderSuggestions();
  });
  listen(commandInput, 'blur', dismissSuggestions);
  listen(suggestions, 'mousedown', event => event.preventDefault());
  listen(suggestions, 'click', event => {
    const row = event.target.closest('[role="option"]');
    if (!row) return;
    selected = Array.from(suggestions.children).indexOf(row);
    complete();
    commandInput.focus();
  });
  listen(workspace, 'keydown', event => {
    if (event.isComposing || event.altKey || event.metaKey) return;
    if (event.key === 'Escape') { event.preventDefault(); enterMode('NORMAL'); return; }
    const inputFocused = event.target === input || event.target === commandInput;
    const reading = event.target === output || event.target === feedback || event.target === transcript || event.target.id === 'log' || event.target === workspace;
    if (!inputFocused && !reading) return;
    if (state.mode === 'NORMAL') {
      const scroller = event.target === output || event.target === feedback || event.target.id === 'log' ? event.target : transcript;
      if (event.ctrlKey) {
        if (event.key === 'd' || event.key === 'u') {
          event.preventDefault();
          scroller.scrollTop += scroller.clientHeight / 2 * (event.key === 'd' ? 1 : -1);
        }
        return;
      }
      if (event.key === 'Enter') event.preventDefault();
      if (event.key === 'i' || event.key === ':') {
        event.preventDefault();
        enterMode(event.key === 'i' ? 'INSERT' : 'COMMAND');
      } else if (['j', 'k', 'g', 'G', 'ArrowDown', 'ArrowUp'].includes(event.key)) {
        event.preventDefault();
        const first = event.key === 'g' && Date.now() - pendingG < 1000;
        if (event.key === 'g' && !first) { pendingG = Date.now(); return; }
        pendingG = 0;
        const direction = first ? 'first' : event.key === 'G' ? 'last' : ['j', 'ArrowDown'].includes(event.key) ? 1 : -1;
        if (scroller === transcript) moveBuffer(direction);
        else scroller.scrollTop = direction === 'first' ? 0 : direction === 'last' ? scroller.scrollHeight : scroller.scrollTop + direction * parseFloat(doc.defaultView.getComputedStyle(scroller).lineHeight);
      } else pendingG = 0;
      return;
    }
    if (!inputFocused || event.ctrlKey) return;
    if (state.mode === 'COMMAND') {
      if (event.key === 'Backspace' && !commandInput.value) { event.preventDefault(); enterMode('NORMAL'); return; }
      if (event.key === 'Tab' && !event.shiftKey && !suggestions.hidden) { event.preventDefault(); complete(); return; }
      if (event.key === 'ArrowUp' || event.key === 'ArrowDown') {
        event.preventDefault();
        const direction = event.key === 'ArrowUp' ? -1 : 1;
        if (!suggestions.hidden) { selected = (selected + direction + matches.length) % matches.length; renderSuggestions(); }
        else commandInput.value = state.recall(direction, commandInput.value);
        return;
      }
    }
    if (event.key === 'Enter') {
      event.preventDefault();
      if (!event.shiftKey) form.requestSubmit();
    }
  });
  listen(form, 'submit', event => {
    event.preventDefault();
    // Hidden legacy controls are also used by the protocol harness.
    if (event.submitter?.id === 'sendTextBtn' || state.mode === 'INSERT') { sendMessage(); return; }
    if (state.mode !== 'COMMAND') return;
    const line = commandInput.value.trim();
    if (!line) { enterMode('NORMAL'); return; }
    state.remember(line);
    const command = resolveTerminalCommand(line);
    if (!command) { writeOutput(`Unknown command: ${line}. Use :h for help.`, 'error'); dismissSuggestions(); return; }
    feedback.textContent = '';
    if (executeCommand(command.name) !== false) enterMode('NORMAL');
  });
  const observer = new MutationObserver(records => {
    // Ignore this module's own cursor attributes to avoid observer feedback.
    if (records.some(record => record.type === 'childList' || record.type === 'characterData')) updateRuler();
  });
  observer.observe(transcript, { childList: true, characterData: true, subtree: true });
  enterMode('NORMAL', doc.activeElement === doc.body);
  return {
    writeOutput, enterMode, showOutput, closeOutput,
    clearOutput: () => { feedback.textContent = ''; feedback.hidden = true; closeOutput(false); },
    refreshDraft: updateRuler,
    dispose: () => { listeners.abort(); observer.disconnect(); },
  };
}
