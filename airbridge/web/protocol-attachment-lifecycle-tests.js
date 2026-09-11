import { receiveAssert as assert, receiveReject as rejects } from './protocol-receive-tests.js';

export function attachmentLifecycleTests(vendor) {
  return [['Attachment lifecycle verified Blob, removal/replacement/clear/pagehide/dispose', async () => {
    const ui = await import('./airbridge-ui.js');
    assert(typeof ui.createAttachmentLifecycle === 'function', 'explicit attachment lifecycle missing');
    const { createReceiveAccumulator } = await import('./airbridge-receive-accumulator.js');
    const host = document.createElement('section');
    document.body.append(host);
    const events = new EventTarget();
    const lifecycle = ui.createAttachmentLifecycle(events);
    const originalCreate = URL.createObjectURL;
    const originalRevoke = URL.revokeObjectURL;
    const created = new Map();
    const revoked = new Map();
    URL.createObjectURL = blob => {
      const url = originalCreate.call(URL, blob);
      created.set(url, blob);
      return url;
    };
    URL.revokeObjectURL = url => {
      assert(!Array.from(document.querySelectorAll('a')).some(a => a.href === url), 'no visible live link at revocation');
      revoked.set(url, (revoked.get(url) || 0) + 1);
      originalRevoke.call(URL, url);
    };
    try {
      const bytes = new Uint8Array(4 * 1024 * 1024 + 65).fill(42);
      const hash = vendor.create().update(bytes).hex();
      const acc = createReceiveAccumulator(vendor);
      for (let i = 0; i < bytes.length; i += 65536) acc.appendAuthenticated(bytes.subarray(i, i + 65536));
      assert(created.size === 0 && !host.querySelector('.attachment-card'), 'no early URL/card');
      const verified = acc.finalize(BigInt(bytes.length), hash, '<img src=x onerror=alert(1)>.bin', 'application/octet-stream');
      const card = ui.renderVerifiedAttachmentCard(verified, lifecycle);
      host.append(card);
      const link = card.querySelector('a');
      assert(created.size === 1 && created.get(link.href) === verified.blob, 'one URL owns exact verified Blob');
      assert(link.download === ui.attachmentDownloadName(verified.name) && !card.querySelector('img'), 'safe download name is a value, not markup');
      assert(card.querySelector('.attachment-name').textContent === verified.name, 'exact display name');
      assert((await (await fetch(link.href)).arrayBuffer()).byteLength === bytes.length, 'download URL works');
      const replacement = ui.renderVerifiedAttachmentCard(verified, lifecycle);
      ui.replaceAttachmentCard(card, replacement);
      assert(!card.isConnected && revoked.size === 1, 'replacement synchronous cleanup');
      ui.removeAttachmentCard(replacement);
      ui.removeAttachmentCard(replacement);
      assert(revoked.size === 2, 'idempotent explicit removal');
      const row = ui.addMessage(host, { side: 'peer', kind: 'attachment', verified, lifecycle });
      assert(row.querySelector('a'), 'verified addMessage path');
      ui.clearTranscript(host);
      ui.clearTranscript(host);
      assert(revoked.size === 3 && !host.querySelector('a'), 'transcript clear synchronous cleanup');
      const nativeRemoved = ui.renderVerifiedAttachmentCard(verified, lifecycle);
      host.append(nativeRemoved);
      nativeRemoved.remove();
      await new Promise(resolve => setTimeout(resolve, 0));
      assert(revoked.size === 4, 'native removal cleanup');
      const nativeReplaced = ui.renderVerifiedAttachmentCard(verified, lifecycle);
      host.append(nativeReplaced);
      nativeReplaced.replaceWith(document.createElement('div'));
      await new Promise(resolve => setTimeout(resolve, 0));
      assert(revoked.size === 5, 'native replacement cleanup');
      host.append(ui.renderVerifiedAttachmentCard(verified, lifecycle));
      events.dispatchEvent(new Event('pagehide'));
      events.dispatchEvent(new Event('pagehide'));
      assert(revoked.size === 6 && !host.querySelector('a'), 'pagehide removes before revoking');
      lifecycle.dispose(); lifecycle.dispose();
      rejects(() => ui.renderVerifiedAttachmentCard(verified, lifecycle));
      assert(created.size === 6, 'disposed lifecycle cannot create URL');
      const second = ui.createAttachmentLifecycle(events);
      host.append(ui.renderVerifiedAttachmentCard(verified, second));
      second.dispose(); second.dispose();
      assert(revoked.size === 7, 'dispose cleans live card');
      // Legacy callers stay compatible and clearTranscript now revokes their URLs.
      host.append(ui.renderAttachmentCard({ name: 'legacy.bin' }, new Uint8Array([1]), hash, false));
      ui.clearTranscript(host, '');
      assert(created.size === 8 && revoked.size === 8, 'legacy Uint8Array cleanup');
      for (const count of revoked.values()) assert(count === 1, 'exactly once revoke');
      for (const mode of ['hash', 'size', 'cancel', 'disconnect', 'reset']) {
        const failed = createReceiveAccumulator(vendor);
        failed.appendAuthenticated(new Uint8Array([1]));
        if (mode === 'hash' || mode === 'size') {
          rejects(() => failed.finalize(mode === 'size' ? 2n : 1n, hash, 'bad', ''));
        } else failed.abort();
        assert(failed.snapshot().retainedChunks === 0 && !failed.snapshot().hasHasher, 'failed ownership released');
      }
      assert(created.size === 8 && !host.querySelector('.attachment-card'), 'failures create no URL/completed card');
    } finally {
      ui.clearTranscript(host, '');
      lifecycle.dispose();
      host.remove();
      URL.createObjectURL = originalCreate;
      URL.revokeObjectURL = originalRevoke;
    }
  }],
  ['R1 owned content/link/href invalidation revokes without waiting for disposal', async () => {
    await withLifecycleProbe(async ({ ui, lifecycle, host, created, revoked }) => {
      const cases = [
        card => card.replaceChildren(document.createTextNode('invalidated')),
        (card, link) => link.remove(),
        (card, link) => { const nested = document.createElement('span'); card.append(nested); nested.append(link); nested.replaceWith(document.createTextNode('invalidated')); },
        (card, link) => link.removeAttribute('href'),
        (card, link) => { link.href = '#invalidated'; },
      ];
      for (const invalidate of cases) {
        const card = ui.renderAttachmentCard({ name: 'test' }, new Uint8Array([1]), 'a'.repeat(64), false, undefined, lifecycle);
        host.append(card);
        const link = card.querySelector('a');
        const url = link.href;
        assert((await fetch(url)).status === 200, 'owned URL initially fetchable');
        invalidate(card, link);
        await mutationDelivery();
        assert(revoked.filter(value => value === url).length === 1, 'R1 invalidated content URL was not revoked exactly once');
        assert(!link.hasAttribute('href'), 'original link invalidated');
        assert(!await fetchable(url), 'invalidated URL still fetchable');
        ui.removeAttachmentCard(card);
      }
      lifecycle.dispose(); lifecycle.dispose();
      assert(created.length === 5 && revoked.length === 5, 'no double cleanup after invalidation');
    });
  }],
  ['R1 connected card/link moves preserve URL and queued cleanup is idempotent', async () => {
    await withLifecycleProbe(async ({ ui, lifecycle, events, host, created, revoked }) => {
      const card = ui.renderAttachmentCard({ name: 'test' }, new Uint8Array([1]), 'a'.repeat(64), false, undefined, lifecycle);
      host.append(card);
      const link = card.querySelector('a');
      const url = link.href;
      await mutationDelivery();
      const next = document.createElement('div'); host.append(next);
      card.remove(); next.append(card);
      const nested = document.createElement('span'); card.append(nested);
      link.remove(); nested.append(link);
      link.removeAttribute('href'); link.href = url;
      await mutationDelivery();
      assert(revoked.length === 0 && card.isConnected && link.isConnected && await fetchable(url), 'valid moved pair revoked spuriously');
      card.replaceChildren(document.createTextNode('invalidated'));
      ui.removeAttachmentCard(card);
      events.dispatchEvent(new Event('pagehide'));
      lifecycle.dispose(); ui.removeAttachmentCard(card);
      await mutationDelivery();
      assert(created.length === 1 && revoked.length === 1, 'queued observer/explicit/pagehide/dispose double revoke');
    });
  }],
  ['R2 throwing href and partial map registration roll back allocated URL exactly once', async () => {
    await withLifecycleProbe(async ({ lifecycle, host, created, revoked }) => {
      for (const phase of ['href', 'partial href', 'map', 'weakmap', 'create']) {
        const card = document.createElement('div');
        const link = document.createElement('a'); card.append(link); host.append(card);
        const error = new Error(`injected ${phase} failure`);
        const create = URL.createObjectURL;
        const mapSet = Map.prototype.set;
        const weakSet = WeakMap.prototype.set;
        const before = created.length;
        try {
          if (phase.includes('href')) Object.defineProperty(link, 'href', { configurable: true, set(value) {
            if (phase === 'partial href') this.setAttribute('href', value);
            throw error;
          } });
          if (phase === 'map') Map.prototype.set = function(key, value) {
            const result = mapSet.call(this, key, value);
            if (key === card) throw error;
            return result;
          };
          if (phase === 'weakmap') WeakMap.prototype.set = function(key, value) {
            const result = weakSet.call(this, key, value);
            if (key === card) throw error;
            return result;
          };
          if (phase === 'create') URL.createObjectURL = () => { throw error; };
          let caught;
          try { lifecycle.own(card, new Blob(['x']), link); } catch (failure) { caught = failure; }
          assert(caught === error, 'original ownership error rethrown');
        } finally {
          URL.createObjectURL = create;
          Map.prototype.set = mapSet;
          WeakMap.prototype.set = weakSet;
          delete link.href;
        }
        assert(!link.hasAttribute('href'), 'partial href retained');
        const allocated = phase === 'create' ? 0 : 1;
        assert(created.length === before + allocated && revoked.length === created.length, 'R2 allocated URL leaked during rollback');
        if (allocated) assert(!await fetchable(created.at(-1)), 'rolled back URL still fetchable');
        // Reusing the same card proves neither ownership map kept a partial entry.
        card.append(link); host.append(card);
        lifecycle.own(card, new Blob(['retry']), link);
        card.remove(); await mutationDelivery();
        assert(revoked.length === created.length, 'retry cleanup');
      }
      lifecycle.dispose(); lifecycle.dispose();
      assert(created.length === 9 && revoked.length === 9 && new Set(revoked).size === 9, 'dispose after rollback double revoke');
    });
  }],
  ['R2a reentrant disposal before/after either registration cannot commit ownership', async () => {
    for (const prototype of [Map.prototype, WeakMap.prototype]) {
      for (const timing of ['before', 'after']) for (const throwAfter of [false, true]) {
        await withLifecycleProbe(async ({ ui, lifecycle, host, created, revoked }) => {
          const card = document.createElement('div');
          const link = document.createElement('a'); card.append(link); host.append(card);
          const originalSet = prototype.set;
          const injected = new Error('registration failed after reentrant dispose');
          let registrationMap;
          let caught;
          try {
            prototype.set = function(key, value) {
              if (key !== card) return originalSet.call(this, key, value);
              registrationMap = this;
              if (timing === 'before') lifecycle.dispose();
              const result = originalSet.call(this, key, value);
              if (timing === 'after') lifecycle.dispose();
              if (throwAfter) throw injected;
              return result;
            };
            try { lifecycle.own(card, new Blob(['x']), link); } catch (error) { caught = error; }
          } finally { prototype.set = originalSet; }
          try {
            assert(lifecycle.disposed && caught instanceof Error, 'R2a own returned successfully after reentrant disposal');
            assert(throwAfter ? caught === injected : /disposed/.test(caught.message), 'setup error identity');
            assert(!registrationMap.has(card), 'late registration survived rollback');
            assert(created.length === 1 && revoked.length === 1 && !link.hasAttribute('href') && !card.isConnected, 'R2a partial ownership retained');
            assert(!await fetchable(created[0]), 'R2a released URL fetchable');
            lifecycle.dispose();
            const fresh = ui.createAttachmentLifecycle(new EventTarget());
            try {
              host.append(card);
              fresh.own(card, new Blob(['retry']), link);
              assert(await fetchable(link.href), 'fresh lifecycle same-card reuse failed');
            } finally { fresh.dispose(); }
            assert(created.length === 2 && revoked.length === 2 && new Set(revoked).size === 2, 'fresh reuse cleanup or double revoke');
          } finally { ui.removeAttachmentCard(card); }
        });
      }
    }
  }],
  ['R2b disposal drains every card despite first/multiple remove exceptions', async () => {
    for (const failures of [1, 2]) await withLifecycleProbe(async ({ ui, lifecycle, events, host, created, revoked }) => {
      const cards = Array.from({ length: 3 }, () => ui.renderAttachmentCard(
        { name: 'probe' }, new Uint8Array([1]), 'a'.repeat(64), false, undefined, lifecycle));
      host.append(...cards);
      await mutationDelivery();
      const links = cards.map(card => card.querySelector('a'));
      const errors = cards.map((_, i) => new Error(`injected remove failure ${i}`));
      const attempts = [];
      const removes = cards.map(card => card.remove);
      let caught;
      try {
        cards.forEach((card, i) => { card.remove = () => {
          attempts.push(i);
          if (i < failures) throw errors[i];
          removes[i].call(card);
        }; });
        try { lifecycle.dispose(); } catch (error) { caught = error; }
      } finally { cards.forEach((card, i) => { card.remove = removes[i]; }); }
      try {
        assert(caught === errors[0], 'dispose must rethrow first original error');
        assert(attempts.join(',') === '0,1,2', 'R2b disposal stopped at first cleanup exception');
        assert(links.every(link => !link.hasAttribute('href')), 'later card href stranded');
        assert(created.length === 3 && revoked.length === 3 && new Set(revoked).size === 3, 'R2b URL stranded');
        for (const url of created) assert(!await fetchable(url), 'disposed URL still fetchable');
        lifecycle.dispose(); events.dispatchEvent(new Event('pagehide')); lifecycle.dispose();
        await mutationDelivery();
        assert(revoked.length === 3 && attempts.length === 3, 'repeated disposal reattempted cleanup');
        const fresh = ui.createAttachmentLifecycle(new EventTarget());
        try {
          for (let i = 0; i < cards.length; i++) {
            host.append(cards[i]);
            fresh.own(cards[i], new Blob(['reuse']), links[i]);
          }
        } finally { fresh.dispose(); }
        assert(created.length === 6 && revoked.length === 6 && new Set(revoked).size === 6, 'disposed maps prevent fresh same-card reuse');
      } finally { for (const card of cards) ui.removeAttachmentCard(card); }
    });
  }]];
}

const mutationDelivery = () => new Promise(resolve => setTimeout(resolve, 0));
async function fetchable(url) {
  try { return (await fetch(url)).ok; } catch { return false; }
}

async function withLifecycleProbe(run) {
  const ui = await import('./airbridge-ui.js');
  const host = document.createElement('section'); document.body.append(host);
  const events = new EventTarget();
  const lifecycle = ui.createAttachmentLifecycle(events);
  const create = URL.createObjectURL;
  const revoke = URL.revokeObjectURL;
  const created = [];
  const revoked = [];
  URL.createObjectURL = blob => { const url = create.call(URL, blob); created.push(url); return url; };
  URL.revokeObjectURL = url => {
    assert(!Array.from(document.querySelectorAll('a')).some(link => link.getAttribute('href') === url), 'live owned href at revoke');
    revoked.push(url); revoke.call(URL, url);
  };
  try { await run({ ui, lifecycle, events, host, created, revoked }); }
  finally {
    lifecycle.dispose(); host.remove();
    URL.createObjectURL = create; URL.revokeObjectURL = revoke;
    for (const url of created) if (!revoked.includes(url)) revoke.call(URL, url);
  }
}
