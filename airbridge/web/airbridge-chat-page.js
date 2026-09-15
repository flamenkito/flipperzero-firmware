// Shared page glue for chat-usb.html and chat-ble.html.
//
// Both chat pages are thin boots that own their mutable state lets and pass a
// ctx accessor object (getters/setters delegating to those lets) into
// createChatPage(config). This module NEVER caches page state in its own
// variables: every read/write of transport, receiver, outbound,
// cryptoSession, isSending, inboundHelloItemId and inboundMeta goes through
// ctx, because the browser test harness assigns those page lets directly.
// Only isConnecting/isCancelling (page-lifecycle flags no hook touches) and
// DOM/evidence handles live in this module's closure.
import { createChatOutbound } from './airbridge-chat-outbound.js';
import { renderReceivedItem, routeReceiveControl } from './airbridge-chat-receive.js';
import { installMockStreamPeer } from './airbridge-mock-stream-peer.js';
import { createPairedChatMock } from './airbridge-chat-mock.js';
import { createTimingRecorder, recordCancel, recordFlipperCounters } from './airbridge-evidence.js';
import {
  AirBridgeCryptoSession,
  buildMessage,
  CRYPTO_STATE,
  ItemReceiver,
  MAX_PAYLOAD,
  MSG,
  parseMessage,
} from './airbridge-protocol.js';
import {
  addMessage,
  clearTranscript,
  createThroughputMeter,
  discardTranscriptDownloads,
  formatBytes,
  setTransferPhase,
  shortHash,
  updateCardProgress,
} from './airbridge-ui.js';

const textEncoder = new TextEncoder();
const COMMANDS = Object.freeze([
  Object.freeze({ name: 'connect', description: 'connect transport', aliases: [] }),
  Object.freeze({ name: 'disconnect', description: 'disconnect transport', aliases: [] }),
  Object.freeze({ name: 'clear', description: 'clear transcript', aliases: [] }),
  Object.freeze({ name: 'clearlog', description: 'clear diagnostics', aliases: [] }),
  Object.freeze({ name: 'cancel', description: 'cancel active transfer', aliases: [] }),
  Object.freeze({ name: 'accept', description: 'accept matching SAS', aliases: [] }),
  Object.freeze({ name: 'abort', description: 'abort SAS session', aliases: [] }),
  Object.freeze({ name: 'file', description: 'pick now; send immediately when verified', aliases: ['attach'] }),
  Object.freeze({ name: 'logs', description: 'toggle diagnostics', aliases: [] }),
  Object.freeze({ name: 'help', description: 'list commands', aliases: [] }),
]);
const COMMAND_BY_TOKEN = new Map(COMMANDS.flatMap(command => [command.name, ...command.aliases].map(token => [token, command])));

