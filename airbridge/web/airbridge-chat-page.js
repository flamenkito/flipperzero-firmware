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
    ctx.$('displayState').dataset.state = state;
    ctx.$('displayState').textContent = `state=${state}`;
    updateHeaderStatus(state);
    updateStateSurfaces(connected, unlocked);
  }

  function updateHeaderStatus(plainState = 'idle') {
    const banner = ctx.$('headerStatus');
    if (!banner) return;
    // When the statusline rail is visible (mock/debug) it already shows every
    // fact, so the header carries only the state; in production it is the sole summary.
    const railVisible = document.body.classList.contains('mock') || document.body.classList.contains('debug');
    if (railVisible) {
      banner.textContent = plainState;
      return;
    }
    const device = ctx.$('deviceName')?.textContent?.trim() || 'No device';
    banner.textContent = `${device} · ${plainState}`;
  }

  function currentEmptyPlaceholder(connected = Boolean(ctx.transport?.isConnected()), unlocked = isCryptoUnlocked()) {
    if (!connected) return 'Connect, then send a message or attachment.';
    if (!unlocked) return 'Compare the SAS code with your peer to unlock the channel.';
    return 'Secure channel ready — send a message or attachment.';
  }

  function updateStateSurfaces(connected, unlocked) {
    const emptyBubble = ctx.$('transcript').querySelector('.message.system.empty .bubble');
    if (emptyBubble) {
      const body = emptyBubble.lastElementChild;
      if (body?.previousElementSibling?.classList?.contains('meta-line')) body.textContent = currentEmptyPlaceholder(connected, unlocked);
    }
    const hint = ctx.$('attachmentLimits');
    if (hint) hint.textContent = unlocked ? 'Channel verified. Files stay in memory only.' : 'Files stay in memory until verified.';
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
    const canCancel = ctx.isSending || isReceiving || Boolean(ctx.inboundMeta);
    ctx.$('connectBtn').disabled = connected || isConnecting;
    ctx.$('disconnectBtn').disabled = !connected || isConnecting;
    ctx.$('textInput').disabled = !connected || !unlocked || ctx.isSending || isReceiving;
    ctx.$('sendTextBtn').disabled = !connected || !unlocked || ctx.isSending || isReceiving || !ctx.$('textInput').value.trim();
    ctx.$('fileInput').disabled = !connected || !unlocked || ctx.isSending || isReceiving;
    ctx.$('sendFileBtn').disabled = !connected || !unlocked || ctx.isSending || isReceiving || !ctx.$('fileInput').files?.length;
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
      transferLabel = 'incoming item';
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
      setPageTransferPhase('Receiving', { bytes: 0n, total: null });
      const totalChunks = metaChunkCount(meta);
      inboundTiming?.setBytes(metaTransferSize(meta));
      inboundMeter = createThroughputMeter();
      inboundMeter.tick(0, totalChunks, 0);
      log(`Receiving ${meta.kind || 'item'} ${meta.name || ''} (${formatBytes(metaTransferSize(meta))}).`);
      if (meta.kind !== 'text' && meta.kind !== 'encrypted-stream') {
        pendingReceiveRow = addMessage(ctx.$('transcript'), { side: 'peer', kind: 'attachment', meta, data: null, hash: null, pending: true, progress: { percent: 0, label: 'Receiving…' } });
      }
    });
    ctx.receiver.on('data', ({ plaintextBytes }) => {
      setPageTransferPhase('Receiving', { bytes: plaintextBytes, total: null });
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

  async function sendSelectedFile() {
    const file = ctx.$('fileInput').files?.[0];
    if (!file) return;
    transferLabel = file.name;
    await ctx.outbound?.sendFile(file);
    markOutboundDelivered();
    ctx.$('fileInput').value = '';
    refreshFileChip();
    updateControls();
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
    if (!file) { chip.hidden = true; return; }
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

  displayStateObserver = new MutationObserver(() => {
    renderDisplayState();
    renderTransferDetail();
  });
  displayStateObserver.observe(ctx.$('panelState'), { attributes: true, attributeFilter: ['data-phase'] });
  displayStateObserver.observe(ctx.$('throughput'), { attributes: true, attributeFilter: ['data-bytes', 'data-total'] });

  ctx.$('connectBtn').addEventListener('click', connect);
  ctx.$('disconnectBtn').addEventListener('click', disconnect);
  ctx.$('textForm').addEventListener('submit', event => {
    event.preventDefault();
    if (stagedFile() && !ctx.$('textInput').value.trim()) { if (!ctx.$('sendFileBtn').disabled) sendSelectedFile(); return; }
    if (!ctx.$('sendTextBtn').disabled) sendText();
  });
  ctx.$('textInput').addEventListener('input', updateControls);
  ctx.$('attachBtn').addEventListener('click', () => ctx.$('fileInput').click());
  ctx.$('fileChipClear').addEventListener('click', () => { ctx.$('fileInput').value = ''; refreshFileChip(); updateControls(); });
  ctx.$('fileInput').addEventListener('change', () => { refreshFileChip(); updateControls(); });
  ctx.$('sendFileBtn').addEventListener('click', sendSelectedFile);
  ctx.$('cancelBtn').addEventListener('click', () => { cancelActiveTransfer('user'); });
  ctx.$('acceptSasBtn').addEventListener('click', () => ctx.cryptoSession?.acceptSas().catch(error => log(`SAS accept failed: ${error.message}`, 'error')));
  ctx.$('abortCryptoBtn').addEventListener('click', () => ctx.cryptoSession?.rejectSas().catch(error => log(`Crypto abort failed: ${error.message}`, 'error')));
  ctx.$('clearTranscriptBtn').addEventListener('click', clearTranscriptView);
  ctx.$('clearLogBtn').addEventListener('click', () => ctx.logger.clear());
  updateControls();
  updateCryptoPanel();
  log(ctx.isMockMode ? 'Mock mode ready. Click Connect to start the in-page verifier.' : config.readyLog);
  window.addEventListener('pagehide', () => { ctx.transcriptController.dispose(); displayStateObserver.disconnect(); ctx.receiver?.dispose(); ctx.outbound?.cancel(); discardTranscriptDownloads(ctx.$('transcript')); });

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
