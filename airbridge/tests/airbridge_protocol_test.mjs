import assert from 'node:assert/strict';
import { test } from 'node:test';

import {
  AirBridgeCryptoSession,
  buildMessage,
  CRYPTO_ROLE,
  CRYPTO_STATE,
  encodeMeta,
  ItemReceiver,
  ItemSender,
  MAX_PAYLOAD,
  MSG,
  parseMessage,
  readAckSeq,
  sha256,
} from '../web/airbridge-protocol.js';

function createDeferred() {
  let resolve;
  const promise = new Promise(settle => { resolve = settle; });
  return { promise, resolve };
}

function itemIdPayload(itemId) {
  return new Uint8Array([
    (itemId >>> 24) & 0xff,
    (itemId >>> 16) & 0xff,
    (itemId >>> 8) & 0xff,
    itemId & 0xff,
  ]);
}

async function prepareReceiverItem(receiver, itemId, text) {
  const data = new TextEncoder().encode(text);
  const meta = {
    kind: 'text',
    itemId,
    name: `${itemId}.txt`,
    mimeType: 'text/plain',
    size: data.length,
    chunks: chunkBytes(data).length,
    hash: `sha256:${await sha256(data)}`,
  };

  await receiver.onMessage(buildMessage(MSG.HELLO, 0, itemIdPayload(itemId)));
  const metaFragments = encodeMeta(meta);
  for (let seq = 0; seq < metaFragments.length; seq++) {
    await receiver.onMessage(buildMessage(MSG.ITEM_META, seq, metaFragments[seq]));
  }
  const dataChunks = chunkBytes(data);
  for (let seq = 0; seq < dataChunks.length; seq++) {
    await receiver.onMessage(buildMessage(MSG.ITEM_DATA, seq, dataChunks[seq]));
  }
  return buildMessage(MSG.ITEM_DONE, 0);
}

function countDoneAcks(messages) {
  return messages.filter(message => message.type === MSG.ACK && readAckSeq(message)?.type === MSG.ITEM_DONE).length;
}

function chunkBytes(bytes) {
  const chunks = [];
  for (let offset = 0; offset < bytes.length; offset += MAX_PAYLOAD) {
    chunks.push(bytes.slice(offset, offset + MAX_PAYLOAD));
  }
  return chunks.length ? chunks : [new Uint8Array(0)];
}

function createEndpoint(role) {
  const endpoint = { peer: null, sent: [], errors: [], received: [] };
  endpoint.sender = new ItemSender(frame => endpoint.send(frame), { timeoutMs: 300, retries: 1 });
  endpoint.receiver = new ItemReceiver({ sendFn: frame => endpoint.send(frame), nackDelayMs: 25 });
  endpoint.session = new AirBridgeCryptoSession({
    role,
    sendFn: frame => endpoint.send(frame),
    timeoutMs: 300,
    retries: 1,
  });
  endpoint.receiver.setItemValidator(({ meta, data }) => endpoint.session.decryptItem(meta, data));
  endpoint.receiver.on('item', detail => endpoint.received.push(detail));
  endpoint.receiver.on('error', error => endpoint.errors.push(error.message || String(error)));
  endpoint.send = async frame => {
    const copy = new Uint8Array(frame);
    endpoint.sent.push(parseMessage(copy));
    await endpoint.peer.receive(copy);
  };
  endpoint.receive = async frame => {
    const message = parseMessage(frame);
    if (message.type === MSG.ACK) {
      if (!endpoint.sender.receiveAck(message)) endpoint.session.receiveAck(message);
      return;
    }
    if (message.type === MSG.NACK) {
      if (!endpoint.sender.receiveNack(message)) endpoint.session.receiveNack(message);
      return;
    }
    if (endpoint.session.isCryptoFrame(message)) {
      await endpoint.session.onMessage(message);
      return;
    }
    if (message.type === MSG.ERROR) {
      endpoint.sender.receiveError(message);
      endpoint.session.clearKeys(CRYPTO_STATE.ABORTED);
      return;
    }
    await endpoint.receiver.onMessage(message);
  };
  return endpoint;
}

async function unlockCryptoPair() {
  const usb = createEndpoint(CRYPTO_ROLE.USB);
  const ble = createEndpoint(CRYPTO_ROLE.BLE);
  usb.peer = ble;
  ble.peer = usb;
  await ble.session.start();
  await usb.session.start();
  assert.equal(usb.session.getStatus().sas, ble.session.getStatus().sas);
  await usb.session.acceptSas();
  await ble.session.acceptSas();
  assert.equal(usb.session.isUnlocked(), true);
  assert.equal(ble.session.isUnlocked(), true);
  return { usb, ble };
}

