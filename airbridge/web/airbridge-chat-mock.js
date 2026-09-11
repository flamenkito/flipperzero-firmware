export function createPairedChatMock(role, room) {
  const channel = new BroadcastChannel(`pocket-airbridge-mock-${room}`);
  let connected = false, remote = false, closed = false, receive, disconnected, ready;
  const peerReady = new Promise(resolve => { ready = resolve; });
  const announce = type => channel.postMessage({role, type});
  function close() {
    if (closed) return;
    closed = true; connected = false; ready();
    channel.close(); window.removeEventListener('pagehide', disconnect);
    disconnected?.();
  }
  function disconnect() { if (!closed) { announce('disconnected'); close(); } }
  channel.onmessage = event => {
    const message = event.data;
    if (message?.role === role || closed) return;
    switch (message?.type) {
      case 'ready':
        if (!connected || !receive) return;
        if (!remote) { remote = true; announce('ready'); ready(); }
        break;
      case 'frame': if (connected && remote) receive?.(new Uint8Array(message.frame)); break;
      case 'disconnected': if (remote) close(); break;
    }
  };
  window.addEventListener('pagehide', disconnect);
  return {
    async connect() { if (closed) throw new Error('Mock transport closed'); connected = true; },
    async disconnect() { disconnect(); },
    async send(frame) {
      if (!connected) throw new Error('Mock transport disconnected');
      await peerReady;
      if (!connected) throw new Error('Mock transport disconnected');
      channel.postMessage({type:'frame',role,frame:new Uint8Array(frame)});
    },
    onReceive(callback) { receive = callback; announce('ready'); return () => { receive = null; }; },
    onDisconnect(callback) { disconnected = callback; return () => { disconnected = null; }; },
    isConnected:() => connected,
    getName:() => `Mock ${role.toUpperCase()} · paired pages`,
  };
}
