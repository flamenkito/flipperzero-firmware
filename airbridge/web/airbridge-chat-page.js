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
import { createTerminalConsole, TERMINAL_COMMANDS } from './airbridge-terminal.js';
import { renderReceivedItem, routeReceiveControl } from './airbridge-chat-receive.js';
import { installMockStreamPeer } from './airbridge-mock-stream-peer.js';
import { createPairedChatMock } from './airbridge-chat-mock.js';
import { createTimingRecorder, recordCancel, recordFlipperCounters } from './airbridge-evidence.js';
import {
  AirBridgeCryptoSession,
  buildMessage,
  CRYPTO_STATE,
  ItemReceiver,
  MSG,
  parseMessage,
} from './airbridge-protocol.js';
import {
  addMessage,
  clearTranscript,
  discardTranscriptDownloads,
  formatBytes,
  setTransferPhase,
  shortHash,
  updateCardProgress,
} from './airbridge-ui.js';

const textEncoder = new TextEncoder();

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
  let inboundTiming = null;
  let displayStateObserver = null;
  let promptContext = null;
  let receiveProgress = null;
  let terminal = null;
  let pendingFileSend = null;
  const evidenceRecords = [];

  const log = (message, level) => {
    ctx.log(message, level);
    if (level === 'error' || level === 'warn') terminal?.writeOutput(message, level);
  };
  const feedback = (message, level = '') => terminal?.writeOutput(message, level);

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
    document.querySelector('.diagnostic-rail')?.classList.toggle('transfer-active', active || phase === 'Failed' || phase === 'Cancelled');
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
    const connection = ctx.$('connectionStatus').textContent;
    const crypto = ctx.$('cryptoState').textContent;
    const sas = ctx.$('sasCode').textContent;
    const failed = ctx.$('statusPill').classList.contains('status-error');
    const security = crypto === 'pending'
      ? `SAS ${sas} ${ctx.$('acceptSasBtn').disabled ? 'waiting-peer' : 'pending'}`
      : crypto === 'verified' ? `verified SAS ${sas}` : crypto;
    const summary = `${failed ? 'error' : connection} ${security}${ctx.isMockMode ? ' [mock]' : ''}`;
    if (promptContext.textContent !== summary) promptContext.textContent = summary;
    promptContext.title = `${ctx.$('deviceName').textContent}: ${promptContext.textContent}`;
    if (receiveProgress) {
      const phase = ctx.$('panelState').dataset.phase;
      const active = ctx.inboundHelloItemId != null && (phase === 'Receiving' || phase === 'Verifying');
      receiveProgress.hidden = !active;
      const total = BigInt(ctx.$('throughput').dataset.total || '0');
      const bytes = BigInt(ctx.$('throughput').dataset.bytes || '0');
      const percent = total > 0n ? Math.min(100, Number(bytes * 100n / total)) : null;
      const label = phase === 'Verifying' ? 'RX verify' : percent == null ? 'RX ...' : `RX ${percent}%`;
      if (receiveProgress.textContent !== label) receiveProgress.textContent = label;
      receiveProgress.setAttribute('aria-valuetext', phase === 'Verifying' ? 'Verifying received item' : percent == null ? 'Waiting for metadata' : `${percent}% received`);
      if (percent != null) receiveProgress.setAttribute('aria-valuenow', String(percent));
      else receiveProgress.removeAttribute('aria-valuenow');
    }
  }

  let prevCryptoState = null;

  function updateCryptoPanel(status = ctx.cryptoSession?.getStatus()) {
    const state = status?.state ?? CRYPTO_STATE.REQUIRED;
    const unlocked = state === CRYPTO_STATE.UNLOCKED;
    if (!unlocked) ctx.outbound?.cancel();
    const pending = state === CRYPTO_STATE.SAS_PENDING;
    const aborted = state === CRYPTO_STATE.ABORTED;
    const exchanging = state === CRYPTO_STATE.HANDSHAKING;
    ctx.$('cryptoState').textContent = unlocked ? 'verified' : pending ? 'pending' : aborted ? 'aborted' : exchanging ? 'key-exchange' : 'locked';
    ctx.$('cryptoPill').className = `status-pill crypto-pill ${unlocked ? 'crypto-unlocked' : pending ? 'crypto-pending' : aborted ? 'crypto-aborted' : 'crypto-locked'}`;
    ctx.$('cryptoLock').textContent = 'SEC';
    ctx.$('sasCode').textContent = status?.sas ?? '------';
    document.querySelector('.sas-verified').hidden = !unlocked;
    ctx.$('acceptSasBtn').disabled = !pending || status.localAccepted;
    ctx.$('abortCryptoBtn').disabled = !ctx.cryptoSession || state === CRYPTO_STATE.REQUIRED;
    if (state !== prevCryptoState) {
      if (exchanging) feedback(`Waiting for ${config.endpoint === 'usb' ? 'BLE' : 'USB'} peer key exchange.`);
      else if (pending) feedback(`verify peer: ${status.sas}`, 'warn');
    }
    if (unlocked && prevCryptoState != null && prevCryptoState !== CRYPTO_STATE.UNLOCKED) {
      log('Secure channel established.', 'ok');
      feedback('Secure channel established.', 'ok');
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
    terminal?.refreshDraft();
  }

  function metaTransferSize(meta) {
    return BigInt(meta?.totalCiphertextBytes ?? 0);
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
      transferLabel = meta.kind === 'text' ? 'text message' : meta.kind === 'encrypted-stream' ? 'incoming item' : String(meta.name || 'attachment');
      if (inboundTiming) {
        inboundTiming.record.operation = meta.kind === 'text' ? 'chat-text' : 'attachment';
        inboundTiming.record.itemId = meta.itemId ?? null;
        inboundTiming.record.details = { ...inboundTiming.record.details, itemId: meta.itemId, kind: meta.kind, name: meta.name };
        inboundTiming.record.metadata = inboundTiming.record.details;
      }
      updateControls();
      setPageTransferPhase('Receiving', { bytes: 0n, total: metaTransferSize(meta) });
      inboundTiming?.setBytes(Number(metaTransferSize(meta)));
      log(`Receiving ${meta.kind || 'item'} ${meta.name || ''} (${formatBytes(metaTransferSize(meta))}).`);
      if (meta.kind !== 'text' && meta.kind !== 'encrypted-stream') {
        pendingReceiveRow = addMessage(ctx.$('transcript'), { side: 'peer', kind: 'attachment', meta, data: null, hash: null, pending: true, progress: { percent: 0, label: 'Receiving…' } });
      }
    });
    ctx.receiver.on('data', ({ receivedCiphertextBytes, plaintextBytes }) => {
      setPageTransferPhase('Receiving', { bytes: receivedCiphertextBytes });
      inboundTiming?.tickTransferBytes(Number(plaintextBytes));
    });
    ctx.receiver.on('item', handleReceivedItem);
    ctx.receiver.on('cancel', () => cancelActiveTransfer('peer'));
    ctx.receiver.on('rejected', () => log('Rejected peer HELLO while this side is busy.', 'warn'));
    ctx.receiver.on('error', error => {
      setPageTransferPhase('Failed', { error: error.message || String(error) });
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
    let attemptedTransport = null;
    try {
      ctx.transport = ctx.isMockMode
        ? (ctx.mockPeerName ? createPairedChatMock(config.endpoint, ctx.mockPeerName) : createInPageMock())
        : new config.TransportAdapter({ onDisconnect: handleDisconnected, onStatus: message => feedback(message) });
      attemptedTransport = ctx.transport;
      await ctx.transport.connect();
      unsubscribeReceive = ctx.transport.onReceive(handleInboundFrame);
      if (typeof ctx.transport.onDisconnect === 'function') unsubscribeDisconnect = ctx.transport.onDisconnect(handleDisconnected);
      const session = new AirBridgeCryptoSession({ role: config.cryptoRole, sendFn: frame => attemptedTransport.send(frame), onStateChange: updateCryptoPanel });
      ctx.cryptoSession = session;
      wireReceiverEvents();
      ctx.outbound = createChatOutbound({ session: ctx.cryptoSession, transport: ctx.transport, setBusy: setSending, log });
      setConnectionStatus('connected', ctx.transport.getName());
      log(`Connected to ${ctx.transport.getName()}.`, 'ok');
      session.start().catch(error => {
        if (ctx.cryptoSession !== session) return;
        log(`Key exchange failed: ${error.message}. Use :d then :c to reconnect.`, 'error');
      });
    } catch (error) {
      if (attemptedTransport) {
        try {
          if (typeof attemptedTransport.dispose === 'function') await attemptedTransport.dispose();
          else await attemptedTransport.disconnect?.();
        } catch (cleanupError) { log(`Connection cleanup failed: ${cleanupError.message}`, 'warn'); }
      }
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
    const expired = ctx.$('transcript').querySelectorAll('.attachment-card:has(a[download])').length;
    discardTranscriptDownloads(ctx.$('transcript'));
    if (expired) feedback(`${expired} attachment download(s) expired on disconnect. Ask your peer to resend any unsaved files.`, 'warn');
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
    // Keep one empty buffer row for the NORMAL-mode cursor.
    clearTranscript(ctx.$('transcript'), ' ');
    resetTransferPanel();
    updateControls();
  }

  function runCommand(name) {
    const unavailable = message => { feedback(message, 'warn'); return false; };
    switch (name) {
      case 'connect':
        if (isConnecting) return unavailable('Connection already in progress.');
        if (ctx.transport?.isConnected()) return unavailable('Already connected. Use :d first to reconnect.');
        feedback(`Connecting ${config.endpoint.toUpperCase()}… Select the device running Pocket AirBridge when prompted.`);
        ctx.$('connectBtn').click();
        break;
      case 'disconnect':
        if (isConnecting) return unavailable('Connection in progress. Finish or dismiss the device picker.');
        if (!ctx.transport?.isConnected()) return unavailable('Already disconnected.');
        ctx.$('disconnectBtn').click();
        break;
      case 'clear':
        if (ctx.isSending || ctx.inboundHelloItemId != null) return unavailable('Transfer in progress. Use :x before clearing the transcript.');
        ctx.$('clearTranscriptBtn').click();
        terminal.clearOutput();
        feedback('Transcript and its downloads cleared.');
        break;
      case 'clearlog':
        ctx.$('clearLogBtn').click();
        feedback('Diagnostics cleared.');
        break;
      case 'cancel':
        if (ctx.$('cancelBtn').disabled) return unavailable('No active transfer. Use :uf to remove a selected file.');
        ctx.$('cancelBtn').click();
        break;
      case 'accept':
        if (ctx.cryptoSession?.getStatus().state === CRYPTO_STATE.SAS_PENDING && ctx.cryptoSession.getStatus().localAccepted) return unavailable('Accepted here. Waiting for your peer to accept.');
        if (ctx.$('acceptSasBtn').disabled) return unavailable('No code is ready to confirm. Connect both endpoints first.');
        ctx.$('acceptSasBtn').click();
        feedback('Code accepted here. Waiting for peer confirmation.');
        break;
      case 'abort':
        if (ctx.$('abortCryptoBtn').disabled) return unavailable('No verification session to abort.');
        ctx.$('abortCryptoBtn').click();
        feedback('Ending verification. Use :d then :c to reconnect.');
        break;
      case 'file':
        if (ctx.$('fileInput').disabled) return unavailable('Transfer in progress. Use :x or wait.');
        openFilePicker();
        break;
      case 'sendfile':
        if (!stagedFile()) return unavailable('No attachment selected. Use :f to choose one.');
        if (!ctx.transport?.isConnected() || !isCryptoUnlocked()) return unavailable('Connect and confirm matching codes before sending. Your selected file is kept.');
        if (ctx.$('sendFileBtn').disabled) return unavailable('Transfer in progress. Use :x or wait.');
        feedback(`Sending ${stagedFile().name}…`);
        sendSelectedFile();
        break;
      case 'unfile':
        if (pendingFileSend) return unavailable('Attachment is sending. Use :x to cancel.');
        ctx.$('fileChipClear').click();
        feedback('Selected attachment removed.');
        break;
      case 'logs': {
        const rail = document.querySelector('.diagnostic-rail');
        const visible = rail.classList.toggle('logs-visible');
        terminal.closeOutput(false);
        feedback(visible ? 'airbridge-log [readonly]  j/k scroll  :q close' : 'Log split closed.');
        break;
      }
      case 'quit':
        terminal.closeOutput(false);
        document.querySelector('.diagnostic-rail').classList.remove('logs-visible');
        feedback('Split closed. Connection kept.');
        break;
      case 'help':
        document.querySelector('.diagnostic-rail').classList.remove('logs-visible');
        terminal.showOutput([
          '*airbridge-help*  Pocket AirBridge',
          '',
          'NORMAL  i insert   : command   j/k move   gg first   G last',
          '        Ctrl-d / Ctrl-u scroll half a screen',
          'INSERT  Enter sends literal text; Esc keeps your draft.',
          'COMMAND Enter runs; Tab completes; arrows select / recall history.',
          '        Esc or Backspace on an empty command returns to NORMAL.',
          '',
          ...TERMINAL_COMMANDS.map(command => `:${command.short.padEnd(3)} :${command.name.padEnd(11)} ${command.description}`),
          '',
          'Files: :f chooses; :s sends. Enter sends only your message.',
          'Save downloads before :d or :cl; these discard received files.',
          'This is a chat buffer. Vim text operators and file editing are unavailable.',
          ...(ctx.isMockMode ? ['', 'MOCK: simulated transport; no physical device.', 'Use ?mock=1&mockPeer=demo on both pages for paired browser QA.'] : []),
        ]);
        feedback('airbridge-help [readonly]  j/k scroll  :q close');
        break;
    }
    return true;
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
  const openFilePicker = () => {
    try { fileInput.click(); }
    catch (error) { feedback(`File picker unavailable: ${error.message}`, 'error'); }
  };
  const requestSendText = () => {
    if (!textInput.value.trim()) return;
    if (!ctx.transport?.isConnected()) { feedback('Disconnected. Press Esc, then :c to connect. Your draft is kept.', 'warn'); return; }
    if (!isCryptoUnlocked()) { feedback('Compare both codes, then Esc :a to confirm. Your draft is kept.', 'warn'); return; }
    if (ctx.isSending || ctx.inboundHelloItemId != null) { feedback('Transfer in progress. Your draft is kept; send it when the transfer finishes.', 'warn'); return; }
    sendText();
  };
  terminal = createTerminalConsole({ form: textForm, input: textInput, executeCommand: runCommand, sendMessage: requestSendText });
  promptContext = document.createElement('div');
  promptContext.className = 'prompt-context';
  promptContext.setAttribute('aria-label', 'Connection and encryption');
  promptContext.setAttribute('role', 'status');
  receiveProgress = document.createElement('span');
  receiveProgress.id = 'receiveProgress';
  receiveProgress.className = 'terminal-receive-progress';
  receiveProgress.hidden = true;
  receiveProgress.setAttribute('role', 'progressbar');
  receiveProgress.setAttribute('aria-label', 'Receiving');
  receiveProgress.setAttribute('aria-valuemin', '0');
  receiveProgress.setAttribute('aria-valuemax', '100');
  ctx.$('terminalRuler').before(promptContext, receiveProgress);
  const diagnosticRail = document.querySelector('.diagnostic-rail');
  if (diagnosticRail) diagnosticRail.classList.toggle('logs-visible', ctx.debugMode);
  renderTransferDetail();

  displayStateObserver = new MutationObserver(() => {
    renderDisplayState();
    renderTransferDetail();
  });
  displayStateObserver.observe(ctx.$('panelState'), { attributes: true, attributeFilter: ['data-phase'] });
  displayStateObserver.observe(ctx.$('throughput'), { attributes: true, attributeFilter: ['data-bytes', 'data-total'] });

  ctx.$('connectBtn').addEventListener('click', connect);
  ctx.$('disconnectBtn').addEventListener('click', disconnect);
  textInput.addEventListener('input', updateControls);
  ctx.$('attachBtn').addEventListener('click', openFilePicker);
  ctx.$('fileChipClear').addEventListener('click', () => { fileInput.value = ''; refreshFileChip(); updateControls(); });
  fileInput.addEventListener('change', () => {
    refreshFileChip();
    updateControls();
    const file = stagedFile();
    if (file) feedback(`Selected ${file.name} (${formatBytes(file.size)}). Use :s to send, :uf to remove. Your message draft is kept.`);
  });
  ctx.$('sendFileBtn').addEventListener('click', sendSelectedFile);
  ctx.$('cancelBtn').addEventListener('click', () => { cancelActiveTransfer('user'); });
  ctx.$('acceptSasBtn').addEventListener('click', () => {
    const accepting = ctx.cryptoSession?.acceptSas();
    updateCryptoPanel();
    accepting?.then(() => updateCryptoPanel()).catch(error => log(`Code confirmation failed: ${error.message}`, 'error'));
  });
  ctx.$('abortCryptoBtn').addEventListener('click', () => ctx.cryptoSession?.rejectSas().catch(error => log(`Verification abort failed: ${error.message}`, 'error')));
  ctx.$('clearTranscriptBtn').addEventListener('click', clearTranscriptView);
  ctx.$('clearLogBtn').addEventListener('click', () => ctx.logger.clear());
  updateControls();
  updateCryptoPanel();
  log(ctx.isMockMode ? 'Mock mode ready. Use :c to connect.' : config.readyLog);
  window.addEventListener('pagehide', () => { terminal.dispose(); ctx.transcriptController.dispose(); displayStateObserver.disconnect(); ctx.receiver?.dispose(); ctx.outbound?.cancel(); discardTranscriptDownloads(ctx.$('transcript')); });

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
