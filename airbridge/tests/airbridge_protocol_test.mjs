import assert from 'node:assert/strict';
import { test } from 'node:test';
import { createRequire } from 'node:module';
import { incrementalSha256Tests } from '../web/protocol-sha256-tests.js';
import { receiveAccumulatorTests } from '../web/protocol-receive-tests.js';
import * as v2 from '../web/airbridge-protocol.js';

const vendorSha256 = createRequire(import.meta.url)('../web/vendor/js-sha256-0.11.1.js');

import {
  AirBridgeCryptoSession,
  buildMessage,
  CRYPTO_ROLE,
  CRYPTO_STATE,
  encodeMeta,
  LegacyItemReceiver as ItemReceiver,
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

const streamKey = 'AAAAAAAAAAAAAAAAAAAAAA';
const zeroHash = '00'.repeat(32);
const bytes = value => Array.from(value);
function streamMeta(payloadSize = 0n, itemId = 1) {
  return v2.makeV2Meta({ keyId: streamKey, direction: 'usb-to-ble', itemId,
    headerLen: 50, payloadSize });
}

test('AB2S exact wire bytes, strict framing, capability and item controls', () => {
  const hello = v2.buildV2Frame(MSG.HELLO, 0, 0x01020304);
  assert.deepEqual(bytes(hello), [1,0,0,0,8,1,2,3,4,65,66,50,83]);
  assert.deepEqual(bytes(v2.buildV2Frame(MSG.ITEM_DATA, 0x1234, 1, 2, new Uint8Array([9]))),
    [3,18,52,0,9,0,0,0,1,0,0,0,2,9]);
  for (const type of [MSG.ITEM_DONE, MSG.CANCEL]) {
    assert.deepEqual(bytes(v2.buildV2Frame(type, 0, 1)), [type,0,0,0,4,0,0,0,1]);
  }
  assert.deepEqual(bytes(v2.buildV2Frame(MSG.ERROR, 0, 1, undefined, 'bad')),
    [7,0,0,0,11,65,66,50,83,0,0,0,1,98,97,100]);
  assert.deepEqual(bytes(v2.buildV2Frame(MSG.BUSY, 0, 1)), [8,0,0,0,8,65,66,50,83,0,0,0,1]);
  assert.equal(v2.parseV2Frame(hello).itemId, 0x01020304);
  const padded = new Uint8Array(64); padded.set(hello);
  assert.equal(v2.parseV2Frame(padded).itemId, 0x01020304);
  for (const raw of [new Uint8Array(0),hello.slice(0,-1),new Uint8Array(65),new Uint8Array([...hello,0])]) {
    assert.throws(() => v2.parseV2Frame(raw));
  }
  assert.throws(() => v2.parseV2Frame(buildMessage(MSG.HELLO,0,itemIdPayload(1))), /Unsupported protocol version/);
  const bad = hello.slice(); bad[9]=0;
  assert.throws(() => v2.parseV2Frame(bad), /Unsupported protocol version/);
  assert.throws(() => v2.buildV2Frame(MSG.ITEM_DATA,0,1,0,new Uint8Array(52)));
  assert.throws(() => v2.buildV2Frame(MSG.ITEM_DONE,1,1));
  assert.throws(() => v2.buildV2Frame(MSG.ERROR,0,1,undefined,'🙂'.repeat(13)));
});

test('AB2S ACK/NACK exact bytes, sentinel, legacy mismatch and one outstanding frame', () => {
  const context = {type:MSG.ITEM_DATA,seq:0x1234,itemId:1,segment:2};
  const ack = v2.makeV2AckPayload(context);
  assert.deepEqual(bytes(ack), [3,18,52,65,66,50,83,0,0,0,1,0,0,0,2]);
  assert.deepEqual(bytes(v2.makeV2AckPayload(context,1)), [...bytes(ack),1]);
  const pending = new v2.V2StopAndWait();
  const hello = v2.buildV2Frame(MSG.HELLO,0,1);
  pending.begin(hello);
  assert.throws(() => pending.begin(hello), /outstanding/);
  assert.throws(() => pending.receive(buildMessage(MSG.ACK,0,v2.makeAckPayload(MSG.HELLO,0))), /Unsupported protocol version/);
  assert.equal(pending.pending, null);
  pending.begin(hello);
  const helloAck = v2.makeV2AckPayload({type:MSG.HELLO,seq:0,itemId:1,segment:0xffffffff});
  assert.equal(helloAck.length,15);
  assert.deepEqual(bytes(helloAck.slice(-4)),[255,255,255,255]);
  assert.equal(pending.receive(buildMessage(MSG.ACK,0,helloAck)), 'ack');
  pending.begin(v2.buildV2Frame(MSG.ITEM_DATA,0x1234,1,2,new Uint8Array([9])));
  for (const delta of [{type:MSG.ITEM_META,segment:0xffffffff},{seq:3},{itemId:0},{segment:3}]) {
    const wrong = {...context,...delta};
    for (const reason of [undefined,1]) assert.equal(pending.receive(buildMessage(reason ? MSG.NACK:MSG.ACK,0,v2.makeV2AckPayload(wrong,reason))), 'ignored');
  }
  assert.equal(pending.receive(buildMessage(MSG.NACK,0,v2.makeV2AckPayload(context,3))), 'retry');
  assert.equal(pending.pending.frame.length,14);
  assert.equal(pending.receive(buildMessage(MSG.ACK,0,ack)), 'ack');
  for (const type of [MSG.KEY_OFFER,MSG.KEY_REPLY,MSG.KEY_CONFIRM]) {
    assert.equal(v2.makeAckPayload(type,0).length,3);
    assert.throws(() => v2.makeV2AckPayload({...context,type}));
  }
  assert.throws(() => v2.makeV2AckPayload({...context,type:MSG.HELLO}));
  assert.throws(() => v2.makeV2AckPayload(context,5));
});

test('AB2S canonical uint64, private fragmented META and finite segment bounds', () => {
  for (const value of [0,1,1n,'','01','+1','-1','1e3',' 1','1\n','1.0','18446744073709551616']) {
    assert.throws(() => v2.parseCanonicalUint64(value));
  }
  assert.equal(v2.parseCanonicalUint64('18446744073709551615'),0xffffffffffffffffn);
  assert.equal(v2.toSafeV2Number(9007199254740991n),Number.MAX_SAFE_INTEGER);
  assert.throws(() => v2.toSafeV2Number(9007199254740992n));
  for (const size of [0n,65486n,65487n,4194305n]) {
    const meta=streamMeta(size);
    const fragments=encodeMeta(meta);
    assert.ok(fragments.length>1);
    const decoded=v2.validateV2Meta(v2.decodeMeta(fragments),{itemId:1,keyId:streamKey,direction:'usb-to-ble'});
    const count=(50n+size+65535n)/65536n;
    assert.equal(decoded.totalCiphertextBytes,50n+size+16n*count);
    assert.equal(decoded.segmentCount,Number(count));
  }
  const meta=streamMeta();
  for (const patch of [{name:'secret'},{kind:'encrypted'},{protocolVersion:1},{cryptoVersion:2},{totalCiphertextBytes:66},
    {totalCiphertextBytes:'016'},{segmentCount:0},{segmentCount:0x100000000},{totalCiphertextBytes:'65553'},
    {maxSegmentPlaintextBytes:65535},{keyId:'x'},{direction:'other'}]) {
    assert.throws(()=>v2.validateV2Meta({...meta,...patch},{itemId:1,keyId:streamKey,direction:'usb-to-ble'}));
  }
  assert.throws(()=>v2.validateV2Meta(meta,{itemId:2,keyId:streamKey,direction:'usb-to-ble'}));
  assert.throws(()=>v2.v2StreamLayout(50,65536n*0xffffffffn));
  assert.equal(v2.v2StreamLayout(50,65536n*0xffffffffn-50n).segmentCount,0xffffffff);
});

test('AB2S authenticated binary header exact bytes, UTF-8 and header bounds', () => {
  const header=v2.encodeV2Header({kind:'attachment',name:'é',mimeType:'x',payloadSize:0n,payloadSha256:zeroHash});
  assert.deepEqual(bytes(header),[65,66,50,83,2,2,0,2,0,1,...new Array(40).fill(0),195,169,120]);
  assert.deepEqual(v2.decodeV2Header(header),{kind:'attachment',name:'é',mimeType:'x',payloadSize:0n,payloadSha256:zeroHash,headerLen:53});
  for(const index of [0,4,5]) {const bad=header.slice();bad[index]=9;assert.throws(()=>v2.decodeV2Header(bad));}
  const bad=header.slice();bad[50]=255;assert.throws(()=>v2.decodeV2Header(bad));
  assert.throws(()=>v2.decodeV2Header(header.slice(0,-1)));
  assert.throws(()=>v2.encodeV2Header({kind:'text',name:'x'.repeat(65487),mimeType:'',payloadSize:0n,payloadSha256:zeroHash}));
  assert.equal(v2.encodeV2Header({kind:'text',name:'x'.repeat(65486),mimeType:'',payloadSize:0n,payloadSha256:zeroHash}).length,65536);
  assert.throws(()=>v2.encodeV2Header({kind:'text',name:'\ud800',mimeType:'',payloadSize:0n,payloadSha256:zeroHash}));
});

test('AB2S segment IV and AAD exact bytes and uint32 shift hazard', () => {
  const prefix=new Uint8Array([10,20,30,40]);
  assert.deepEqual(bytes(v2.makeV2Iv(prefix,1,2)),[10,20,30,40,0,0,0,1,0,0,0,2]);
  assert.notDeepEqual(v2.makeV2Iv(prefix,1,0),v2.makeV2Iv(prefix,0,1));
  const seen=new Set();
  for(let item=1;item<4;item++) for(let segment=0;segment<3;segment++) seen.add(bytes(v2.makeV2Iv(prefix,item,segment)).join(','));
  assert.equal(seen.size,9);
  for(const pair of [[-1,0],[0,0x100000000],[0x100000000,0]]) assert.throws(()=>v2.makeV2Iv(prefix,...pair));
  const aad=v2.makeV2Aad({cryptoVersion:1,keyId:streamKey,direction:'usb-to-ble',itemId:1,segmentIndex:2,segmentPlaintextLen:50,finalFlag:true});
  assert.deepEqual(bytes(aad),[65,66,50,45,65,65,68,1,...new Array(16).fill(0),1,0,0,0,1,0,0,0,2,0,0,0,50,1]);
  assert.throws(()=>v2.makeV2Aad({cryptoVersion:2,keyId:streamKey,direction:'usb-to-ble',itemId:1,segmentIndex:0,segmentPlaintextLen:50,finalFlag:true}));
});

test('AB2S confirmed-session item IDs burn monotonically, inbound replay and wrap abort', async () => {
  const locked=new AirBridgeCryptoSession({role:CRYPTO_ROLE.USB,sendFn:async()=>{}});
  assert.throws(()=>locked.allocateStreamItemId(),/locked/);
  const {usb,ble}=await unlockCryptoPair();
  assert.equal(usb.session.allocateStreamItemId(),1);
  assert.equal(ble.session.allocateStreamItemId(),1);
  for(const outcome of ['cancel','error','timeout','success']) {
    const id=usb.session.allocateStreamItemId();
    ble.session.acceptStreamItemId(id);
    assert.throws(()=>ble.session.acceptStreamItemId(id),/stale/);
    assert.ok(id>1,outcome);
  }
  assert.equal(v2.compareV2Offers(1,1,1,2),-1);
  assert.equal(v2.compareV2Offers(2,1,1,2),1);
  usb.session.streamOutboundId=0xffffffff;
  assert.throws(()=>usb.session.allocateStreamItemId(),/wrapped/);
  assert.equal(usb.session.isUnlocked(),false);
  assert.equal(usb.session.keys,null);
});

test('AB2S bounded retry context, stale controls and exact 1286-frame segment rollover', () => {
  const state=new v2.V2ReceiveState({keyId:streamKey,direction:'usb-to-ble'});
  const hello=v2.buildV2Frame(MSG.HELLO,0,1);
  assert.equal(state.accept(hello).action,'ack');
  assert.equal(state.accept(hello).action,'duplicate');
  const meta=streamMeta(65537n-50n);
  for(const [seq,fragment] of encodeMeta(meta).entries()) {
    const frame=v2.buildV2Frame(MSG.ITEM_META,seq,1,undefined,fragment);
    assert.equal(state.accept(frame).action,'ack');
    assert.equal(state.accept(frame).action,'duplicate');
  }
  for(let seq=0;seq<1286;seq++) {
    const frame=v2.buildV2Frame(MSG.ITEM_DATA,seq,1,0,new Uint8Array(seq===1285?17:51));
    assert.equal(state.accept(frame).action,'ack');
    assert.equal(state.accept(frame).action,'duplicate');
  }
  assert.equal(state.segmentIndex,1);
  assert.equal(state.chunkInSegment,0);
  assert.equal(state.ciphertextBytes,65552n);
  assert.equal(state.accept(v2.buildV2Frame(MSG.ITEM_DATA,0,1,1,new Uint8Array(17))).action,'ack');
  assert.throws(()=>state.accept(v2.buildV2Frame(MSG.ITEM_DONE,0,1)),/verification/);
  state.abort();
  assert.equal(state.accept(v2.buildV2Frame(MSG.HELLO,0,2)).action,'ack');
  for(let i=0;i<3;i++) for(const type of [MSG.CANCEL,MSG.ITEM_DONE,MSG.ERROR,MSG.BUSY,MSG.ITEM_DATA]) {
    const raw=v2.buildV2Frame(type,0,1,type===MSG.ITEM_DATA?0:undefined,type===MSG.ERROR?'old':undefined);
    assert.equal(state.accept(raw).action,'stale');
    assert.equal(state.itemId,2);
  }
  assert.equal(state.accept(v2.buildV2Frame(MSG.CANCEL,0,2)).action,'ack');
  assert.equal(state.accept(v2.buildV2Frame(MSG.HELLO,0,3)).action,'ack');
  const fragment=encodeMeta(streamMeta(0n,3))[0];
  const frame=v2.buildV2Frame(MSG.ITEM_META,0,3,undefined,fragment);
  state.accept(frame);
  const conflicting=frame.slice();conflicting[conflicting.length-1]^=1;
  assert.throws(()=>state.accept(conflicting),/conflicting duplicate/);
  assert.equal(state.itemId,null);
});

test('AB2S completion gates count, header, authentication and payload hash without Blob', () => {
  const state=new v2.V2ReceiveState({keyId:streamKey,direction:'usb-to-ble'});
  state.accept(v2.buildV2Frame(MSG.HELLO,0,1));
  for(const [seq,fragment] of encodeMeta(streamMeta()).entries()) state.accept(v2.buildV2Frame(MSG.ITEM_META,seq,1,undefined,fragment));
  state.accept(v2.buildV2Frame(MSG.ITEM_DATA,0,1,0,new Uint8Array(51)));
  state.accept(v2.buildV2Frame(MSG.ITEM_DATA,1,1,0,new Uint8Array(15)));
  const proof={authenticatedSegments:1,streamPlaintextBytes:50n,headerLen:50,payloadSize:0n,payloadBytes:0n,payloadSha256:zeroHash,actualSha256:zeroHash};
  for(const patch of [{authenticatedSegments:0},{payloadBytes:1n},{actualSha256:'11'.repeat(32)},{streamPlaintextBytes:51n}]) {
    assert.throws(()=>v2.verifyV2Completion(streamMeta(),{...proof,...patch}));
  }
  state.verifyCompletion(proof);
  const done=v2.buildV2Frame(MSG.ITEM_DONE,0,1);
  assert.equal(state.accept(done).action,'ack');
  assert.equal(state.accept(done).action,'duplicate');
});

test('AB2S generated >4 MiB is accepted segmentwise without replay or ciphertext retention', () => {
  const payloadSize=4194305n;
  const meta=streamMeta(payloadSize);
  const state=new v2.V2ReceiveState({keyId:streamKey,direction:'usb-to-ble'});
  state.accept(v2.buildV2Frame(MSG.HELLO,0,1));
  for (const [seq,fragment] of encodeMeta(meta).entries()) state.accept(v2.buildV2Frame(MSG.ITEM_META,seq,1,undefined,fragment));
  let frames=0;
  for(let segment=0;segment<meta.segmentCount;segment++) {
    const length=segment===meta.segmentCount-1 ? Number(BigInt(meta.totalCiphertextBytes)-BigInt(segment)*65552n):65552;
    for(let offset=0;offset<length;offset+=51) {
      const generated=new Uint8Array(Math.min(51,length-offset)).fill(segment&255);
      state.accept(v2.buildV2Frame(MSG.ITEM_DATA,offset/51,1,segment,generated));
      frames++;
      assert.ok(state.last.payload.length<=59);
      assert.equal(state.metaFragments.length,0);
    }
  }
  assert.ok(frames>4096);
  assert.equal(state.ciphertextBytes,BigInt(meta.totalCiphertextBytes));
  state.verifyCompletion({authenticatedSegments:meta.segmentCount,streamPlaintextBytes:payloadSize+50n,
    headerLen:50,payloadSize,payloadBytes:payloadSize,payloadSha256:zeroHash,actualSha256:zeroHash});
  assert.equal(state.accept(v2.buildV2Frame(MSG.ITEM_DONE,0,1)).action,'ack');
});

test('AB2S malformed controls, legacy failure response and rejected DATA do not advance', () => {
  const state=new v2.V2ReceiveState({keyId:streamKey,direction:'usb-to-ble'});
  const legacy=state.accept(buildMessage(MSG.HELLO,0,itemIdPayload(1)));
  assert.equal(legacy.action,'error');
  assert.equal(new TextDecoder().decode(parseMessage(legacy.frame).payload),'Unsupported protocol version');
  assert.equal(state.itemId,null);
  state.accept(v2.buildV2Frame(MSG.HELLO,0,1));
  assert.throws(()=>state.accept(v2.buildV2Frame(MSG.ITEM_DATA,0,1,0,new Uint8Array(51))),/order/);
  assert.equal(state.itemId,null);
  const context={type:MSG.ITEM_DATA,seq:0,itemId:2,segment:0};
  for (const type of [MSG.ACK,MSG.NACK]) {
    const good=buildMessage(type,0,v2.makeV2AckPayload(context,type===MSG.NACK?1:undefined));
    for(const index of [4,8]) {const bad=good.slice();bad[index]^=1;assert.throws(()=>v2.parseV2Frame(bad));}
  }
  for (const type of [MSG.CANCEL,MSG.ITEM_DONE,MSG.ERROR,MSG.BUSY]) {
    assert.throws(()=>v2.parseV2Frame(buildMessage(type,0,new Uint8Array(3))));
  }
  assert.throws(()=>v2.encodeV2Header({kind:'text',name:'',mimeType:'',payloadSize:0n,payloadSha256:zeroHash+'\n'}));
});

function malformedV2Error(itemId) {
  return buildMessage(MSG.ERROR,0,new Uint8Array([65,66,50,83,...itemIdPayload(itemId),255]));
}

test('R1 stale malformed ERROR cannot interrupt the next receiver item', () => {
  const receiver=new v2.V2ReceiveState({keyId:streamKey,direction:'usb-to-ble'});
  receiver.accept(v2.buildV2Frame(MSG.HELLO,0,1));
  receiver.accept(v2.buildV2Frame(MSG.CANCEL,0,1));
  receiver.accept(v2.buildV2Frame(MSG.HELLO,0,2));
  const stale=malformedV2Error(1);
  assert.throws(()=>v2.parseV2Frame(stale),/encoding/);
  const interrupt=()=>{
    const before={...receiver};
    for(let i=0;i<10;i++) assert.equal(receiver.accept(stale).action,'stale');
    assert.deepEqual({...receiver},before);
    assert.equal(receiver.itemId,2);
  };
  interrupt();
  for(const [seq,fragment] of encodeMeta(streamMeta(0n,2)).entries()) {
    receiver.accept(v2.buildV2Frame(MSG.ITEM_META,seq,2,undefined,fragment));
    interrupt();
  }
  receiver.accept(v2.buildV2Frame(MSG.ITEM_DATA,0,2,0,new Uint8Array(51)));
  interrupt();
  receiver.accept(v2.buildV2Frame(MSG.ITEM_DATA,1,2,0,new Uint8Array(15)));
  receiver.verifyCompletion({authenticatedSegments:1,streamPlaintextBytes:50n,headerLen:50,
    payloadSize:0n,payloadBytes:0n,payloadSha256:zeroHash,actualSha256:zeroHash});
  interrupt();
  assert.equal(receiver.accept(v2.buildV2Frame(MSG.ITEM_DONE,0,2)).action,'ack');
  receiver.accept(v2.buildV2Frame(MSG.HELLO,0,3));
  assert.throws(()=>receiver.accept(malformedV2Error(3)),/encoding/);
  assert.equal(receiver.itemId,null);
  const badMagic=stale.slice(); badMagic[5]=0;
  const badSeq=stale.slice(); badSeq[2]=1;
  const padded=new Uint8Array(64);padded.set(stale);padded[63]=1;
  for(const ambiguous of [badMagic,badSeq,padded,stale.slice(0,12),new Uint8Array(65)]) {
    assert.throws(()=>receiver.accept(ambiguous));
  }
});

test('R1 sender isolates malformed stale ERROR before optional reason decoding', () => {
  const wait=new v2.V2StopAndWait();
  wait.begin(v2.buildV2Frame(MSG.HELLO,0,2));
  const pending=wait.pending;
  for(let i=0;i<10;i++) {
    assert.equal(wait.receive(malformedV2Error(1)),'ignored');
    assert.equal(wait.pending,pending);
  }
  assert.equal(wait.receive(buildMessage(MSG.ACK,0,v2.makeV2AckPayload({type:MSG.HELLO,seq:0,itemId:2,segment:0xffffffff}))),'ack');
  wait.begin(v2.buildV2Frame(MSG.ITEM_DATA,0,2,0,new Uint8Array(51)));
  assert.equal(wait.receive(malformedV2Error(1)),'ignored');
  assert.throws(()=>wait.receive(malformedV2Error(2)),/encoding/);
  const badMagic=malformedV2Error(1);badMagic[5]=0;
  const other=new v2.V2StopAndWait();other.begin(v2.buildV2Frame(MSG.HELLO,0,2));
  assert.throws(()=>other.receive(badMagic));
});

test('R2 retry owns exact BufferSource bytes including DataView and typed-array subviews', () => {
  const hello=v2.buildV2Frame(MSG.HELLO,0,1);
  const factories=[
    buffer=>new DataView(buffer,3,hello.length),
    buffer=>new Uint8Array(buffer,3,hello.length),
    buffer=>new Uint16Array(buffer,4,32),
    buffer=>new Float32Array(buffer,4,16),
  ];
  for(const viewOf of factories) {
    const buffer=new ArrayBuffer(80);
    new Uint8Array(buffer).fill(0xa5);
    const view=viewOf(buffer);
    const raw=new Uint8Array(buffer,view.byteOffset,view.byteLength);
    raw.fill(0);raw.set(hello);
    const expected=raw.slice();
    const wait=new v2.V2StopAndWait();wait.begin(view);
    assert.deepEqual(wait.pending.frame,expected,view.constructor.name);
    new Uint8Array(buffer).fill(0xff);
    assert.deepEqual(wait.pending.frame,expected,'caller mutation changed retry');
    assert.equal(v2.parseV2Frame(wait.pending.frame).itemId,1);
  }
});

test('R2 Node Buffer subview retains owned Uint8Array retry bytes after caller mutation', () => {
  const storage = Buffer.alloc(96, 0xa5);
  const input = storage.subarray(16, 80);
  input.fill(0);
  input.set(v2.buildV2Frame(MSG.HELLO, 0, 1));
  const expected = new Uint8Array(input);
  assert.equal(input.byteOffset - storage.byteOffset, 16);
  assert.equal(input.byteLength, 64);
  const wait = new v2.V2StopAndWait();
  wait.begin(input);
  assert.deepEqual(Array.from(wait.pending.frame), Array.from(expected));
  input.fill(0xff);
  assert.deepEqual(Array.from(wait.pending.frame), Array.from(expected), 'caller Buffer mutation changed retry bytes');
  assert.notEqual(wait.pending.frame.buffer, input.buffer, 'retry shares caller backing buffer');
  assert.equal(Object.getPrototypeOf(wait.pending.frame), Uint8Array.prototype);
  storage.fill(0x55);
  assert.deepEqual(wait.pending.frame, expected);
  assert.equal(v2.parseV2Frame(wait.pending.frame).itemId, wait.pending.itemId);
});

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

for (const [name, run] of incrementalSha256Tests(vendorSha256)) test(name, run);
for (const [name, run] of receiveAccumulatorTests(vendorSha256)) test(name, run);

const { segmentedCryptoTests } = await import('../web/protocol-segmented-crypto-tests.js');
for (const [name, run] of segmentedCryptoTests(vendorSha256)) test(name, run);

test('AB2S AEAD Node Buffer subviews preserve owned bytes during authentication', async () => {
  const {usb, ble} = await unlockCryptoPair();
  const backing = Buffer.alloc(80, 255); backing.set([0, 1, 2], 17);
  const source = {async *[Symbol.asyncIterator]() { yield backing.subarray(17, 20); }};
  const stream = usb.session.openEncryptedStream({kind:'attachment', name:'', mimeType:'', payloadSize:3n,
    payloadSha256:await sha256(new Uint8Array([0, 1, 2]))}, source);
  const receiver = ble.session.openSegmentReceiver(stream.meta);
  for await (const segment of stream) {
    backing.fill(99);
    let result;
    for (let offset = 0, seq = 0; offset < segment.bytes.length; offset += 51, seq++) {
      const slice = segment.bytes.subarray(offset, offset + 51);
      const parent = Buffer.alloc(100, 255); parent.set(slice, 13);
      const pending = receiver.push({itemId:stream.meta.itemId, segmentIndex:segment.index, chunkInSegment:seq},
        parent.subarray(13, 13 + slice.length));
      parent.fill(77);
      result = await pending;
    }
    assert.deepEqual(result.payload, new Uint8Array([0, 1, 2]));
  }
});