function sleep(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

// Unified in-page mock transport (USB semantics): send throws when EITHER
// side is disconnected, frames are byte-copied so the sender cannot mutate
// retained retry bytes, and crypto handshake frames take an 8ms delay.
class LinkedMockTransport {
  constructor(name) {
    this.name = name;
    this.peer = null;
    this.connected = false;
    this.receiveCallback = null;
    this.disconnectCallback = null;
  }

  link(peer) { this.peer = peer; }
  async connect() { this.connected = true; if (this.peer) this.peer.connected = true; }
  async disconnect() { this.connected = false; if (this.peer) this.peer.connected = false; this.disconnectCallback?.(); }
  async send(frame) {
    if (!this.connected || !this.peer?.connected) throw new Error(`${this.name} is not connected`);
    const copy = new Uint8Array(frame);
    const message = parseMessage(frame);
    if (message.type >= MSG.KEY_OFFER || (message.type === MSG.ACK && message.payload.length === 3)) await sleep(8);
    else await Promise.resolve();
    this.peer.receiveCallback?.(copy);
  }
  onReceive(callback) { this.receiveCallback = callback; return () => { if (this.receiveCallback === callback) this.receiveCallback = null; }; }
  onDisconnect(callback) { this.disconnectCallback = callback; return () => { if (this.disconnectCallback === callback) this.disconnectCallback = null; }; }
  isConnected() { return this.connected; }
  getName() { return this.name; }
}

export function createChatPage(ctx, config) {
  let isConnecting = false;
  let isCancelling = false;
  let unsubscribeReceive = null;
  let unsubscribeDisconnect = null;
  let pendingReceiveRow = null;
  let inboundMeter = null;
  let inboundTiming = null;
  let displayStateObserver = null;
  let promptContext = null;
  let termPrompt = null;
  let termMirror = null;
  let termMirrorTrack = null;
  let termMirrorValue = null;
  let termGhost = null;
  let termCursor = null;
  let termSuggestions = null;
  let termCursorFrame = 0;
  let termCursorResizeObserver = null;
  let completionMatches = [];
  let completionIndex = 0;
  let completionDismissedValue = null;
  let pendingFileSend = null;
  const evidenceRecords = [];

  const log = (message, level) => ctx.log(message, level);

  function publishEvidence(record) {
    if (!ctx.evidenceMode) return record;
    evidenceRecords.push(record);
    globalThis.dispatchEvent?.(new CustomEvent('airbridge:evidence', { detail: record }));
    if (record.completeTimeMs != null) {
      const state = record.errorState ? ` error=${record.errorState}` : '';
      log(`Evidence ${record.operation}: ${record.bundleBytes} bytes, first=${record.firstReportLatencyMs} ms, total=${record.completeTimeMs} ms, ${record.transferKbps} KB/s.${state}`, record.errorState ? 'warn' : 'ok');
    }
    return record;
  }

  let transferLabel = null;
  let rateEmaBps = 0;
  let rateLastBytes = 0n;
  let rateLastTime = 0;

  function renderTransferDetail() {
    const detail = ctx.$('transferDetail');
    if (!detail) return;
    const phase = ctx.$('panelState').dataset.phase || 'Ready';
    const active = phase === 'Hashing' || phase === 'Sending' || phase === 'Receiving' || phase === 'Verifying';
    document.querySelector('.diagnostic-rail')?.classList.toggle('transfer-active', active);
    if (!active) {
      detail.hidden = true;
      detail.textContent = '';
      transferLabel = null;
      rateEmaBps = 0;
      rateLastBytes = 0n;
      rateLastTime = 0;
      return;
    }
    const bytes = BigInt(ctx.$('throughput').dataset.bytes || '0');
    const now = Date.now();
    if (rateLastTime && now > rateLastTime && bytes > rateLastBytes) {
      const sample = Number(bytes - rateLastBytes) / ((now - rateLastTime) / 1000);
      rateEmaBps = rateEmaBps ? 0.35 * sample + 0.65 * rateEmaBps : sample;
    }
    rateLastBytes = bytes;
    rateLastTime = now;
    const direction = phase === 'Receiving' ? 'receiving' : phase === 'Verifying' ? 'verifying' : phase === 'Hashing' ? 'hashing' : 'sending';
    const rate = rateEmaBps > 0 ? `${(rateEmaBps / 1024).toFixed(1)} KB/s` : '-- KB/s';
    detail.hidden = false;
    detail.textContent = `${direction}${transferLabel ? ` · ${transferLabel}` : ''} · ${rate}`;
  }

  function setPageTransferPhase(phase, progress = {}) {
    setTransferPhase(phase, progress);
    renderDisplayState();
    renderTransferDetail();
  }

  function resetTransferPanel() {
    setPageTransferPhase('Ready', { bytes: 0n, total: null, reset: true });
  }

  function setConnectionStatus(status, deviceName = '') {
    const text = status === 'connecting' ? 'connecting' : status === 'connected' ? 'connected' : 'disconnected';
    const pillClass = status === 'connected'
      ? 'status-connected'
      : status === 'connecting'
        ? 'status-connecting'
        : status === 'error'
          ? 'status-error'
          : 'status-disconnected';
    ctx.$('connectionStatus').textContent = text;
    ctx.$('statusPill').className = `status-pill ${pillClass}`;
    ctx.$('deviceName').textContent = deviceName || 'No device';
    const helper = document.querySelector('.endpoint-helper');
    if (helper) helper.hidden = status === 'connected';
    renderDisplayState();
    updateControls();
  }

  function renderDisplayState() {
    const connected = Boolean(ctx.transport?.isConnected());
    const unlocked = isCryptoUnlocked();
    const phase = ctx.$('panelState').dataset.phase || 'Ready';
    const state = !connected ? 'idle' : !unlocked ? 'locked' : phase === 'Ready' ? 'ready' : phase.toLowerCase();
    const displayState = ctx.$('displayState');
    displayState.dataset.state = state;
    let value = displayState.querySelector('.status-value');
    if (!value) {
      const key = document.createElement('span');
      key.className = 'status-key';
      key.dataset.label = 'state:';
      key.textContent = 'state=';
      value = document.createElement('span');
      value.className = 'status-value';
      const cursor = document.createElement('span');
      cursor.className = 'status-cursor';
      cursor.setAttribute('aria-hidden', 'true');
      displayState.replaceChildren(key, value, cursor);
    }
    value.textContent = state;
    updateHeaderStatus(state);
    updateStateSurfaces(connected, unlocked);
    renderPromptContext();
  }

  function updateHeaderStatus(plainState = 'idle') {
    const banner = ctx.$('headerStatus');
    if (!banner) return;
    banner.dataset.state = plainState;
    banner.textContent = plainState;
  }

  function renderPromptContext() {
    if (!promptContext) return;
    const endpoint = document.querySelector('.tabline-endpoint')?.textContent.trim() || config.endpoint;
    const connection = ctx.$('connectionStatus').textContent;
    const crypto = ctx.$('cryptoState').textContent;
    const sas = ctx.$('sasCode').textContent;
    const mode = ctx.isMockMode ? ' (mock)' : '';
    let tone = 'idle';
    let parts;

    if (ctx.$('statusPill').classList.contains('status-error')) {
      tone = 'failure';
      parts = [[endpoint, 'endpoint'], [' ● failure', 'failure'], [' — /connect to retry', 'hint']];
    } else if (connection === 'disconnected') {
      parts = [[endpoint, 'endpoint'], [' ○ disconnected', 'idle'], [' — /connect to begin', 'hint']];
    } else if (connection === 'connecting') {
      tone = 'pending';
      parts = [[endpoint, 'endpoint'], [' ● connecting', 'pending'], mode && [mode, 'mode']];
    } else if (crypto === 'verified') {
      tone = 'live';
      parts = [[endpoint, 'endpoint'], [' ● connected', 'live'], [' ✔ verified', 'live'], [` sas ${sas}`, 'sas'], mode && [mode, 'mode']];
    } else if (crypto === 'aborted') {
      tone = 'failure';
      parts = [[endpoint, 'endpoint'], [' ● connected', 'live'], [' ✖ aborted', 'failure'], [` sas ${sas}`, 'sas'], mode && [mode, 'mode']];
    } else {
      tone = 'pending';
      parts = [[endpoint, 'endpoint'], [' ● connected', 'live'], [` • ${crypto}`, 'pending'], [` sas ${sas}`, 'sas'], mode && [mode, 'mode']];
    }

    promptContext.replaceChildren(...parts.filter(Boolean).map(([text, role]) => {
      const span = document.createElement('span');
      span.className = `prompt-context-${role}`;
      span.textContent = text;
      return span;
    }));
    promptContext.dataset.tone = tone;
    promptContext.title = promptContext.textContent;
  }

  function renderInputMirror() {
    if (!termMirrorValue) return;
    const value = ctx.$('textInput').value;
    const plain = document.createElement('span');
    plain.className = 'term-token term-token-plain';
    if (!value.startsWith('/') || value.startsWith('//')) {
      plain.textContent = value;
      termMirrorValue.replaceChildren(plain);
      return;
    }

    const commandToken = value.match(/^\/\S*/)?.[0] ?? value;
    const commandName = commandToken.slice(1).toLowerCase();
    const exact = COMMAND_BY_TOKEN.has(commandName);
    const prefix = !exact && COMMANDS.some(command => command.name.startsWith(commandName));
    const command = document.createElement('span');
    command.className = `term-token term-token-${exact ? 'valid' : prefix ? 'prefix' : 'invalid'}`;
    command.textContent = commandToken;
    plain.textContent = value.slice(commandToken.length);
    termMirrorValue.replaceChildren(command, plain);
  }

  function dismissAutocomplete(remember = false) {
    completionMatches = [];
    completionIndex = 0;
    completionDismissedValue = remember ? ctx.$('textInput').value : null;
    if (termSuggestions) {
      termSuggestions.hidden = true;
      termSuggestions.replaceChildren();
    }
    if (termGhost) termGhost.textContent = '';
    ctx.$('textInput').setAttribute('aria-expanded', 'false');
    ctx.$('textInput').removeAttribute('aria-activedescendant');
  }

  function renderAutocomplete() {
    if (!termSuggestions || completionMatches.length === 0) return;
    const prefix = ctx.$('textInput').value.slice(1);
    completionIndex = Math.min(completionIndex, completionMatches.length - 1);
    const rows = completionMatches.map((command, index) => {
      const row = document.createElement('div');
      row.className = 'term-suggestion';
      row.id = `term-suggestion-${index}`;
      row.setAttribute('role', 'option');
      row.setAttribute('aria-selected', String(index === completionIndex));
      const name = document.createElement('span');
      name.className = 'term-suggestion-name';
      name.textContent = `/${command.name}`;
      const description = document.createElement('span');
      description.className = 'term-suggestion-description';
      description.textContent = ` — ${command.description}`;
      const marker = document.createElement('span');
      marker.className = 'term-suggestion-marker';
      marker.textContent = index === completionIndex ? '◄' : '';
      row.append(name, description, marker);
      return row;
    });
    termSuggestions.replaceChildren(...rows);
    termSuggestions.hidden = false;
    const selected = completionMatches[completionIndex];
    termGhost.textContent = selected.name.slice(prefix.length);
    const input = ctx.$('textInput');
    input.setAttribute('aria-expanded', 'true');
    input.setAttribute('aria-activedescendant', `term-suggestion-${completionIndex}`);
  }

  function refreshAutocomplete() {
    const value = ctx.$('textInput').value;
    if (completionDismissedValue !== null && completionDismissedValue !== value) completionDismissedValue = null;
    if (!/^\/[a-z]*$/i.test(value) || value.startsWith('//') || completionDismissedValue === value) {
      dismissAutocomplete(completionDismissedValue === value);
      return;
    }
    const prefix = value.slice(1).toLowerCase();
    completionMatches = COMMANDS.filter(command => command.name.startsWith(prefix));
    if (completionMatches.length === 0) {
      dismissAutocomplete();
      return;
    }
    completionIndex = 0;
    renderAutocomplete();
  }

  function acceptAutocomplete() {
    if (completionMatches.length === 0) return;
    const names = completionMatches.map(command => command.name);
    let completion = names[0];
    for (let index = 1; index < names.length; index++) {
      let length = 0;
      while (length < completion.length && length < names[index].length && completion[length] === names[index][length]) length++;
      completion = completion.slice(0, length);
    }
    const input = ctx.$('textInput');
    input.value = `/${completion}`;
    input.setSelectionRange(input.value.length, input.value.length);
    input.dispatchEvent(new Event('input', { bubbles: true }));
  }

  function updateTerminalCursor() {
    cancelAnimationFrame(termCursorFrame);
    termCursorFrame = requestAnimationFrame(() => {
      if (!termPrompt || !termMirror || !termMirrorValue || !termCursor) return;
      const form = ctx.$('textForm');
      const input = ctx.$('textInput');
      renderInputMirror();
      const inputStyle = getComputedStyle(input);
      const font = inputStyle.font;
      const letterSpacing = inputStyle.letterSpacing;
      termPrompt.style.font = font;
      termPrompt.style.letterSpacing = letterSpacing;
      termMirror.style.font = font;
      termMirror.style.letterSpacing = letterSpacing;
      termMirror.style.paddingInline = inputStyle.paddingInline;
      termMirror.style.lineHeight = inputStyle.height;
      termMirror.style.blockSize = inputStyle.height;
      termCursor.style.font = font;
      termCursor.style.letterSpacing = letterSpacing;
      termCursor.style.lineHeight = inputStyle.height;

      const formRect = form.getBoundingClientRect();
      const inputRect = input.getBoundingClientRect();
      const paddingStart = Number.parseFloat(inputStyle.paddingInlineStart) || 0;
      const paddingEnd = Number.parseFloat(inputStyle.paddingInlineEnd) || 0;
      const textStart = inputRect.left - formRect.left + paddingStart;
      const textWidth = termMirrorValue.getBoundingClientRect().width;
      const cursorWidth = termCursor.getBoundingClientRect().width;
      const textEnd = inputRect.right - formRect.left - paddingEnd - cursorWidth;
      const cursorX = Math.max(textStart, Math.min(textStart + textWidth - input.scrollLeft, textEnd));
      form.style.setProperty('--term-input-x', `${inputRect.left - formRect.left}px`);
      form.style.setProperty('--term-input-width', `${inputRect.width}px`);
      form.style.setProperty('--term-input-scroll', `${-input.scrollLeft}px`);
      form.style.setProperty('--term-cursor-x', `${cursorX}px`);
      form.style.setProperty('--term-cursor-y', `${inputRect.top - formRect.top}px`);
    });
  }

  function currentEmptyPlaceholder(connected = Boolean(ctx.transport?.isConnected()), unlocked = isCryptoUnlocked()) {
    if (!connected) return 'Type /connect to open a secure channel.';
    if (!unlocked) return 'Compare the SAS code, then type /accept or /abort.';
    return 'Secure channel ready — type a message or /file.';
  }

  function updateStateSurfaces(connected, unlocked) {
    const emptyBubble = ctx.$('transcript').querySelector('.message.system.empty .bubble');
    if (emptyBubble) {
      const body = emptyBubble.lastElementChild;
      if (body?.previousElementSibling?.classList?.contains('meta-line')) body.textContent = currentEmptyPlaceholder(connected, unlocked);
    }
  }

  let prevCryptoState = null;

  function updateCryptoPanel(status = ctx.cryptoSession?.getStatus()) {
    const state = status?.state ?? CRYPTO_STATE.REQUIRED;
    const unlocked = state === CRYPTO_STATE.UNLOCKED;
    if (!unlocked) ctx.outbound?.cancel();
    const pending = state === CRYPTO_STATE.SAS_PENDING;
    const aborted = state === CRYPTO_STATE.ABORTED;
    ctx.$('cryptoState').textContent = unlocked ? 'verified' : pending ? 'pending' : aborted ? 'aborted' : 'locked';
    ctx.$('cryptoPill').className = `status-pill crypto-pill ${unlocked ? 'crypto-unlocked' : pending ? 'crypto-pending' : aborted ? 'crypto-aborted' : 'crypto-locked'}`;
    ctx.$('cryptoLock').textContent = 'SEC';
    ctx.$('sasCode').textContent = status?.sas ?? '------';
    const bannerCode = ctx.$('sasBannerCode');
    if (bannerCode) bannerCode.textContent = status?.sas ?? '------';
    document.querySelector('.sas-verified').hidden = !unlocked;
    ctx.$('acceptSasBtn').disabled = !pending || status.localAccepted;
    ctx.$('abortCryptoBtn').disabled = !ctx.cryptoSession || state === CRYPTO_STATE.REQUIRED;
    const sasBanner = ctx.$('sasBanner');
    if (sasBanner) sasBanner.hidden = !pending;
    if (unlocked && prevCryptoState != null && prevCryptoState !== CRYPTO_STATE.UNLOCKED) {
    log('Secure channel established.', 'ok');
    }
    prevCryptoState = state;
    renderDisplayState();
    updateControls();
  }

  function isCryptoUnlocked() {
    return Boolean(ctx.cryptoSession?.isUnlocked());
  }

  async function sendProtocolError(reason) {
    if (!ctx.transport?.isConnected()) return;
    await ctx.transport.send(buildMessage(MSG.ERROR, 0, textEncoder.encode(reason)));
  }

  function setSending(value) {
    ctx.isSending = value;
    ctx.receiver?.setBusy(value);
    updateControls();
  }

  function updateControls() {
    const connected = Boolean(ctx.transport?.isConnected());
    const unlocked = isCryptoUnlocked();
    const isReceiving = ctx.inboundHelloItemId != null;
    const fileBusy = ctx.isSending || isReceiving || Boolean(ctx.inboundMeta);
    const selectedFile = stagedFile();
    const pendingSelectedFile = pendingFileSend?.file === selectedFile;
    const canCancel = fileBusy;
    ctx.$('connectBtn').disabled = connected || isConnecting;
    ctx.$('disconnectBtn').disabled = !connected || isConnecting;
    ctx.$('textInput').disabled = false;
    ctx.$('sendTextBtn').disabled = !connected || !unlocked || ctx.isSending || isReceiving || !ctx.$('textInput').value.trim();
    ctx.$('fileInput').disabled = fileBusy;
    ctx.$('sendFileBtn').disabled = !connected || !unlocked || !selectedFile || (fileBusy && !pendingSelectedFile);
    const attachBtn = ctx.$('attachBtn');
    if (attachBtn) attachBtn.disabled = ctx.$('fileInput').disabled;
    ctx.$('cancelBtn').hidden = !canCancel;
    ctx.$('cancelBtn').disabled = !canCancel;
  }

  function metaTransferSize(meta) {
    return Number(meta?.totalCiphertextBytes ?? 0);
  }

  function metaChunkCount(meta) {
    return meta?.chunks ?? Math.max(1, Math.ceil(metaTransferSize(meta) / MAX_PAYLOAD));
  }

  function wireReceiverEvents() {
    // The USB page's setupProtocol hook contract creates the receiver here
    // (its test hooks call setupProtocol() on a null receiver), while the
    // BLE page's wireProtocolEvents hook contract pre-creates it (its test
    // hooks assign receiver=new ItemReceiver(...) first). Always disposing
    // and recreating satisfies both: a hook-supplied instance is replaced
    // with an identical one, and a null receiver is created fresh.
    ctx.receiver?.dispose();
    ctx.receiver = new ItemReceiver({ session: ctx.cryptoSession, sendFn: frame => ctx.transport.send(frame) });
    // ACK/NACK routing decision: the current ItemReceiver
    // (airbridge-item-receiver.js) IGNORES ack/nack frames outright (it
    // returns a resolved promise and emits no 'ack'/'nack' events), while
    // AirBridgeCryptoSession.receiveAck is single-shot (an accepted ACK
    // deletes its waiter) and receiveNack re-sends on EVERY accepted NACK.
    // So ACK/NACK must be routed EXACTLY ONCE, in handleInboundFrame below
    // (outbound stream control first, then the crypto handshake sender).
    // Wiring receiver.on('ack'/'nack') here would be dead code today and
    // double-processing (duplicate NACK resends) if the receiver ever
    // re-added those emissions — the BLE page's old dual wiring is dropped.
    ctx.receiver.on('hello', detail => {
      ctx.inboundHelloItemId = detail.itemId;
      transferLabel = 'awaiting meta…';
      setPageTransferPhase('Receiving', { bytes: 0n, total: null, reset: true });
      inboundTiming = createTimingRecorder({ operation: 'chat-receive', direction: 'inbound', mock: ctx.isMockMode, hardware: !ctx.isMockMode, sink: publishEvidence });
      inboundTiming.markFirstReport();
      updateControls();
      log(`Incoming HELLO${detail.itemId == null ? '' : ` item ${detail.itemId}`}.`);
    });
    ctx.receiver.on('meta', meta => {
      if (isCryptoUnlocked() && meta.kind !== 'encrypted-stream') {
        ctx.receiver.resetItem();
        sendProtocolError('plaintext metadata rejected').catch(() => {});
        ctx.cryptoSession?.clearKeys(CRYPTO_STATE.ABORTED);
        log('Rejected plaintext metadata after crypto unlock.', 'error');
        return;
      }
      ctx.inboundMeta = meta;
      transferLabel = meta.kind === 'text' ? 'text message' : String(meta.name || 'attachment');
      if (inboundTiming) {
        inboundTiming.record.operation = meta.kind === 'text' ? 'chat-text' : 'attachment';
        inboundTiming.record.itemId = meta.itemId ?? null;
        inboundTiming.record.details = { ...inboundTiming.record.details, itemId: meta.itemId, kind: meta.kind, name: meta.name };
        inboundTiming.record.metadata = inboundTiming.record.details;
      }
      updateControls();
      setPageTransferPhase('Receiving', { bytes: 0n, total: BigInt(metaTransferSize(meta)) });
      const totalChunks = metaChunkCount(meta);
      inboundTiming?.setBytes(metaTransferSize(meta));
      inboundMeter = createThroughputMeter();
      inboundMeter.tick(0, totalChunks, 0);
      log(`Receiving ${meta.kind || 'item'} ${meta.name || ''} (${formatBytes(metaTransferSize(meta))}).`);
      if (meta.kind !== 'text' && meta.kind !== 'encrypted-stream') {
        pendingReceiveRow = addMessage(ctx.$('transcript'), { side: 'peer', kind: 'attachment', meta, data: null, hash: null, pending: true, progress: { percent: 0, label: 'Receiving…' } });
      }
    });
    ctx.receiver.on('data', ({ ciphertextBytes, plaintextBytes }) => {
      setPageTransferPhase('Receiving', { bytes: BigInt(ciphertextBytes) });
      inboundTiming?.tickTransferBytes(Number(plaintextBytes));
    });
    ctx.receiver.on('item', handleReceivedItem);
    ctx.receiver.on('cancel', () => cancelActiveTransfer('peer'));
    ctx.receiver.on('rejected', () => log('Rejected peer HELLO while this side is busy.', 'warn'));
    ctx.receiver.on('error', error => {
      setPageTransferPhase('Failed');
      inboundTiming?.fail(error.message || error);
      inboundTiming = null;
      ctx.inboundMeta = null;
      ctx.inboundHelloItemId = null;
      updateControls();
      log(`Protocol error: ${error.message || error}`, 'error');
    });
    ctx.receiver.on('invalid', error => {
      inboundTiming?.fail(error.message || error);
      inboundTiming = null;
      log(`Invalid frame: ${error.message}`, 'error');
    });
  }

  async function handleInboundFrame(frame) {
    const control = routeReceiveControl(frame, ctx.receiver);
    if (control) return control;
    if (ctx.outbound?.receive(frame)) return;
    const msg = parseMessage(frame);
    if (!msg) {
      log('Malformed frame received.', 'error');
      return;
    }

    if (msg.type === MSG.ACK) {
      ctx.cryptoSession?.receiveAck(msg);
      return;
    }

    if (msg.type === MSG.NACK) {
      ctx.cryptoSession?.receiveNack(msg);
      return;
    }

    if (ctx.cryptoSession?.isCryptoFrame(msg)) {
      try {
        await ctx.cryptoSession.onMessage(msg);
      } catch (error) {
        log(`Crypto handshake failed: ${error.message}`, 'error');
      }
      return;
    }

    if ((msg.type === MSG.HELLO || msg.type === MSG.ITEM_META || msg.type === MSG.ITEM_DATA || msg.type === MSG.ITEM_DONE) && !isCryptoUnlocked()) {
      await sendProtocolError('Unsupported protocol version');
      ctx.cryptoSession?.clearKeys(CRYPTO_STATE.ABORTED);
      setPageTransferPhase('Failed');
      log('Unsupported protocol version', 'error');
      return;
    }

    if (msg.type === MSG.ITEM_DONE && frame.byteLength >= 9 && ctx.receiver?.snapshot().itemId === new DataView(frame.buffer, frame.byteOffset).getUint32(5)) setPageTransferPhase('Verifying');
    return ctx.receiver?.onMessage(frame);
  }

  async function handleReceivedItem(item) {
    await renderReceivedItem(ctx.$('transcript'), item);
    if (!item.isCurrent()) return;
    inboundTiming?.complete({ bundleBytes: Number(item.size), transferBytes: Number(item.size), direction: 'inbound' });
    inboundTiming = null;
    ctx.inboundHelloItemId = null;
    ctx.inboundMeta = null;
    updateControls();
    log(`Received ${item.kind} ${item.name}; hash ${shortHash(item.hash)} verified.`, 'ok');
    setPageTransferPhase('Ready', { bytes: item.size, total: item.size });
  }

  function createInPageMock() {
    const local = new LinkedMockTransport(config.mockNames.local);
    const peer = new LinkedMockTransport(config.mockNames.peer);
    local.link(peer);
    peer.link(local);
    installMockStreamPeer(peer, config.mockPeerRole);
    return local;
  }

  async function connect() {
    if (ctx.transport) await disconnect();
    isConnecting = true;
    setConnectionStatus('connecting');
    updateControls();
    try {
      ctx.transport = ctx.isMockMode
        ? (ctx.mockPeerName ? createPairedChatMock(config.endpoint, ctx.mockPeerName) : createInPageMock())
        : new config.TransportAdapter({ onDisconnect: handleDisconnected });
      await ctx.transport.connect();
      unsubscribeReceive = ctx.transport.onReceive(handleInboundFrame);
      if (typeof ctx.transport.onDisconnect === 'function') unsubscribeDisconnect = ctx.transport.onDisconnect(handleDisconnected);
      ctx.cryptoSession = new AirBridgeCryptoSession({ role: config.cryptoRole, sendFn: frame => ctx.transport.send(frame), onStateChange: updateCryptoPanel });
      wireReceiverEvents();
      ctx.outbound = createChatOutbound({ session: ctx.cryptoSession, transport: ctx.transport, setBusy: setSending, log });
      setConnectionStatus('connected', ctx.transport.getName());
      log(`Connected to ${ctx.transport.getName()}.`, 'ok');
      ctx.cryptoSession.start().catch(error => log(`Crypto handshake failed: ${error.message}`, 'error'));
    } catch (error) {
      setConnectionStatus('error');
      log(`Connection failed: ${error.message}`, 'error');
      ctx.transport = null;
    } finally {
      isConnecting = false;
      updateControls();
    }
  }

  async function disconnect() {
    ctx.receiver?.dispose();
    ctx.outbound?.dispose();
    unsubscribeReceive?.();
    unsubscribeDisconnect?.();
    unsubscribeReceive = null;
    unsubscribeDisconnect = null;
    const current = ctx.transport;
    ctx.transport = null;
    ctx.receiver = null;
    ctx.cryptoSession?.clearKeys();
    ctx.cryptoSession = null;
    setSending(false);
    if (current) {
      if (typeof current.dispose === 'function') await current.dispose();
      else if (current.isConnected?.()) await current.disconnect?.();
    }
    handleDisconnected();
  }

  function handleDisconnected() {
    const wasActive = ctx.isSending || ctx.inboundHelloItemId != null;
    discardTranscriptDownloads(ctx.$('transcript'));
    ctx.receiver?.dispose();
    ctx.outbound?.dispose();
    ctx.outbound = null;
    ctx.transport = null;
    ctx.receiver = null;
    ctx.cryptoSession?.clearKeys();
    ctx.cryptoSession = null;
    inboundTiming?.fail('disconnected');
    inboundTiming = null;
    ctx.inboundMeta = null;
    ctx.inboundHelloItemId = null;
    pendingReceiveRow = null;
    inboundMeter = null;
    updateCryptoPanel();
    setSending(false);
    if (wasActive) setPageTransferPhase('Cancelled');
    else resetTransferPanel();
    setConnectionStatus('disconnected');
    log('Disconnected.', 'warn');
    updateControls();
  }

  function markOutboundDelivered() {
    if ((ctx.$('panelState').dataset.phase || '') !== 'Ready') return;
    const bubbles = ctx.$('transcript').querySelectorAll('.message.you .bubble');
    bubbles[bubbles.length - 1]?.classList.add('acked');
  }

  async function sendText() {
    const text = ctx.$('textInput').value.trim();
    if (!text) return;
    transferLabel = 'text message';
    await ctx.outbound?.sendText(text);
    markOutboundDelivered();
  }

  function sendSelectedFile() {
    const file = stagedFile();
    if (!file) return pendingFileSend?.promise ?? Promise.resolve();
    if (pendingFileSend?.file === file) return pendingFileSend.promise;
    if (pendingFileSend) return pendingFileSend.promise;
    transferLabel = file.name;
    const promise = Promise.resolve(ctx.outbound?.sendFile(file))
      .then(() => markOutboundDelivered())
      .finally(() => {
        if (pendingFileSend?.file === file) pendingFileSend = null;
        if (stagedFile() === file) ctx.$('fileInput').value = '';
        refreshFileChip();
        updateControls();
      });
    pendingFileSend = { file, promise };
    refreshFileChip();
    updateControls();
    return promise;
  }

  async function cancelActiveTransfer(origin) {
    if (origin === 'peer' && isCancelling) {
      log('Already cancelling; ignoring peer cancel.', 'warn');
      return;
    }
    if (ctx.isSending && ctx.outbound) { ctx.outbound.cancel(); return; }
    const cancelItemId = ctx.inboundMeta?.itemId ?? ctx.inboundHelloItemId;
    const cancelBytes = ctx.inboundMeta?.size ?? 0;
    if (origin === 'user') {
      if (!ctx.inboundMeta && ctx.inboundHelloItemId == null) return;
      log('Transfer cancelled by user', 'warn');
      if (ctx.evidenceMode) recordCancel({ origin: 'user', endpoint: config.evidenceEndpoint, itemId: cancelItemId, transferBytes: cancelBytes, mock: ctx.isMockMode, hardware: !ctx.isMockMode }, { sink: publishEvidence });
      if (pendingReceiveRow) updateCardProgress(pendingReceiveRow, 0, 'Cancelled', 'cancelled');
      pendingReceiveRow = null;
      isCancelling = true;
      try {
        const cancellingReceiver = ctx.receiver;
        const cancelled = await cancellingReceiver?.cancel();
        if (ctx.receiver !== cancellingReceiver || cancelled?.itemId !== cancelItemId ||
          cancellingReceiver.snapshot().generation !== cancelled.generation) return;
        if (ctx.inboundMeta || ctx.inboundHelloItemId != null) {
          inboundTiming?.fail('cancelled-by-user');
          inboundTiming = null;
          ctx.receiver?.resetItem();
          ctx.inboundMeta = null;
          ctx.inboundHelloItemId = null;
        }
      } finally {
        isCancelling = false;
      }
    } else if (origin === 'peer') {
      inboundTiming?.fail('cancelled-by-peer');
      inboundTiming = null;
      ctx.inboundMeta = null;
      updateControls();
      log('Transfer cancelled by peer', 'warn');
      if (ctx.evidenceMode) recordCancel({ origin: 'peer', endpoint: config.evidenceEndpoint, itemId: cancelItemId, transferBytes: cancelBytes, mock: ctx.isMockMode, hardware: !ctx.isMockMode }, { sink: publishEvidence });
      if (pendingReceiveRow) updateCardProgress(pendingReceiveRow, 0, 'Cancelled by peer', 'cancelled');
      pendingReceiveRow = null;
    }
    setSending(false);
    setPageTransferPhase('Cancelled');
  }

  function stagedFile() {
    return ctx.$('fileInput').files?.[0] ?? null;
  }

  function refreshFileChip() {
    const file = stagedFile();
    const chip = ctx.$('fileChip');
    if (!chip) return;
    if (!file || pendingFileSend?.file === file) { chip.hidden = true; return; }
    const label = chip.querySelector('.file-chip-text');
    if (label) label.textContent = `${file.name} · ${formatBytes(file.size)}`;
    chip.hidden = false;
  }

  function clearTranscriptView() {
    ctx.outbound?.cancel();
    ctx.receiver?.resetItem();
    ctx.inboundMeta = null;
    ctx.inboundHelloItemId = null;
    inboundTiming?.fail('transcript-cleared');
    inboundTiming = null;
    clearTranscript(ctx.$('transcript'), currentEmptyPlaceholder());
    resetTransferPanel();
    updateControls();
  }

  function finishCommand() {
    const input = ctx.$('textInput');
    input.value = '';
    input.dispatchEvent(new Event('input', { bubbles: true }));
  }

  function runCommand(entered) {
    const [token] = entered.toLowerCase().split(/\s+/);
    const command = COMMAND_BY_TOKEN.get(token.slice(1));
    if (!command) {
      log(entered);
      log(`Unknown command: ${token} — try /help`, 'warn');
      finishCommand();
      return;
    }
    if (command.name === 'clearlog') {
      ctx.$('clearLogBtn').click();
      log(entered);
      finishCommand();
      return;
    }

    log(entered);
    switch (command.name) {
      case 'connect':
        if (isConnecting) log('Connection already in progress.', 'warn');
        else if (ctx.transport?.isConnected()) log('Already connected.', 'warn');
        else ctx.$('connectBtn').click();
        break;
      case 'disconnect':
        if (isConnecting) log('Connection in progress.', 'warn');
        else if (!ctx.transport?.isConnected()) log('Not connected.', 'warn');
        else ctx.$('disconnectBtn').click();
        break;
      case 'clear':
        ctx.$('clearTranscriptBtn').click();
        break;
      case 'cancel':
        if (ctx.$('cancelBtn').disabled) log('No active transfer.', 'warn');
        else ctx.$('cancelBtn').click();
        break;
      case 'accept':
        if (ctx.$('acceptSasBtn').disabled) log('No SAS confirmation is pending.', 'warn');
        else ctx.$('acceptSasBtn').click();
        break;
      case 'abort':
        if (ctx.$('abortCryptoBtn').disabled) log('No crypto session to abort.', 'warn');
        else ctx.$('abortCryptoBtn').click();
        break;
      case 'file':
        if (ctx.$('fileInput').disabled) log('Transfer in progress.', 'warn');
        else openFilePicker();
        break;
      case 'logs':
        document.querySelector('.diagnostic-rail')?.classList.toggle('logs-visible');
        break;
      case 'help':
        log('Commands:');
        for (const availableCommand of COMMANDS) log(`/${availableCommand.name} — ${availableCommand.description}`);
        log('//text — send literal /text');
        break;
    }
    finishCommand();
  }

  globalThis.AirBridgeEvidence = Object.freeze({
    records: evidenceRecords,
    record: publishEvidence,
    recordFlipperCounters: counters => publishEvidence(recordFlipperCounters(counters, { mock: ctx.isMockMode, hardware: !ctx.isMockMode })),
  });
  if (ctx.evidenceMode) {
    window.__airbridgeEvidence = evidenceRecords;
    window.recordAirBridgeFlipperCounters = globalThis.AirBridgeEvidence.recordFlipperCounters;
  }

  if (ctx.isMockMode) document.body.classList.add('mock');
  if (ctx.debugMode) document.body.classList.add('debug');
  const textForm = ctx.$('textForm');
  const textInput = ctx.$('textInput');
  const fileInput = ctx.$('fileInput');
  let filePickerOpen = false;
  const pageHasOpenDialog = () => Boolean(document.querySelector('dialog[open], [role="dialog"]:not([hidden])'));
  const openFilePicker = () => {
    filePickerOpen = true;
    try {
      fileInput.click();
    } catch (error) {
      filePickerOpen = false;
      log(`File picker unavailable: ${error instanceof Error ? error.message : 'request blocked'}`, 'warn');
    }
  };
  promptContext = document.createElement('div');
  promptContext.className = 'prompt-context';
  promptContext.setAttribute('aria-hidden', 'true');
  termPrompt = document.createElement('span');
  termPrompt.className = 'term-prompt';
  termPrompt.setAttribute('aria-hidden', 'true');
  termPrompt.textContent = '>';
  termMirror = document.createElement('span');
  termMirror.className = 'term-mirror';
  termMirror.setAttribute('aria-hidden', 'true');
  termMirrorTrack = document.createElement('span');
  termMirrorTrack.className = 'term-mirror-track';
  termMirrorValue = document.createElement('span');
  termMirrorValue.className = 'term-mirror-value';
  termGhost = document.createElement('span');
  termGhost.className = 'term-ghost';
  termMirrorTrack.append(termMirrorValue, termGhost);
  termMirror.append(termMirrorTrack);
  termCursor = document.createElement('span');
  termCursor.className = 'term-cursor';
  termCursor.setAttribute('aria-hidden', 'true');
  termCursor.textContent = '_';
  termSuggestions = document.createElement('div');
  termSuggestions.id = 'termSuggestions';
  termSuggestions.className = 'term-suggestions';
  termSuggestions.setAttribute('role', 'listbox');
  termSuggestions.setAttribute('aria-label', 'Command suggestions');
  termSuggestions.hidden = true;
  textInput.removeAttribute('placeholder');
  textInput.setAttribute('aria-autocomplete', 'list');
  textInput.setAttribute('aria-controls', termSuggestions.id);
  textInput.setAttribute('aria-expanded', 'false');
  textForm.prepend(promptContext);
  textForm.append(termSuggestions, termPrompt, termMirror, termCursor);
  const sasBannerText = document.querySelector('.sas-banner-text');
  if (sasBannerText) sasBannerText.textContent = 'If the six digits match on both screens, type /accept on both sides to open the secure channel. If they differ, type /abort and reconnect.';
  const consoleMenu = document.querySelector('.console-menu');
  const consoleSummary = consoleMenu?.querySelector('summary');
  if (consoleMenu) consoleMenu.open = false;
  if (consoleSummary) {
    consoleSummary.textContent = 'MODE=cli';
    consoleSummary.tabIndex = -1;
    consoleSummary.setAttribute('aria-disabled', 'true');
  }
  const statusline = document.querySelector('.statusline');
  const diagnosticRail = document.querySelector('.diagnostic-rail');
  if (diagnosticRail) diagnosticRail.classList.toggle('logs-visible', ctx.isMockMode || ctx.debugMode);
  if (statusline && diagnosticRail) diagnosticRail.prepend(statusline);
  renderTransferDetail();

  displayStateObserver = new MutationObserver(() => {
    renderDisplayState();
    renderTransferDetail();
  });
  displayStateObserver.observe(ctx.$('panelState'), { attributes: true, attributeFilter: ['data-phase'] });
  displayStateObserver.observe(ctx.$('throughput'), { attributes: true, attributeFilter: ['data-bytes', 'data-total'] });

  ctx.$('connectBtn').addEventListener('click', connect);
  ctx.$('disconnectBtn').addEventListener('click', disconnect);
  textForm.addEventListener('submit', event => {
    event.preventDefault();
    const entered = ctx.$('textInput').value.trim();
    if (entered.startsWith('/') && !entered.startsWith('//')) { runCommand(entered); return; }
    if (stagedFile()) {
      if (!ctx.transport?.isConnected() || !isCryptoUnlocked()) {
        log('Staged file not sent — /connect and verify first.', 'warn');
        return;
      }
      if (ctx.$('sendFileBtn').disabled) { log('Transfer in progress.', 'warn'); return; }
      finishCommand();
      sendSelectedFile();
      return;
    }
    if (entered.startsWith('//')) {
      ctx.$('textInput').value = entered.slice(1);
      ctx.$('textInput').dispatchEvent(new Event('input', { bubbles: true }));
      if (!ctx.$('sendTextBtn').disabled) sendText();
      dismissAutocomplete(true);
      updateTerminalCursor();
      return;
    }
    if (!ctx.$('sendTextBtn').disabled) sendText();
  });
  textInput.addEventListener('keydown', event => {
    if (!termSuggestions.hidden) {
      if (event.key === 'Tab') {
        event.preventDefault();
        acceptAutocomplete();
        return;
      }
      if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
        event.preventDefault();
        const direction = event.key === 'ArrowDown' ? 1 : -1;
        completionIndex = (completionIndex + direction + completionMatches.length) % completionMatches.length;
        renderAutocomplete();
        updateTerminalCursor();
        return;
      }
      if (event.key === 'Escape') {
        event.preventDefault();
        dismissAutocomplete(true);
        updateTerminalCursor();
        return;
      }
    }
    if (event.key !== 'Enter' || event.isComposing || event.altKey || event.ctrlKey || event.metaKey || event.shiftKey) return;
    event.preventDefault();
    textForm.requestSubmit();
  });
  textInput.addEventListener('input', () => { refreshAutocomplete(); updateControls(); updateTerminalCursor(); });
  textInput.addEventListener('focus', updateTerminalCursor);
  textInput.addEventListener('blur', () => { dismissAutocomplete(true); updateTerminalCursor(); });
  textInput.addEventListener('scroll', updateTerminalCursor);
  document.addEventListener('keydown', event => {
    const targetIsInteractive = event.target instanceof Element && event.target.closest('input, textarea, select, button, a, summary, [contenteditable]');
    if (targetIsInteractive || event.altKey || event.ctrlKey || event.metaKey || event.isComposing || event.key.length !== 1 || filePickerOpen || pageHasOpenDialog()) return;
    textInput.focus();
  });
  window.addEventListener('resize', updateTerminalCursor);
  termCursorResizeObserver = new ResizeObserver(updateTerminalCursor);
  termCursorResizeObserver.observe(textForm);
  ctx.$('attachBtn').addEventListener('click', openFilePicker);
  ctx.$('fileChipClear').addEventListener('click', () => { ctx.$('fileInput').value = ''; refreshFileChip(); updateControls(); });
  fileInput.addEventListener('change', () => {
    filePickerOpen = false;
    const file = stagedFile();
    if (!file) {
      refreshFileChip();
      updateControls();
      return;
    }
    if (!ctx.transport?.isConnected() || !isCryptoUnlocked()) {
      refreshFileChip();
      updateControls();
      log(`Staged ${file.name} (${formatBytes(file.size)}) — /connect and verify, then Enter to send`);
      return;
    }
    if (pendingFileSend?.file === file) return;
    if (ctx.isSending || ctx.inboundHelloItemId != null || ctx.inboundMeta) {
      log('busy — /cancel or wait', 'warn');
      fileInput.value = '';
      refreshFileChip();
      updateControls();
      return;
    }
    log(`Sending ${file.name} (${formatBytes(file.size)})…`);
    sendSelectedFile();
  });
  fileInput.addEventListener('cancel', () => { filePickerOpen = false; });
  ctx.$('sendFileBtn').addEventListener('click', sendSelectedFile);
  ctx.$('cancelBtn').addEventListener('click', () => { cancelActiveTransfer('user'); });
  ctx.$('acceptSasBtn').addEventListener('click', () => ctx.cryptoSession?.acceptSas().catch(error => log(`SAS accept failed: ${error.message}`, 'error')));
  ctx.$('abortCryptoBtn').addEventListener('click', () => ctx.cryptoSession?.rejectSas().catch(error => log(`Crypto abort failed: ${error.message}`, 'error')));
  ctx.$('clearTranscriptBtn').addEventListener('click', clearTranscriptView);
  ctx.$('clearLogBtn').addEventListener('click', () => ctx.logger.clear());
  updateControls();
  updateCryptoPanel();
  refreshAutocomplete();
  updateTerminalCursor();
  log(ctx.isMockMode ? 'Mock mode ready. Click Connect to start the in-page verifier.' : config.readyLog);
  window.addEventListener('pagehide', () => { cancelAnimationFrame(termCursorFrame); termCursorResizeObserver.disconnect(); window.removeEventListener('resize', updateTerminalCursor); ctx.transcriptController.dispose(); displayStateObserver.disconnect(); ctx.receiver?.dispose(); ctx.outbound?.cancel(); discardTranscriptDownloads(ctx.$('transcript')); });

  return {
    connect,
    disconnect,
    handleDisconnected,
    wireReceiverEvents,
    handleInboundFrame,
    updateCryptoPanel,
    updateControls,
    setSending,
    sendText,
    sendSelectedFile,
    cancelActiveTransfer,
    clearTranscriptView,
  };
}
