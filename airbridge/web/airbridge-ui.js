// airbridge-ui.js — shared UI helpers for chat-usb.html and chat-ble.html.
// Extracted from chat-usb.html; reproduces its DOM structures and class names
// exactly. Zero dependencies, no DOM access at module top level, no emoji.
// build_bundle.py inlines this file: declaration exports only, no import/export lists.

export function formatBytes(size) {
  const n = Number(size) || 0;
  if (n < 1024) return `${n} B`;
  return `${(n / 1024).toFixed(n < 10 * 1024 ? 1 : 0)} KB`;
}

export function shortHash(hash) {
  const normalized = String(hash || '').replace(/^sha256:/, '');
  return normalized ? `${normalized.slice(0, 12)}…` : 'unknown';
}

export function attachmentDisplayName(value) {
  return String(value || 'attachment').replace(/[\u0000-\u001f\u007f-\u009f]/g, '').trim() || 'attachment';
}

export function attachmentDownloadName(value) {
  const name = attachmentDisplayName(value).replace(/[\u202a-\u202e\u2066-\u2069]/g, '').split(/[\\/]/).pop()
    .replace(/[<>:"|?*]/g, '_').replace(/^[. ]+|[. ]+$/g, '');
  return !name || /^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)/i.test(name)
    ? `attachment${name ? `-${name}` : ''}` : name;
}

const asciiWidthCaches = new WeakMap();
const asciiResizeWindows = new WeakSet();
const asciiFontReadyDocuments = new WeakSet();

export function fillAsciiProgress(container, fraction) {
  const fill = container.querySelector('.ab-progress-fill');
  if (!fill) return '';
  const doc = container.ownerDocument;
  const view = doc.defaultView;
  if (doc.fonts && !asciiFontReadyDocuments.has(doc)) {
    asciiFontReadyDocuments.add(doc);
    doc.fonts.ready.then(() => {
      asciiWidthCaches.delete(doc);
      for (const bar of doc.querySelectorAll('.ab-progress')) {
        fillAsciiProgress(bar, Number(bar.dataset.progressFraction || '0'));
      }
    });
  }
  if (view && !asciiResizeWindows.has(view)) {
    asciiResizeWindows.add(view);
    view.addEventListener('resize', () => {
      asciiWidthCaches.delete(doc);
      for (const bar of doc.querySelectorAll('.ab-progress')) {
        fillAsciiProgress(bar, Number(bar.dataset.progressFraction || '0'));
      }
    });
  }
  let cache = asciiWidthCaches.get(doc);
  if (!cache) { cache = new Map(); asciiWidthCaches.set(doc, cache); }
  let charWidth = cache.get('ab-progress-fill');
  if (!charWidth) {
    const probe = doc.createElement('span');
    probe.className = 'ab-progress-fill';
    probe.textContent = '#';
    probe.style.cssText = 'position:absolute;visibility:hidden;display:inline-block;inline-size:auto;overflow:visible;';
    doc.body.appendChild(probe);
    charWidth = probe.getBoundingClientRect().width;
    probe.remove();
    if (charWidth > 0 && (!doc.fonts || doc.fonts.status === 'loaded')) cache.set('ab-progress-fill', charWidth);
  }
  const safe = Math.max(0, Math.min(1, Number(fraction) || 0));
  container.dataset.progressFraction = String(safe);
  const style = view?.getComputedStyle(container);
  const contentWidth = container.clientWidth - (parseFloat(style?.paddingInlineStart || '0') + parseFloat(style?.paddingInlineEnd || '0'));
  fill.textContent = charWidth > 0 ? '#'.repeat(Math.max(0, Math.floor(contentWidth / charWidth * safe))) : '';
  return fill.textContent;
}

function createAsciiProgress(doc) {
  const group = doc.createElement('div');
  group.className = 'ab-progress-ui';
  const label = doc.createElement('div');
  label.className = 'ab-progress-label';
  const caption = doc.createElement('span');
  caption.className = 'ab-progress-caption';
  const value = doc.createElement('span');
  value.className = 'ab-progress-value';
  label.append(caption, value);
  const bar = doc.createElement('div');
  bar.className = 'ab-progress';
  bar.setAttribute('aria-hidden', 'true');
  const fill = doc.createElement('span');
  fill.className = 'ab-progress-fill';
  bar.appendChild(fill);
  group.append(label, bar);
  return group;
}

function updateAsciiProgress(group, phase, percent, visible = true) {
  const safe = Math.max(0, Math.min(100, Number(percent) || 0));
  group.hidden = !visible;
  group.querySelector('.ab-progress-caption').textContent = `${String(phase).toUpperCase()}:`;
  group.querySelector('.ab-progress-value').textContent = `${Number.isInteger(safe) ? safe : safe.toFixed(2).replace(/0+$/, '').replace(/\.$/, '')}%`;
  fillAsciiProgress(group.querySelector('.ab-progress'), safe / 100);
}

export function setTransferPhase(phase, progress = {}) {
  const panel = document.getElementById('panelState');
  const meter = document.getElementById('throughput');
  const same = !progress.reset && panel.dataset.phase === phase;
  const previous = BigInt(meter.dataset.bytes || '0');
  const bytes = progress.bytes == null ? previous : same && progress.bytes < previous ? previous : progress.bytes;
  if (progress.reset) meter.dataset.progress = '0';
  panel.dataset.phase = phase;
  panel.textContent = phase;
  panel.setAttribute('role', 'status');
  meter.dataset.bytes = String(bytes);
  if (phase === 'Hashing') meter.dataset.hashedBytes = String(bytes);
  if (phase === 'Sending') meter.dataset.sentBytes = String(bytes);
  if (phase === 'Receiving') meter.dataset.receivedBytes = String(bytes);
  if (Object.hasOwn(progress, 'total')) meter.dataset.total = progress.total == null ? '' : String(progress.total);
  const total = meter.dataset.total;
  if (phase === 'Hashing' || phase === 'Sending' || phase === 'Receiving') {
    const fraction = total && BigInt(total) > 0n ? Number(bytes * 10000n / BigInt(total)) / 100 : 0;
    meter.dataset.progress = String(phase === 'Hashing' ? fraction / 2 : phase === 'Sending' ? 50 + fraction / 2 : fraction);
  } else if (phase === 'Ready') meter.dataset.progress = '100';
  if (phase === 'Ready') meter.textContent = '--';
  else if (phase === 'Cancelled' || phase === 'Failed') meter.textContent = `${formatBytes(bytes)} bytes`;
  else if (total && BigInt(total) > 0n) meter.textContent = `${formatBytes(bytes)}/${formatBytes(total)}`;
  else meter.textContent = `${formatBytes(bytes)} bytes`;
  meter.setAttribute('role', 'progressbar');
  meter.setAttribute('aria-label', `${phase}: ${meter.textContent}`);
  if (total) {
    meter.setAttribute('aria-valuemin', '0');
    meter.setAttribute('aria-valuemax', total);
    meter.setAttribute('aria-valuenow', String(bytes));
  } else {
    for (const attribute of ['aria-valuemin','aria-valuemax','aria-valuenow']) meter.removeAttribute(attribute);
  }
  let ascii = meter.parentElement.querySelector(':scope > .transfer-ascii-progress');
  if (!ascii) {
    ascii = createAsciiProgress(document);
    ascii.classList.add('transfer-ascii-progress');
    meter.before(ascii);
  }
  const active = phase === 'Hashing' || phase === 'Sending' || phase === 'Receiving' || phase === 'Verifying';
  const percent = total && BigInt(total) > 0n ? Number(bytes * 10000n / BigInt(total)) / 100 : Number(meter.dataset.progress || '0');
  updateAsciiProgress(ascii, phase, percent, active);
  const failure = document.getElementById('transferFailure');
  if (failure) { failure.hidden = phase !== 'Failed'; failure.textContent = progress.error || 'Transfer failed. Retry the attachment; reconnect and compare SAS if the problem persists.'; }
}

export function discardTranscriptDownloads(transcript) {
  for (const card of transcript.querySelectorAll('.attachment-card:has(a[download])')) {
    const row = card.closest('.message');
    removeAttachmentCard(card);
    row?.remove();
  }
}

export function timeLabel(date = new Date()) {
  const pad = value => String(value).padStart(2, '0');
  return `${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}`;
}

export function escapeHtml(value) {
  return String(value).replace(
    /[&<>"]/g,
    char => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[char],
  );
}

// Status log (#log). Works with <pre> and <div> hosts: one block line per entry.
// The pages seed the log with a placeholder ("Waiting…"); the first real log
// line clears it.
export function createLogger(logEl, options = {}) {
  const placeholder = options.placeholder ?? 'Waiting…';
  const earlierControl = options.earlierControl ?? logEl.parentElement?.querySelector('.log-earlier');
  const isPinned = () => logEl.scrollHeight - logEl.clientHeight - logEl.scrollTop <= 2;

  function hiddenEarlierRows() {
    const viewportTop = logEl.getBoundingClientRect().top + logEl.clientTop;
    return Array.from(logEl.children).filter(row => row.getBoundingClientRect().top < viewportTop);
  }

  function updateEarlierControl() {
    if (!earlierControl) return;
    const hiddenCount = hiddenEarlierRows().length;
    earlierControl.hidden = hiddenCount === 0;
    earlierControl.textContent = `[${hiddenCount} earlier] gg`;
  }

  function revealEarlier() {
    const oldest = hiddenEarlierRows()[0];
    if (!oldest) {
      updateEarlierControl();
      return;
    }
    const viewportTop = logEl.getBoundingClientRect().top + logEl.clientTop;
    const rowTop = oldest.getBoundingClientRect().top;
    logEl.scrollTop = Math.max(0, logEl.scrollTop + rowTop - viewportTop);
    oldest.tabIndex = -1;
    oldest.focus({ preventScroll: true });
    updateEarlierControl();
  }

  function log(message, className = '') {
    const wasPinned = isPinned();
    if (logEl.textContent === placeholder) logEl.textContent = '';
    const line = document.createElement('div');
    line.className = className;
    const renderedMessage = `[${timeLabel()}] ${message}`;
    line.textContent = renderedMessage;
    line.title = renderedMessage;
    logEl.appendChild(line);
    if (wasPinned) logEl.scrollTop = logEl.scrollHeight;
    updateEarlierControl();
    return line;
  }

  function clear() {
    logEl.textContent = '';
    updateEarlierControl();
  }

  logEl.addEventListener('scroll', updateEarlierControl, { passive: true });
  logEl.addEventListener('click', event => {
    const row = event.target;
    if (row instanceof Element && row.parentElement === logEl) row.classList.toggle('expanded');
  });
  earlierControl?.addEventListener('click', revealEarlier);
  updateEarlierControl();
  return { log, clear };
}

const transcriptControllers = new WeakMap();

function createTranscriptController(transcriptEl, jumpEl, pageEvents = window) {
  const existing = transcriptControllers.get(transcriptEl);
  if (existing && !existing.disposed) return existing;

  let pinned = true;
  let unread = 0;
  let disposed = false;
  let resizePoll;
  let observedWidth = transcriptEl.clientWidth;
  let observedHeight = transcriptEl.clientHeight;
  const isPinned = () => transcriptEl.scrollHeight - transcriptEl.clientHeight - transcriptEl.scrollTop <= 2;

  function renderJump() {
    if (!jumpEl) return;
    jumpEl.hidden = unread === 0;
    jumpEl.textContent = `[${unread} new] G`;
  }

  function resetUnread() {
    unread = 0;
    renderJump();
  }

  function pinToBottom() {
    if (disposed) return;
    transcriptEl.scrollTop = transcriptEl.scrollHeight;
    observedWidth = transcriptEl.clientWidth;
    observedHeight = transcriptEl.clientHeight;
    pinned = true;
    resetUnread();
  }

  function onScroll() {
    if (disposed) return;
    if (pinned && (transcriptEl.clientWidth !== observedWidth || transcriptEl.clientHeight !== observedHeight)) return;
    pinned = isPinned();
    if (pinned) resetUnread();
  }

  function onViewportResize() {
    if (disposed) return;
    if (!pinned) {
      observedWidth = transcriptEl.clientWidth;
      observedHeight = transcriptEl.clientHeight;
      return;
    }
    observedWidth = transcriptEl.clientWidth;
    observedHeight = transcriptEl.clientHeight;
    pinToBottom();
  }

  function dispose() {
    if (disposed) return;
    disposed = true;
    clearInterval(resizePoll);
    observer?.disconnect();
    transcriptEl.removeEventListener('scroll', onScroll);
    jumpEl?.removeEventListener('click', pinToBottom);
    pageEvents?.removeEventListener('resize', onViewportResize);
    pageEvents?.removeEventListener('pagehide', dispose);
    transcriptControllers.delete(transcriptEl);
  }

  const observer = typeof ResizeObserver === 'function' ? new ResizeObserver(() => {
    if (!transcriptEl.isConnected) { dispose(); return; }
    if (pinned) pinToBottom();
    observedWidth = transcriptEl.clientWidth;
    observedHeight = transcriptEl.clientHeight;
  }) : null;
  resizePoll = setInterval(() => {
    if (disposed) return;
    if (!transcriptEl.isConnected) { dispose(); return; }
    if (transcriptEl.clientWidth !== observedWidth || transcriptEl.clientHeight !== observedHeight) onViewportResize();
  }, 250);
  observer?.observe(transcriptEl);
  transcriptEl.addEventListener('scroll', onScroll, { passive: true });
  jumpEl?.addEventListener('click', pinToBottom);
  pageEvents?.addEventListener('resize', onViewportResize);
  pageEvents?.addEventListener('pagehide', dispose);
  pinned = isPinned();
  renderJump();

  const controller = {
    get disposed() { return disposed; },
    capturePinned() { return disposed ? false : isPinned(); },
    messageAppended(wasPinned) {
      if (disposed) return;
      if (!transcriptEl.isConnected) { dispose(); return; }
      pinned = wasPinned;
      if (pinned) pinToBottom();
      else { unread += 1; renderJump(); }
    },
    cleared() {
      resetUnread();
      if (!transcriptEl.isConnected) { dispose(); return; }
      pinToBottom();
    },
    dispose,
  };
  transcriptControllers.set(transcriptEl, controller);
  return controller;
}

// Removes all transcript rows and restores the empty-state placeholder.
export function clearTranscript(transcriptEl, emptyText = 'Connect, then send a message or attachment.') {
  for (const card of transcriptEl.querySelectorAll('.attachment-card')) removeAttachmentCard(card);
  transcriptEl.textContent = '';
  if (emptyText) {
    const empty = document.createElement('article');
    empty.className = 'message system empty';
    const bubble = document.createElement('div');
    bubble.className = 'bubble';
    const metaLine = document.createElement('div');
    metaLine.className = 'meta-line';
    const sender = document.createElement('span');
    sender.className = 'sender';
    sender.textContent = 'system>';
    const body = document.createElement('div');
    body.textContent = emptyText;
    metaLine.appendChild(sender);
    bubble.append(metaLine, body);
    empty.appendChild(bubble);
    transcriptEl.appendChild(empty);
  }
  transcriptControllers.get(transcriptEl)?.cleared();
}

function clearTranscriptEmptyState(transcriptEl) {
  const empty = transcriptEl.querySelector('.empty');
  if (empty) empty.remove();
}

// Transcript row: .message.you|peer|system > .bubble > .meta-line + body/card.
export function addMessage(transcriptEl, { side, kind, text, meta, data, hash, pending = false, progress, verified, lifecycle }) {
  const controller = transcriptControllers.get(transcriptEl);
  const wasPinned = controller?.capturePinned();
  clearTranscriptEmptyState(transcriptEl);
  const row = document.createElement('article');
  row.className = `message ${side}`;

  const bubble = document.createElement('div');
  bubble.className = 'bubble';
  const metaLine = document.createElement('div');
  metaLine.className = 'meta-line';
  const senderLabel = document.createElement('span');
  senderLabel.className = 'sender';
  const endpoint = document.body?.dataset.endpoint;
  if (endpoint) {
    const peerEndpoint = endpoint === 'usb' ? 'ble' : 'usb';
    senderLabel.textContent = side === 'you' ? `you/${endpoint}` : side === 'peer' ? `peer/${peerEndpoint}` : 'system';
  } else {
    senderLabel.textContent = side === 'you' ? 'You' : side === 'peer' ? 'Peer' : 'System';
  }
  const time = document.createElement('span');
  time.textContent = timeLabel().slice(0, 5);
  metaLine.append(time, senderLabel);
  bubble.appendChild(metaLine);

  if (kind === 'text') {
    const body = document.createElement('div');
    body.textContent = text;
    bubble.appendChild(body);
  } else {
    bubble.appendChild(verified
      ? renderVerifiedAttachmentCard(verified, lifecycle)
      : renderAttachmentCard(meta, data, hash, pending, progress));
  }

  row.appendChild(bubble);
  transcriptEl.appendChild(row);
  for (const bar of row.querySelectorAll('.ab-progress')) fillAsciiProgress(bar, Number(bar.dataset.progressFraction || '0'));
  if (controller) controller.messageAppended(wasPinned);
  else transcriptEl.scrollTop = transcriptEl.scrollHeight;
  return row;
}

addMessage.createTranscriptController = createTranscriptController;

export function addSystemMessage(transcriptEl, text) {
  return addMessage(transcriptEl, { side: 'system', kind: 'text', text });
}

// Attachment card. All meta-derived strings go through textContent only —
// never innerHTML. Creates an object-URL download link when data is present.
export function renderAttachmentCard(meta, data, hash, pending, progress, lifecycle) {
  if (pending && data instanceof Blob) throw new Error('Pending attachment cannot own a Blob');
  const card = document.createElement('div');
  card.className = 'attachment-card';
  card.dataset.verification = pending ? 'pending' : 'verified';
  card.dataset.phase = pending ? 'Hashing' : 'Ready';
  card.dataset.size = String(meta.size ?? data?.size ?? data?.length ?? 0);
  const fullHash = hash || meta.hash || '';
  if (!pending) card.dataset.sha256 = fullHash;
  const name = document.createElement('div');
  name.className = 'attachment-name';
  name.textContent = attachmentDisplayName(meta.name || meta.filename);
  const size = document.createElement('div');
  size.className = 'attachment-meta';
  size.textContent = `${formatBytes(meta.size ?? data?.length ?? 0)} · ${meta.mimeType || 'application/octet-stream'}`;
  const hashLine = document.createElement('div');
  hashLine.dataset.attachmentHash = '';
  hashLine.className = pending ? 'warn' : 'hash-ok';
  hashLine.textContent = pending
    ? `sha256:${shortHash(fullHash)} pending`
    : `sha256:${shortHash(fullHash)} verified`;
  hashLine.hidden = pending && !fullHash;
  card.append(name, size, hashLine);

  if (!pending && fullHash) {
    const details = document.createElement('details');
    details.className = 'attachment-hash-details';
    const summary = document.createElement('summary');
    summary.textContent = '[sha256]';
    summary.setAttribute('aria-label', 'Show full SHA-256 hash');
    const full = document.createElement('div');
    full.className = 'attachment-hash-full';
    full.textContent = `sha256:${fullHash}`;
    details.append(summary, full);
    card.appendChild(details);
  }

  if (progress != null) {
    const label = document.createElement('div');
    label.className = 'progress-label';
    label.textContent = progress.label.split(':')[0];
    card.appendChild(label);
  }

  if (data) {
    const blob = data instanceof Blob ? data : new Blob([data], { type: meta.mimeType || 'application/octet-stream' });
    const link = document.createElement('a');
    link.className = 'download-link';
    link.download = attachmentDownloadName(meta.name || meta.filename);
    link.dataset.downloadAttachment = '';
    link.textContent = '[download]';
    link.setAttribute('aria-label', `Download ${attachmentDisplayName(meta.name || meta.filename)}`);
    card.appendChild(link);
    (lifecycle ?? defaultAttachmentLifecycle()).own(card, blob, link);
    const remove = document.createElement('button');
    remove.type = 'button'; remove.className = 'clear-btn';
    remove.dataset.removeAttachment = '';
    remove.textContent = '[remove]';
    remove.setAttribute('aria-label', `Remove ${attachmentDisplayName(meta.name || meta.filename)}`);
    remove.addEventListener('click', () => { const row = card.closest('.message'); removeAttachmentCard(card); row?.remove(); });
    card.append(remove);
  }
  return card;
}

// Trust boundary: only pass a successful accumulator receipt here, never a
// partially authenticated receive. The card owns the Blob's URL until cleanup.
export function renderVerifiedAttachmentCard(verified, lifecycle) {
  const { blob, name, mime, size, hash } = verified;
  if (!(blob instanceof Blob) || typeof size !== 'bigint' || BigInt(blob.size) !== size
    || typeof name !== 'string' || typeof mime !== 'string' || typeof hash !== 'string'
    || hash.length !== 64 || !/^[0-9a-f]+$/.test(hash) || blob.type !== mime.toLowerCase()) {
    throw new TypeError('Expected verified Blob receipt');
  }
  return renderAttachmentCard({ name, mimeType: mime, size, hash }, blob, hash, false,
    {percent:100, complete:true, label:'Verified: 100%'}, lifecycle);
}

const attachmentCleanups = new WeakMap();
const attachmentDefaults = new WeakMap();

function defaultAttachmentLifecycle() {
  let lifecycle = attachmentDefaults.get(document);
  if (!lifecycle || lifecycle.disposed) {
    lifecycle = createAttachmentLifecycle(document.defaultView);
    attachmentDefaults.set(document, lifecycle);
  }
  return lifecycle;
}

export function removeAttachmentCard(card) {
  card.remove();
  attachmentCleanups.get(card)?.();
}

export function replaceAttachmentCard(card, replacement) {
  if (card === replacement) return;
  card.replaceWith(replacement);
  attachmentCleanups.get(card)?.();
}

// Explicit/disposable binding. Legacy rendering lazily requests a document
// lifecycle; importing this module never touches DOM or installs listeners.
export function createAttachmentLifecycle(pageEvents = window) {
  const owned = new Map();
  let disposed = false;
  const observer = new MutationObserver(records => {
    for (const [card, { link, url, cleanup }] of owned) {
      const removedCard = !card.isConnected && records.some(record =>
        Array.from(record.removedNodes).some(removed => removed.contains(card)));
      if (removedCard || !card.contains(link) || link.getAttribute('href') !== url) cleanup();
    }
  });
  observer.observe(document, { childList: true, subtree: true, attributes: true, attributeFilter: ['href'] });

  function dispose() {
    if (disposed) return;
    disposed = true;
    observer.disconnect();
    pageEvents.removeEventListener('pagehide', dispose);
    let failed = false;
    let firstError;
    for (const { cleanup } of Array.from(owned.values())) {
      try { cleanup(); } catch (error) {
        if (!failed) { failed = true; firstError = error; }
      }
    }
    if (failed) throw firstError;
  }
  pageEvents.addEventListener('pagehide', dispose);

  return {
    get disposed() { return disposed; },
    own(card, blob, link) {
      if (disposed) throw new Error('Attachment lifecycle disposed');
      if (attachmentCleanups.has(card)) throw new Error('Attachment card already owned');
      const url = URL.createObjectURL(blob);
      let released = false;
      function cleanup() {
        if (released) return;
        released = true;
        owned.delete(card);
        attachmentCleanups.delete(card);
        blob = null;
        try {
          card.remove();
        } finally {
          try { link.removeAttribute('href'); }
          finally { URL.revokeObjectURL(url); }
        }
      }
      try {
        link.href = url;
        if (disposed) throw new Error('Attachment lifecycle disposed');
        owned.set(card, { link, url, cleanup });
        if (disposed) throw new Error('Attachment lifecycle disposed');
        attachmentCleanups.set(card, cleanup);
        if (disposed) throw new Error('Attachment lifecycle disposed');
      } catch (error) {
        try { cleanup(); } finally {
          // Reentrant disposal may have released the URL before a late map write.
          owned.delete(card);
          attachmentCleanups.delete(card);
          throw error;
        }
      }
    },
    dispose,
  };
}

export function updateCardProgress(messageRow, _percent, label, state) {
  const card = messageRow.querySelector('.attachment-card');
  if (card) card.dataset.phase = state === 'cancelled' ? (label === 'Failed' ? 'Failed' : 'Cancelled') : label.split(':')[0];
  const labelEl = messageRow.querySelector('.progress-label');
  if (labelEl) labelEl.textContent = label.split(':')[0];
}

export function completeOutboundCard(row, receipt, file) {
  if (file) {
    const blob = file.type === receipt.mimeType.toLowerCase() ? file : new Blob([file], {type:receipt.mimeType});
    const target = row.querySelector('.attachment-card');
    const lifecycle = createAttachmentLifecycle(document.defaultView);
    let replacement;
    try {
      if (!target) throw new Error('Outbound attachment card is detached');
      replacement = renderVerifiedAttachmentCard({
        blob, name:receipt.name, mime:receipt.mimeType, size:receipt.payloadSize, hash:receipt.payloadSha256,
      }, lifecycle);
      if (!target.isConnected) throw new Error('Outbound attachment card is detached');
      replaceAttachmentCard(target, replacement);
      for (const bar of replacement.querySelectorAll('.ab-progress')) fillAsciiProgress(bar, Number(bar.dataset.progressFraction || '0'));
    } catch (error) {
      if (!replacement?.isConnected) {
        try { lifecycle.dispose(); } catch (_) { /* Preserve the original setup/replacement failure. */ }
      }
      throw error;
    }
    return;
  }
  const hash = row.querySelector('[data-attachment-hash]');
  hash.textContent = `SHA-256: ${receipt.payloadSha256}`;
  hash.className = 'hash-ok';
  hash.hidden = false;
  updateCardProgress(row, 100, 'Sent', 'complete');
}

export function createTransferRateMeter({now = () => performance.now(), windowMs = 3000, sampleMs = 250} = {}) {
  let points, latest, initial, started;
  function reset(bytes = 0n) {
    latest = initial = BigInt(bytes);
    started = now();
    points = [{time:started, bytes:latest}];
  }
  function update(bytes) {
    bytes = BigInt(bytes);
    if (bytes < latest) reset(bytes);
    latest = bytes;
  }
  function bps() {
    const time = Math.max(now(), points.at(-1).time);
    if (time - points.at(-1).time >= sampleMs) points.push({time, bytes:latest});
    else if (points.length > 1 && time === points.at(-1).time) points.at(-1).bytes = latest;
    const boundary = Math.max(started, time - windowMs);
    while (points.length > 1 && points[1].time <= boundary) points.shift();
    const first = points[0], second = points[1];
    // Average byte deltas over elapsed time, never equally weight per-ACK rates.
    const beforeBoundary = second && first.time < boundary
      ? Number(second.bytes - first.bytes) * (boundary - first.time) / (second.time - first.time) : 0;
    if (time - started < 500 || time === boundary) return null;
    return Math.max(0, Number(latest - first.bytes) - beforeBoundary) * 1000 / (time - boundary);
  }
  function averageBps() {
    const elapsed = now() - started;
    return elapsed > 0 ? Number(latest - initial) * 1000 / elapsed : null;
  }
  reset();
  return {reset, update, bps, averageBps};
}

// Throughput meter. EMA over inter-chunk byte rates; ETA from remaining bytes
// (estimated via average bytes/chunk). "--" until at least 2 rate samples.
// Inject options.now (ms) for deterministic tests.
export function createThroughputMeter(options = {}) {
  const nowFn = options.now ?? (() => Date.now());
  const alpha = options.alpha ?? 0.35;
  let lastTime = null;
  let lastBytes = 0;
  let emaBps = 0;
  let samples = 0;
  let chunksDone = 0;
  let totalChunks = 0;
  let bytesDone = 0;

  function tick(done, total, bytes) {
    chunksDone = done;
    totalChunks = total;
    bytesDone = bytes;
    const t = nowFn();
    if (lastTime != null) {
      const dt = (t - lastTime) / 1000;
      const db = bytes - lastBytes;
      if (dt > 0 && db >= 0) {
        const sample = db / dt;
        emaBps = samples === 0 ? sample : alpha * sample + (1 - alpha) * emaBps;
        samples += 1;
      }
    }
    lastTime = t;
    lastBytes = bytes;
  }

  function text() {
    const prefix = `${chunksDone}/${totalChunks} chunks`;
    if (samples < 2 || emaBps <= 0) return `${prefix} · -- KB/s · -- left`;
    const kbps = emaBps / 1024;
    const avgChunkBytes = chunksDone > 0 ? bytesDone / chunksDone : 0;
    const remainingBytes = avgChunkBytes * Math.max(0, totalChunks - chunksDone);
    const etaSec = Math.max(0, Math.round(remainingBytes / emaBps));
    return `${prefix} · ${kbps.toFixed(1)} KB/s · ~${etaSec}s left`;
  }

  return { tick, text };
}
