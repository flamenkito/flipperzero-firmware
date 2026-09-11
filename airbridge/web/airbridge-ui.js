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

export function setTransferPhase(phase, progress = {}) {
  const panel = document.getElementById('panelState');
  const meter = document.getElementById('throughput');
  const same = !progress.reset && panel.dataset.phase === phase;
  const previous = BigInt(meter.dataset.bytes || '0');
  const bytes = progress.bytes == null ? previous : same && progress.bytes < previous ? previous : progress.bytes;
  panel.dataset.phase = phase;
  panel.textContent = phase;
  panel.setAttribute('role', 'status');
  meter.dataset.bytes = String(bytes);
  if (phase === 'Hashing') meter.dataset.hashedBytes = String(bytes);
  if (phase === 'Sending') meter.dataset.sentBytes = String(bytes);
  if (phase === 'Receiving') meter.dataset.receivedBytes = String(bytes);
  if (Object.hasOwn(progress, 'total')) meter.dataset.total = progress.total == null ? '' : String(progress.total);
  const total = meter.dataset.total;
  if (phase === 'Hashing' || phase === 'Sending') {
    const fraction = total && BigInt(total) > 0n ? Number(bytes * 10000n / BigInt(total)) / 100 : 0;
    meter.dataset.progress = String(phase === 'Hashing' ? fraction / 2 : 50 + fraction / 2);
  } else if (phase === 'Ready') meter.dataset.progress = '100';
  meter.textContent = `${bytes}${total ? `/${total}` : ''} payload bytes`;
  meter.setAttribute('role', 'progressbar');
  meter.setAttribute('aria-label', `${phase}: ${meter.textContent}`);
  if (total) {
    meter.setAttribute('aria-valuemin', '0');
    meter.setAttribute('aria-valuemax', total);
    meter.setAttribute('aria-valuenow', String(bytes));
  } else {
    for (const attribute of ['aria-valuemin','aria-valuemax','aria-valuenow']) meter.removeAttribute(attribute);
  }
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

  function log(message, className = '') {
    if (logEl.textContent === placeholder) logEl.textContent = '';
    const line = document.createElement('div');
    line.className = className;
    line.textContent = `[${timeLabel()}] ${message}`;
    logEl.appendChild(line);
    logEl.scrollTop = logEl.scrollHeight;
    return line;
  }

  function clear() {
    logEl.textContent = '';
  }

  return { log, clear };
}

// Removes all transcript rows and restores the empty-state placeholder.
export function clearTranscript(transcriptEl, emptyText = 'Connect, then send a message or attachment.') {
  for (const card of transcriptEl.querySelectorAll('.attachment-card')) removeAttachmentCard(card);
  transcriptEl.textContent = '';
  if (!emptyText) return;
  const empty = document.createElement('div');
  empty.className = 'empty';
  empty.textContent = emptyText;
  transcriptEl.appendChild(empty);
}

function clearTranscriptEmptyState(transcriptEl) {
  const empty = transcriptEl.querySelector('.empty');
  if (empty) empty.remove();
}

// Transcript row: .message.you|peer|system > .bubble > .meta-line + body/card.
export function addMessage(transcriptEl, { side, kind, text, meta, data, hash, pending = false, progress, verified, lifecycle }) {
  clearTranscriptEmptyState(transcriptEl);
  const row = document.createElement('article');
  row.className = `message ${side}`;

  const bubble = document.createElement('div');
  bubble.className = 'bubble';
  const metaLine = document.createElement('div');
  metaLine.className = 'meta-line';
  const senderLabel = document.createElement('span');
  senderLabel.className = 'sender';
  senderLabel.textContent = side === 'you' ? 'You' : side === 'peer' ? 'Peer' : 'System';
  const time = document.createElement('span');
  time.textContent = timeLabel();
  metaLine.append(senderLabel, time);
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
  transcriptEl.scrollTop = transcriptEl.scrollHeight;
  return row;
}

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
  if (!pending) card.dataset.sha256 = hash || meta.hash || '';
  const name = document.createElement('div');
  name.className = 'attachment-name';
  name.textContent = attachmentDisplayName(meta.name || meta.filename);
  const size = document.createElement('div');
  size.textContent = `${formatBytes(meta.size ?? data?.length ?? 0)} · ${meta.mimeType || 'application/octet-stream'}`;
  const hashLine = document.createElement('div');
  hashLine.dataset.attachmentHash = '';
  hashLine.className = pending ? 'warn' : 'hash-ok';
  hashLine.textContent = pending
    ? `SHA-256 pending: ${shortHash(meta.hash)}`
    : `SHA-256 verified: ${hash || meta.hash || 'unknown'}`;
  card.append(name, size, hashLine);

  if (progress != null) {
    const container = document.createElement('div');
    container.className = 'progress-container';
    const track = document.createElement('div');
    track.className = 'progress-bar-track';
    track.setAttribute('role', 'progressbar');
    track.setAttribute('aria-valuenow', Math.round(progress.percent));
    track.setAttribute('aria-valuemin', '0');
    track.setAttribute('aria-valuemax', '100');
    const fill = document.createElement('div');
    fill.className = `progress-bar-fill${progress.complete ? ' complete' : ''}${progress.cancelled ? ' cancelled' : ''}`;
    fill.style.width = `${progress.percent}%`;
    fill.setAttribute('aria-hidden', 'true');
    track.appendChild(fill);
    const label = document.createElement('div');
    label.className = 'progress-label';
    label.textContent = progress.label;
    container.append(track, label);
    card.appendChild(container);
  }

  if (data) {
    const blob = data instanceof Blob ? data : new Blob([data], { type: meta.mimeType || 'application/octet-stream' });
    const link = document.createElement('a');
    link.className = 'download-link';
    link.download = attachmentDownloadName(meta.name || meta.filename);
    link.dataset.downloadAttachment = '';
    link.textContent = 'Download attachment';
    card.appendChild(link);
    (lifecycle ?? defaultAttachmentLifecycle()).own(card, blob, link);
    const remove = document.createElement('button');
    remove.type = 'button'; remove.className = 'clear-btn';
    remove.dataset.removeAttachment = '';
    remove.textContent = 'Remove attachment';
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
  return renderAttachmentCard({ name, mimeType: mime, size, hash }, blob, hash, false, undefined, lifecycle);
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

export function updateCardProgress(messageRow, percent, label, state) {
  const card = messageRow.querySelector('.attachment-card');
  if (card) card.dataset.phase = state === 'cancelled' ? (label === 'Failed' ? 'Failed' : 'Cancelled') : label.split(':')[0];
  const track = messageRow.querySelector('.progress-bar-track');
  const fill = messageRow.querySelector('.progress-bar-fill');
  const labelEl = messageRow.querySelector('.progress-label');
  if (!track || !fill || !labelEl) return;
  const safe = Math.max(0, Math.min(100, percent));
  track.setAttribute('aria-valuenow', Math.round(safe));
  fill.style.width = `${safe}%`;
  fill.className = `progress-bar-fill${state === 'complete' ? ' complete' : ''}${state === 'cancelled' ? ' cancelled' : ''}`;
  labelEl.textContent = label;
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
  updateCardProgress(row, 100, 'Sent', 'complete');
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
