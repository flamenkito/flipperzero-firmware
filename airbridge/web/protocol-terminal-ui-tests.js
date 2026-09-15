import { loadChatTestPage } from './protocol-chat-pages-tests.js';

function check(value, message) { if (!value) throw new Error(message); }
function key(win, input, value) { input.dispatchEvent(new win.KeyboardEvent('keydown', { key: value, bubbles: true })); }
function type(win, input, value) { input.value = value; input.dispatchEvent(new win.Event('input', { bubbles: true })); }

export function terminalUiTests() {
  return ['usb', 'ble'].map(endpoint => [`Terminal ${endpoint}: modes, literal drafts, aliases, completion and visible feedback`, async () => {
    const page = await loadChatTestPage(endpoint, `terminal-${endpoint}-${Date.now()}`);
    const { win, iframe } = page;
    iframe.hidden = false;
    iframe.style.cssText = 'width:375px;height:900px;border:0';
    const doc = win.document, input = doc.getElementById('textInput'), command = doc.getElementById('commandInput');
    const buffer = doc.getElementById('transcript');
    const mode = () => doc.getElementById('terminalMode').textContent;
    const output = doc.getElementById('terminalOutput');
    const feedback = doc.getElementById('terminalFeedback');
    try {
      check(mode() === 'NORMAL' && input.readOnly, 'page did not start in NORMAL');
      check(input.closest('.draft-line').hidden, 'empty draft takes up a buffer line in NORMAL');
      check(win.getComputedStyle(doc.querySelector('.diagnostic-rail')).display === 'none', 'diagnostics clutter the default buffer');
      check(win.getComputedStyle(doc.querySelector('.console-chrome')).display === 'none', 'shell header is visible');
      key(win, buffer, 'i');
      type(win, input, '/path :help is literal chat');
      input.setSelectionRange(6, 11, 'backward');
      key(win, input, 'Escape');
      key(win, buffer, ':');
      check(mode() === 'COMMAND' && command.value === '', 'colon did not open separate command input');
      type(win, command, 'h'); key(win, command, 'Enter');
      check(mode() === 'NORMAL' && !output.parentElement.hidden && output.textContent.includes(':c'), 'short help command has no visible output');
      check(win.getComputedStyle(output).display !== 'none', 'command output is hidden by CSS');
      const chatStatus = doc.getElementById('terminalStatusline').getBoundingClientRect();
      check(chatStatus.bottom <= output.getBoundingClientRect().top + 1, 'chat statusline is below the help split');
      check(doc.querySelector('.chat-workspace').dataset.window === 'help', 'help is not the focused split');
      key(win, buffer, 'i');
      check(input.value === '/path :help is literal chat' && input.selectionStart === 6 && input.selectionEnd === 11 && input.selectionDirection === 'backward', 'command changed message draft or selection');
      check(win.getComputedStyle(input).caretColor !== 'rgba(0, 0, 0, 0)', 'native editing caret is hidden');
      key(win, input, 'Escape'); key(win, buffer, ':');
      type(win, command, 'c'); key(win, command, 'ArrowDown'); key(win, command, 'Tab');
      check(command.value === 'cancel', 'completion ignored the highlighted command');
      key(win, command, 'Escape');
      check(mode() === 'NORMAL' && input.value.startsWith('/path'), 'Escape executed a command or lost draft');
      key(win, buffer, ':');
      type(win, command, 'notacommand'); key(win, command, 'Enter');
      check(mode() === 'COMMAND' && !feedback.hidden && feedback.textContent.includes('Unknown command'), 'unknown command did not remain editable with visible error');
      key(win, command, 'Escape'); key(win, buffer, ':');
      key(win, command, 'ArrowUp');
      check(command.value === 'notacommand', 'command history unavailable');
      key(win, command, 'Escape');
      key(win, buffer, ':'); type(win, command, 'q'); key(win, command, 'Enter');
      check(output.parentElement.hidden && doc.activeElement === buffer, ':q did not return focus to the chat buffer');
      key(win, buffer, ':'); key(win, command, 'Backspace');
      check(mode() === 'NORMAL', 'Backspace on an empty command did not leave COMMAND');
      const transfer = new win.DataTransfer();
      transfer.items.add(new win.File(['staged'], 'staged.txt'));
      doc.getElementById('fileInput').files = transfer.files;
      doc.getElementById('fileInput').dispatchEvent(new win.Event('change'));
      check(!doc.getElementById('fileChip').hidden && !page.qa.transport(), 'choosing a file started a connection or transfer');
      check(win.getComputedStyle(doc.getElementById('fileChipClear')).display !== 'none', 'selected file cannot be removed');
      key(win, buffer, ':'); type(win, command, 'uf'); key(win, command, 'Enter');
      check(doc.getElementById('fileChip').hidden && input.value.startsWith('/path'), 'unfile did not preserve message draft');
      // Navigation changes the selected record and preserves the draft.
      buffer.replaceChildren(...Array.from({length: 35}, (_, index) => {
        const row = doc.createElement('article'); row.className = 'message peer';
        const text = doc.createElement('div'); text.className = 'bubble'; text.textContent = `record ${index}`;
        row.append(text); return row;
      }));
      await new Promise(resolve => win.setTimeout(resolve, 0));
      buffer.focus();
      key(win, buffer, 'g'); key(win, buffer, 'g');
      check(buffer.firstElementChild.getAttribute('aria-current') === 'true', 'gg did not select the first record');
      key(win, buffer, 'j');
      check(buffer.children[1].getAttribute('aria-current') === 'true', 'j did not move down');
      key(win, buffer, 'k');
      check(buffer.firstElementChild.getAttribute('aria-current') === 'true', 'k did not move up');
      key(win, buffer, 'G');
      check(buffer.lastElementChild.getAttribute('aria-current') === 'true' && doc.getElementById('terminalRuler').textContent.startsWith('35,1'), 'G/ruler did not reach the last record');
      check(input.value.startsWith('/path'), 'buffer navigation changed the message draft');
      const bubbleStyle = win.getComputedStyle(buffer.firstElementChild.querySelector('.bubble'));
      check(bubbleStyle.backgroundColor === 'rgba(0, 0, 0, 0)' && bubbleStyle.borderLeftWidth === '0px', 'message still renders as a bubble');
      const footer = doc.querySelector('.terminal-commandline').getBoundingClientRect();
      check(footer.bottom <= win.innerHeight && footer.bottom >= win.innerHeight - 1, 'command line is not the final screen row');
      const jump = doc.querySelector('.transcript-jump');
      jump.hidden = false;
      check(win.getComputedStyle(jump).display !== 'none', 'unread indicator is suppressed');
      check(doc.documentElement.scrollWidth <= win.innerWidth, 'terminal overflows narrow viewport');
      doc.body.classList.remove('mock');
      const panel = doc.getElementById('panelState');
      panel.dataset.phase = 'Failed';
      await new Promise(resolve => win.setTimeout(resolve, 0));
      check(win.getComputedStyle(panel).display !== 'none', 'failure phase disappears outside mock mode');
    } finally { page.qa?.dispose(); iframe.remove(); }
  }]);
}
