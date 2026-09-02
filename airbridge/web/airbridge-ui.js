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
export function addMessage(transcriptEl, { side, kind, text, meta, data, hash, pending = false, progress }) {
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
    bubble.appendChild(renderAttachmentCard(meta, data, hash, pending, progress));
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
export function renderAttachmentCard(meta, data, hash, pending, progress) {
  const card = document.createElement('div');
  card.className = 'attachment-card';
  const name = document.createElement('div');
  name.className = 'attachment-name';
  name.textContent = meta.name || meta.filename || 'attachment';
  const size = document.createElement('div');
  size.textContent = `${formatBytes(meta.size ?? data?.length ?? 0)} · ${meta.mimeType || 'application/octet-stream'}`;
  const hashLine = document.createElement('div');
  hashLine.className = pending ? 'warn' : 'hash-ok';
  hashLine.textContent = pending
    ? `SHA-256 pending: ${shortHash(meta.hash)}`
    : `SHA-256 verified: ${shortHash(hash || meta.hash)}`;
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
    const blob = new Blob([data], { type: meta.mimeType || 'application/octet-stream' });
    const link = document.createElement('a');
    link.className = 'download-link';
    link.href = URL.createObjectURL(blob);
    link.download = meta.name || meta.filename || 'airbridge-attachment';
    link.textContent = 'Download attachment';
    card.appendChild(link);
  }
  return card;
}

export function updateCardProgress(messageRow, percent, label, state) {
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
