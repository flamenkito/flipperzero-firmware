use crate::{frame::FRAME_LEN, link::Link};
use anyhow::{Context, Result, bail, ensure};
use btleplug::{
    api::{
        Central, CentralEvent, CharPropFlags, Manager as _, Peripheral as _, ScanFilter, WriteType,
    },
    platform::{Adapter, Manager, Peripheral},
};
use futures_util::StreamExt;
use std::time::Duration;
use uuid::Uuid;

pub const SERVICE: Uuid = Uuid::from_u128(0x7b871228_baf0_c5b4_5f46_9c2613d627a3);
pub const TX: Uuid = Uuid::from_u128(0x87825ec0_7398_8cb7_3242_b083eaa34f27);
pub const RX: Uuid = Uuid::from_u128(0x152f7eeb_e3b7_5898_ba41_7ff66121c98d);

async fn scan(seconds: u64) -> Result<(Adapter, Vec<Peripheral>)> {
    let manager = Manager::new()
        .await
        .context("initialize Bluetooth; allow Bluetooth access for abt/Terminal")?;
    let adapter = manager
        .adapters()
        .await?
        .into_iter()
        .next()
        .context("no Bluetooth adapter")?;
    adapter
        .start_scan(ScanFilter {
            services: vec![SERVICE],
        })
        .await
        .context("start BLE scan")?;
    tokio::time::sleep(Duration::from_secs(seconds)).await;
    adapter.stop_scan().await?;
    let mut peers = Vec::new();
    for peer in adapter.peripherals().await? {
        if let Some(properties) = peer.properties().await?
            && properties.services.contains(&SERVICE)
        {
            peers.push(peer);
        }
    }
    Ok((adapter, peers))
}

pub async fn devices(seconds: u64) -> Result<()> {
    let (_, peers) = scan(seconds).await?;
    for peer in peers {
        let name = peer
            .properties()
            .await?
            .and_then(|p| p.local_name)
            .unwrap_or_default();
        println!("{} {}", peer.id(), name);
    }
    Ok(())
}

pub async fn open(selector: Option<&str>, seconds: u64) -> Result<Link> {
    eprintln!("scanning BLE for AirBridge serial service ({seconds}s)");
    let (adapter, peers) = scan(seconds).await?;
    let mut matches = Vec::new();
    for peer in peers {
        let name = peer
            .properties()
            .await?
            .and_then(|p| p.local_name)
            .unwrap_or_default();
        if selector.is_none_or(|s| s == peer.id().to_string() || s == name) {
            matches.push(peer);
        }
    }
    ensure!(
        matches.len() == 1,
        "expected one AirBridge BLE peer, found {}; run `abt devices ble` or select --device; close browser BLE clients",
        matches.len()
    );
    let peer = matches.remove(0);
    eprintln!(
        "connecting BLE {} (confirm OS/Flipper pairing if prompted)",
        peer.id()
    );
    let setup = async {
        if !peer.is_connected().await? {
            peer.connect().await?;
        }
        peer.discover_services().await?;
        let chars = peer.characteristics();
        let tx = chars
            .iter()
            .find(|c| c.uuid == TX && c.service_uuid == SERVICE)
            .context("missing AirBridge TX")?
            .clone();
        let rx = chars
            .iter()
            .find(|c| c.uuid == RX && c.service_uuid == SERVICE)
            .context("missing AirBridge RX")?
            .clone();
        ensure!(
            tx.properties.contains(CharPropFlags::NOTIFY),
            "TX does not support notifications"
        );
        let write_type = if rx
            .properties
            .contains(CharPropFlags::WRITE_WITHOUT_RESPONSE)
        {
            WriteType::WithoutResponse
        } else {
            ensure!(
                rx.properties.contains(CharPropFlags::WRITE),
                "RX is not writable"
            );
            WriteType::WithResponse
        };
        let notifications = peer.notifications().await?;
        peer.subscribe(&tx).await?;
        Ok::<_, anyhow::Error>((tx, rx, write_type, notifications))
    };
    let setup = tokio::time::timeout(Duration::from_secs(30), setup)
        .await
        .context("BLE discovery timed out")
        .and_then(|result| result);
    let (tx_char, rx_char, write_type, mut notifications) = match setup {
        Ok(ready) => ready,
        Err(error) => {
            let _ = peer.disconnect().await;
            return Err(error);
        }
    };
    eprintln!("BLE subscribed, MTU={}, write={write_type:?}", peer.mtu());
    let mut events = adapter.events().await?;
    let (link, mut driver) = Link::channel();
    tokio::spawn(async move {
        let run = async {
            loop {
                tokio::select! {
                    _ = driver.stop.changed() => return Ok(()),
                    frame = driver.rx.recv() => {
                        let Some(frame) = frame else { return Ok(()) };
                        tokio::time::timeout(Duration::from_secs(3), peer.write(&rx_char, &frame, write_type)).await.context("BLE write timed out")??;
                    }
                    notification = notifications.next() => {
                        let notification = notification.context("BLE notification stream ended")?;
                        if notification.uuid != TX { continue; }
                        ensure!(notification.value.len() == FRAME_LEN, "unexpected BLE frame length {}", notification.value.len());
                        if driver.tx.send(Ok(notification.value.as_slice().try_into()?)).await.is_err() { return Ok(()) }
                    }
                    event = events.next() => {
                        match event {
                            Some(CentralEvent::DeviceDisconnected(id)) if id == peer.id() => bail!("BLE disconnected"),
                            None => bail!("Bluetooth adapter stopped"),
                            _ => {},
                        }
                    }
                }
            }
        }.await;
        if let Err(error) = run {
            let _ = driver.tx.send(Err(error)).await;
        }
        let _ = tokio::time::timeout(Duration::from_secs(2), peer.unsubscribe(&tx_char)).await;
        let _ = tokio::time::timeout(Duration::from_secs(2), peer.disconnect()).await;
    });
    Ok(link)
}
