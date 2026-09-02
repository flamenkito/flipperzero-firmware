const DEFAULT_EVIDENCE_PATH = '.omo/evidence/airbridge-roadmap-encryption/task-1/';

function clockNow() {
  return globalThis.performance?.now ? globalThis.performance.now() : Date.now();
}

function isoNow() {
  return new Date().toISOString();
}

function finiteNumber(value, fallback = 0) {
  return Number.isFinite(value) ? value : fallback;
}

function byteCount(value) {
  if (!Number.isFinite(value)) return 0;
  return Math.max(0, Math.trunc(value));
}

function markerFrom(options = {}) {
  const mock = Boolean(options.mock);
  const hardware = Boolean(options.hardware);
  return {
    mock,
    hardware,
    marker: hardware ? 'hardware' : mock ? 'mock' : 'unspecified',
  };
}

function transferKbps(bytes, elapsedMs) {
  if (!Number.isFinite(elapsedMs) || elapsedMs <= 0) return 0;
  return Number(((bytes / 1024) / (elapsedMs / 1000)).toFixed(3));
}

function normalizeCounters(counters = {}) {
  return {
    usbToBle: byteCount(counters.usbToBle ?? counters.u2b ?? counters['U->B']),
    bleToUsb: byteCount(counters.bleToUsb ?? counters.b2u ?? counters['B->U']),
    drop: byteCount(counters.drop ?? counters.DROP),
    txerr: byteCount(counters.txerr ?? counters.TXERR),
  };
}

function emit(sink, record) {
  if (typeof sink !== 'function') return record;
  sink(record);
  return record;
}

export function createWindowEvidenceSink(options = {}) {
  const target = options.target ?? globalThis;
  const key = options.key ?? '__airbridgeEvidence';
  if (!Array.isArray(target[key])) target[key] = [];
  return record => {
    target[key].push({
      ...record,
      endpoint: record.endpoint ?? options.endpoint ?? null,
      evidencePath: record.evidencePath ?? options.evidencePath ?? DEFAULT_EVIDENCE_PATH,
    });
    return record;
  };
}

export function createTimingRecorder(options = {}) {
  const startMs = clockNow();
  const startedAt = isoNow();
  const marker = markerFrom(options);
  const details = options.details ?? options.metadata ?? {};
  const record = {
    schema: 'airbridge-timing/v1',
    operation: options.operation ?? 'transfer',
    endpoint: options.endpoint ?? null,
    direction: options.direction ?? details.direction ?? null,
    itemId: options.itemId ?? details.itemId ?? null,
    bundleBytes: byteCount(options.bundleBytes ?? options.transferBytes),
    transferBytes: byteCount(options.transferBytes ?? options.bundleBytes),
    firstReportTimestampMs: null,
    firstReportAt: null,
    firstReportLatencyMs: null,
    completeTimeMs: null,
    errorState: null,
    transferKbps: 0,
    mock: marker.mock,
    hardware: marker.hardware,
    marker: marker.marker,
    evidencePath: options.evidencePath ?? DEFAULT_EVIDENCE_PATH,
    startedAt,
    completedAt: null,
    details,
    metadata: details,
  };
  let terminal = false;

  function updateThroughput(elapsedMs) {
    record.transferKbps = transferKbps(record.transferBytes || record.bundleBytes, elapsedMs);
  }

  return {
    record,
    setBytes(bytes) {
      const count = byteCount(bytes);
      record.bundleBytes = count;
      record.transferBytes = count;
      return record;
    },
    tickTransferBytes(bytes) {
      record.transferBytes = byteCount(bytes);
      updateThroughput(clockNow() - startMs);
      return record;
    },
    markFirstReport() {
      if (record.firstReportLatencyMs == null) {
        const timestamp = clockNow();
        record.firstReportTimestampMs = Number(timestamp.toFixed(3));
        record.firstReportAt = isoNow();
        record.firstReportLatencyMs = Number((timestamp - startMs).toFixed(3));
      }
      return record;
    },
    complete(extra = {}) {
      if (terminal) return record;
      terminal = true;
      Object.assign(record, extra);
      if (extra.details && !extra.metadata) record.metadata = extra.details;
      if (extra.metadata && !extra.details) record.details = extra.metadata;
      const elapsed = clockNow() - startMs;
      record.completeTimeMs = Number(elapsed.toFixed(3));
      record.completedAt = isoNow();
      updateThroughput(elapsed);
      return emit(options.sink, record);
    },
    fail(errorState, extra = {}) {
      if (terminal) return record;
      terminal = true;
      Object.assign(record, extra);
      if (extra.details && !extra.metadata) record.metadata = extra.details;
      if (extra.metadata && !extra.details) record.details = extra.metadata;
      const elapsed = clockNow() - startMs;
      record.errorState = errorState instanceof Error ? errorState.message : String(errorState || 'error');
      record.completeTimeMs = Number(elapsed.toFixed(3));
      record.completedAt = isoNow();
      updateThroughput(elapsed);
      return emit(options.sink, record);
    },
    snapshot() {
      return { ...record, details: { ...record.details }, metadata: { ...record.metadata } };
    },
  };
}

export function recordCancel(detail = {}, options = {}) {
  const marker = markerFrom({ ...options, ...detail });
  const record = {
    schema: 'airbridge-cancel/v1',
    operation: 'cancel',
    endpoint: detail.endpoint ?? options.endpoint ?? null,
    direction: detail.direction ?? options.direction ?? null,
    itemId: detail.itemId ?? options.itemId ?? null,
    origin: detail.origin ?? options.origin ?? 'unknown',
    bundleBytes: byteCount(detail.bundleBytes ?? detail.transferBytes),
    transferBytes: byteCount(detail.transferBytes ?? detail.bundleBytes),
    firstReportTimestampMs: finiteNumber(detail.firstReportTimestampMs, 0),
    firstReportAt: detail.firstReportAt ?? isoNow(),
    firstReportLatencyMs: finiteNumber(detail.firstReportLatencyMs, 0),
    completeTimeMs: finiteNumber(detail.completeTimeMs, 0),
    errorState: detail.errorState ?? 'cancelled',
    transferKbps: finiteNumber(detail.transferKbps, 0),
    mock: marker.mock,
    hardware: marker.hardware,
    marker: marker.marker,
    evidencePath: detail.evidencePath ?? options.evidencePath ?? DEFAULT_EVIDENCE_PATH,
    completedAt: isoNow(),
    details: detail.details ?? detail.metadata ?? {},
    metadata: detail.metadata ?? detail.details ?? {},
  };
  return emit(options.sink, record);
}

export function recordFlipperCounters(counters = {}, options = {}) {
  const marker = markerFrom(options);
  const record = {
    schema: 'airbridge-flipper-counters/v1',
    operation: 'flipper-counters',
    endpoint: options.endpoint ?? null,
    flipperCounters: normalizeCounters(counters),
    bundleBytes: 0,
    transferBytes: 0,
    firstReportTimestampMs: 0,
    firstReportAt: isoNow(),
    firstReportLatencyMs: 0,
    completeTimeMs: 0,
    errorState: null,
    transferKbps: 0,
    mock: marker.mock,
    hardware: marker.hardware,
    marker: marker.marker,
    evidencePath: options.evidencePath ?? DEFAULT_EVIDENCE_PATH,
    completedAt: isoNow(),
    details: options.details ?? options.metadata ?? {},
    metadata: options.metadata ?? options.details ?? {},
  };
  return emit(options.sink, record);
}

export { DEFAULT_EVIDENCE_PATH };