test('recomputed outer hash cannot ACK tampered encrypted ITEM_DONE', async () => {
  const { usb, ble } = await unlockCryptoPair();
  const data = new TextEncoder().encode('authenticated plaintext');
  const hash = await sha256(data);
  const meta = {
    kind: 'text',
    itemId: 9001,
    name: 'tampered.txt',
    mimeType: 'text/plain',
    size: data.length,
    chunks: 1,
    hash: `sha256:${hash}`,
  };
  const encrypted = await usb.session.encryptItem(meta, data);
  const tampered = encrypted.data.slice();
  tampered[0] ^= 0x80;
  const tamperedMeta = { ...encrypted.meta, encryptedSha256: `sha256:${await sha256(tampered)}` };

  await usb.sender.sendHello(meta.itemId);
  await usb.sender.sendItemMeta(tamperedMeta);
  await usb.sender.sendItemData(chunkBytes(tampered));

  await assert.rejects(
    usb.sender.sendItemDone(),
    error => error.name === 'PeerProtocolError' && /AES-GCM authentication failed/.test(error.message),
  );

  const doneAcks = ble.sent.filter(message => message.type === MSG.ACK && readAckSeq(message)?.type === MSG.ITEM_DONE);
  assert.equal(doneAcks.length, 0);
  assert.equal(ble.sent.some(message => message.type === MSG.ERROR), true);
  assert.equal(ble.received.length, 0);
  assert.deepEqual(ble.errors, ['AES-GCM authentication failed']);
  assert.equal(ble.receiver.meta, null);
  assert.equal(ble.receiver.chunks.size, 0);
});

test('CANCEL during delayed validation drops the stale completed item', async () => {
  const sent = [];
  const received = [];
  const validationStarted = createDeferred();
  const releaseValidation = createDeferred();
  const receiver = new ItemReceiver({ sendFn: frame => sent.push(parseMessage(frame)) });
  receiver.setItemValidator(item => {
    validationStarted.resolve();
    return releaseValidation.promise.then(() => item);
  });
  receiver.on('item', item => received.push(item));

  const doneFrame = await prepareReceiverItem(receiver, 9101, 'cancel during validation');
  sent.length = 0;
  const pendingDone = receiver.onMessage(doneFrame);
  await validationStarted.promise;
  await receiver.onMessage(buildMessage(MSG.CANCEL, 0));
  releaseValidation.resolve();
  await pendingDone;

  assert.equal(countDoneAcks(sent), 0);
  assert.equal(received.length, 0);
  assert.deepEqual(readAckSeq(sent.at(-1)), { type: MSG.CANCEL, seq: 0 });
});

test('duplicate ITEM_DONE frames share validation and emit once', async () => {
  const sent = [];
  const received = [];
  const validationStarted = createDeferred();
  const releaseValidation = createDeferred();
  let validationCalls = 0;
  const receiver = new ItemReceiver({ sendFn: frame => sent.push(parseMessage(frame)) });
  receiver.setItemValidator(item => {
    validationCalls++;
    validationStarted.resolve();
    return releaseValidation.promise.then(() => item);
  });
  receiver.on('item', item => received.push(item));

  const doneFrame = await prepareReceiverItem(receiver, 9102, 'duplicate done');
  sent.length = 0;
  const firstDone = receiver.onMessage(doneFrame);
  await validationStarted.promise;
  const duplicateDone = receiver.onMessage(doneFrame);
  releaseValidation.resolve();
  await Promise.all([firstDone, duplicateDone]);

  assert.equal(validationCalls, 1);
  assert.equal(received.length, 1);
  assert.equal(countDoneAcks(sent), 2);
});

test('replacement HELLO during validation drops the old completed item', async () => {
  const sent = [];
  const received = [];
  const oldValidationStarted = createDeferred();
  const releaseOldValidation = createDeferred();
  const oldItemId = 9103;
  const newItemId = 9104;
  const receiver = new ItemReceiver({ sendFn: frame => sent.push(parseMessage(frame)) });
  receiver.setItemValidator(item => {
    if (item.meta.itemId === oldItemId) {
      oldValidationStarted.resolve();
      return releaseOldValidation.promise.then(() => item);
    }
    return item;
  });
  receiver.on('item', item => received.push(item));

  const oldDoneFrame = await prepareReceiverItem(receiver, oldItemId, 'replaced item');
  sent.length = 0;
  const pendingOldDone = receiver.onMessage(oldDoneFrame);
  await oldValidationStarted.promise;
  const newDoneFrame = await prepareReceiverItem(receiver, newItemId, 'replacement item');
  await receiver.onMessage(newDoneFrame);
  releaseOldValidation.resolve();
  await pendingOldDone;

  assert.deepEqual(received.map(item => item.meta.itemId), [newItemId]);
  assert.equal(countDoneAcks(sent), 1);
});
