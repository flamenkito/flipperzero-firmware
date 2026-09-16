use crate::frame::{Frame, Report};
use anyhow::{Context, Result, anyhow};
use std::time::Duration;
use tokio::sync::{mpsc, watch};

pub const QUEUE_CAPACITY: usize = 8;

pub struct Link {
    tx: mpsc::Sender<Report>,
    rx: mpsc::Receiver<Result<Report>>,
    stop: watch::Sender<bool>,
    pub invalid_frames: u64,
}

pub struct Driver {
    pub tx: mpsc::Sender<Result<Report>>,
    pub rx: mpsc::Receiver<Report>,
    pub stop: watch::Receiver<bool>,
}

impl Link {
    pub fn channel() -> (Self, Driver) {
        let (out_tx, out_rx) = mpsc::channel(QUEUE_CAPACITY);
        let (in_tx, in_rx) = mpsc::channel(QUEUE_CAPACITY);
        let (stop, stopped) = watch::channel(false);
        (
            Self {
                tx: out_tx,
                rx: in_rx,
                stop,
                invalid_frames: 0,
            },
            Driver {
                tx: in_tx,
                rx: out_rx,
                stop: stopped,
            },
        )
    }

    pub async fn send(&self, frame: Frame) -> Result<()> {
        tokio::time::timeout(Duration::from_secs(3), self.tx.send(frame.encode()?))
            .await
            .context("transport send queue stalled")?
            .map_err(|_| anyhow!("transport disconnected"))
    }

    pub async fn receive(&mut self) -> Result<Frame> {
        loop {
            let raw = self.rx.recv().await.context("transport disconnected")??;
            match Frame::decode(&raw) {
                Ok(frame) => return Ok(frame),
                Err(_) => self.invalid_frames += 1,
            }
        }
    }

    pub fn is_closed(&self) -> bool {
        self.tx.is_closed() || self.rx.is_closed()
    }

    pub fn close(&self) {
        let _ = self.stop.send(true);
    }

    pub async fn shutdown(&mut self) {
        self.close();
        let _ = tokio::time::timeout(Duration::from_secs(5), async {
            while self.rx.recv().await.is_some() {}
        })
        .await;
    }
}

impl Drop for Link {
    fn drop(&mut self) {
        self.close();
    }
}
