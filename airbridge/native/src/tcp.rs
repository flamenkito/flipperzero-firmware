use crate::{
    frame::{Frame, Kind},
    link::Link,
    session::{self, Config, Role, Stats},
};
use anyhow::{Context, Result, bail, ensure};
use std::{collections::VecDeque, net::SocketAddr, time::Duration};
use tokio::net::{TcpListener, TcpStream};

#[derive(Default)]
struct Recent(VecDeque<(u64, Option<u32>)>);

impl Recent {
    fn add(&mut self, id: u64, result: &Result<Stats>) {
        self.0
            .push_back((id, result.as_ref().ok().map(|stats| stats.receive_next)));
        if self.0.len() > 16 {
            self.0.pop_front();
        }
    }

    async fn handle(&self, frame: &Frame, link: &Link) -> Result<bool> {
        let Some((_, next)) = self.0.iter().find(|(id, _)| *id == frame.session) else {
            return Ok(false);
        };
        let reply = match (next, frame.kind) {
            (Some(next), Kind::Data | Kind::Fin) => Some((Kind::Ack, *next)),
            (Some(_), Kind::Ping) => Some((Kind::Pong, 0)),
            (_, Kind::Reset | Kind::Ack | Kind::Pong) => None,
            _ => Some((Kind::Reset, 0)),
        };
        if let Some((kind, sequence)) = reply {
            link.send(Frame::control(kind, frame.session, sequence))
                .await?;
        }
        Ok(true)
    }
}

fn report(id: u64, result: &Result<Stats>) {
    match result {
        Ok(stats) => eprintln!(
            "stream {id:016x} closed: tx={} rx={} retries={} peak_pending={} acks_sent={}",
            stats.sent_bytes,
            stats.received_bytes,
            stats.retransmissions,
            stats.peak_pending,
            stats.acks_sent
        ),
        Err(error) => eprintln!("stream {id:016x} failed: {error:#}"),
    }
}

pub async fn listen(link: &mut Link, address: SocketAddr, cfg: &Config) -> Result<()> {
    ensure!(
        address.ip().is_loopback(),
        "listen address must be loopback"
    );
    let listener = TcpListener::bind(address)
        .await
        .context("bind local TCP listener")?;
    listen_on(link, listener, cfg).await
}

async fn listen_on(link: &mut Link, listener: TcpListener, cfg: &Config) -> Result<()> {
    eprintln!(
        "listening on {}; one active TCP connection",
        listener.local_addr()?
    );
    let mut recent = Recent::default();
    loop {
        let (stream, peer) = tokio::select! {
            accepted = listener.accept() => accepted?,
            frame = link.receive() => {
                let frame = frame?;
                if !recent.handle(&frame, link).await? && frame.kind == Kind::Open {
                    link.send(Frame::control(Kind::Reset, frame.session, 0)).await?;
                }
                continue;
            }
        };
        stream.set_nodelay(true)?;
        let id = loop {
            let id = rand::random::<u64>();
            if id != 0 {
                break id;
            }
        };
        eprintln!("stream {id:016x} opening for {peer}");
        let result = {
            let operation = async {
                let window = session::open(link, id, cfg).await?;
                let negotiated = Config {
                    window,
                    ..cfg.clone()
                };
                eprintln!("stream {id:016x} connected; window={window}");
                session::run(stream, link, id, Role::Initiator, &negotiated).await
            };
            tokio::pin!(operation);
            loop {
                tokio::select! {
                    result = &mut operation => break result,
                    extra = listener.accept() => {
                        let (stream, peer) = extra?;
                        drop(stream);
                        eprintln!("rejected {peer}: bridge already has an active TCP connection");
                    }
                }
            }
        };
        if result.is_err() {
            let _ = link.send(Frame::control(Kind::Reset, id, 0)).await;
        }
        report(id, &result);
        recent.add(id, &result);
        ensure!(!link.is_closed(), "transport disconnected");
    }
}

async fn connect_target(link: &mut Link, target: &str, id: u64) -> Result<TcpStream> {
    let connection = tokio::time::timeout(Duration::from_secs(10), TcpStream::connect(target));
    tokio::pin!(connection);
    loop {
        tokio::select! {
            result = &mut connection => return result.context("target TCP connect timed out")?.context("target TCP connect failed"),
            frame = link.receive() => {
                let frame = frame?;
                if frame.session == id && frame.kind == Kind::Reset { bail!("opener cancelled connection"); }
                if frame.session != id && frame.kind == Kind::Open {
                    link.send(Frame::control(Kind::Reset, frame.session, 0)).await?;
                }
            }
        }
    }
}

pub async fn serve(link: &mut Link, target: &str, cfg: &Config) -> Result<()> {
    eprintln!("waiting for stream; fixed TCP target {target}");
    let mut recent = Recent::default();
    loop {
        let frame = link.receive().await?;
        if recent.handle(&frame, link).await? || frame.kind != Kind::Open {
            continue;
        }
        let id = frame.session;
        let window = frame.window().min(cfg.window);
        let negotiated = Config {
            window,
            ..cfg.clone()
        };
        let result = match connect_target(link, target, id).await {
            Ok(stream) => {
                stream.set_nodelay(true)?;
                eprintln!("stream {id:016x} connected to {target}; window={window}");
                session::run(stream, link, id, Role::Responder, &negotiated).await
            }
            Err(error) => {
                let _ = link.send(Frame::control(Kind::Reset, id, 0)).await;
                Err(error)
            }
        };
        report(id, &result);
        recent.add(id, &result);
        ensure!(!link.is_closed(), "transport disconnected");
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::link::Driver;
    use tokio::io::{AsyncReadExt, AsyncWriteExt};
    use tokio::task::JoinHandle;

    struct Tasks(Vec<JoinHandle<()>>);
    impl Drop for Tasks {
        fn drop(&mut self) {
            for task in &self.0 {
                task.abort();
            }
        }
    }

    fn pair() -> (Link, Link, Tasks) {
        let (a, Driver { tx: at, rx: ar, .. }) = Link::channel();
        let (b, Driver { tx: bt, rx: br, .. }) = Link::channel();
        let tasks = [(ar, bt), (br, at)]
            .into_iter()
            .map(|(mut rx, tx)| {
                tokio::spawn(async move {
                    while let Some(frame) = rx.recv().await {
                        if tx.send(Ok(frame)).await.is_err() {
                            break;
                        }
                    }
                })
            })
            .collect();
        (a, b, Tasks(tasks))
    }

    fn cfg() -> Config {
        Config {
            retry: Duration::from_millis(10),
            ..Config::default()
        }
    }

    async fn listener() -> TcpListener {
        TcpListener::bind("127.0.0.1:0").await.unwrap()
    }

    #[tokio::test]
    async fn connector_negotiates_both_versions_and_latches_the_first_offer() {
        tokio::time::timeout(Duration::from_secs(3), async {
            for (offered, maximum, expected) in [(2, 4, 2), (4, 2, 2), (4, 4, 4)] {
                let (mut client, mut server, mut tasks) = pair();
                let target = listener().await;
                let destination = target.local_addr().unwrap().to_string();
                tasks.0.push(tokio::spawn(async move {
                    let config = Config {
                        window: maximum,
                        ..cfg()
                    };
                    let _ = serve(&mut server, &destination, &config).await;
                }));
                client
                    .send(Frame::handshake(Kind::Open, 101, offered))
                    .await
                    .unwrap();
                let (mut stream, _) = target.accept().await.unwrap();
                let accepted = client.receive().await.unwrap();
                assert_eq!((accepted.kind, accepted.window()), (Kind::Accept, expected));
                client
                    .send(Frame::handshake(Kind::Open, 101, 2))
                    .await
                    .unwrap();
                let repeated = client.receive().await.unwrap();
                assert_eq!(repeated, accepted);
                client
                    .send(Frame::data(101, 0, b"still connected"))
                    .await
                    .unwrap();
                let mut data = [0; 15];
                stream.read_exact(&mut data).await.unwrap();
                assert_eq!(&data, b"still connected");
                assert!(
                    tokio::time::timeout(Duration::from_millis(20), target.accept())
                        .await
                        .is_err()
                );
            }
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn tcp_busy_rejection_and_sequential_reuse() {
        tokio::time::timeout(Duration::from_secs(3), async {
            let (mut a, mut b, mut tasks) = pair();
            let local = listener().await;
            let address = local.local_addr().unwrap();
            let target = listener().await;
            let destination = target.local_addr().unwrap().to_string();
            tasks.0.push(tokio::spawn(async move {
                let _ = listen_on(&mut a, local, &cfg()).await;
            }));
            tasks.0.push(tokio::spawn(async move {
                let _ = serve(&mut b, &destination, &cfg()).await;
            }));
            for number in 0..3u8 {
                let mut client = TcpStream::connect(address).await.unwrap();
                let (mut server, _) = target.accept().await.unwrap();
                client.write_all(&[number; 121]).await.unwrap();
                let mut request = [0; 121];
                server.read_exact(&mut request).await.unwrap();
                assert_eq!(request, [number; 121]);
                let mut extra = TcpStream::connect(address).await.unwrap();
                let mut byte = [0];
                assert_eq!(extra.read(&mut byte).await.unwrap(), 0);
                client.shutdown().await.unwrap();
                assert_eq!(server.read(&mut byte).await.unwrap(), 0);
                server.write_all(b"response after EOF").await.unwrap();
                server.shutdown().await.unwrap();
                let mut response = Vec::new();
                client.read_to_end(&mut response).await.unwrap();
                assert_eq!(response, b"response after EOF");
                tokio::time::sleep(Duration::from_millis(70)).await;
            }
        })
        .await
        .unwrap();
    }

    #[tokio::test]
    async fn target_refusal_recovers_and_stale_open_cannot_reconnect() {
        tokio::time::timeout(Duration::from_secs(3), async {
            let (mut client, mut server, mut tasks) = pair();
            let target = listener().await;
            let address = target.local_addr().unwrap();
            drop(target);
            let destination = address.to_string();
            tasks.0.push(tokio::spawn(async move {
                let _ = serve(&mut server, &destination, &cfg()).await;
            }));
            assert!(
                session::open(&mut client, 10, &cfg())
                    .await
                    .unwrap_err()
                    .to_string()
                    .contains("refused")
            );
            let target = TcpListener::bind(address).await.unwrap();
            assert!(
                session::open(&mut client, 10, &cfg())
                    .await
                    .unwrap_err()
                    .to_string()
                    .contains("refused")
            );
            session::open(&mut client, 11, &cfg()).await.unwrap();
            let (_connection, _) = target.accept().await.unwrap();
            client
                .send(Frame::control(Kind::Open, 11, 0))
                .await
                .unwrap();
            assert_eq!(client.receive().await.unwrap().kind, Kind::Accept);
            assert!(
                tokio::time::timeout(Duration::from_millis(50), target.accept())
                    .await
                    .is_err()
            );
            client
                .send(Frame::control(Kind::Reset, 11, 0))
                .await
                .unwrap();
        })
        .await
        .unwrap();
    }
}
