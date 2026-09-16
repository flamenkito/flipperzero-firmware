use crate::{frame::FRAME_LEN, link::Link};
use anyhow::{Context, Result, ensure};
use hidapi::{HidApi, HidDevice};

pub fn devices(vid: u16, pid: u16) -> Result<()> {
    let api = HidApi::new().context("initialize HID; check USB permissions")?;
    for device in api
        .device_list()
        .filter(|d| d.vendor_id() == vid && d.product_id() == pid)
    {
        println!(
            "{:04x}:{:04x} usage={:04x}:{:04x} {} path={}",
            device.vendor_id(),
            device.product_id(),
            device.usage_page(),
            device.usage(),
            device.product_string().unwrap_or(""),
            device.path().to_string_lossy()
        );
    }
    Ok(())
}

pub fn open(vid: u16, pid: u16, path: Option<&str>) -> Result<Link> {
    ensure!(
        vid != 0x0483,
        "STM32 USB identity is not an AirBridge impersonation profile"
    );
    let api = HidApi::new().context("initialize HID; check USB permissions")?;
    let candidates: Vec<_> = api
        .device_list()
        .filter(|d| {
            d.vendor_id() == vid
                && d.product_id() == pid
                && d.usage_page() == 0xff00
                && d.usage() == 1
                && path.is_none_or(|p| d.path().to_string_lossy() == p)
        })
        .collect();
    ensure!(
        candidates.len() == 1,
        "expected one {:04x}:{:04x} vendor HID collection, found {}; run `abt devices usb`, use --device for duplicates, and keep Flipper in Bridge",
        vid,
        pid,
        candidates.len()
    );
    let device = candidates[0].open_device(&api).context(
        "open vendor HID; close other AirBridge clients and check Input Monitoring permissions",
    )?;
    eprintln!(
        "USB {:04x}:{:04x} vendor usage ff00:0001, 64-byte reports",
        vid, pid
    );
    let (link, mut driver) = Link::channel();
    std::thread::Builder::new()
        .name("abt-hid".into())
        .spawn(move || {
            let result = pump(device, &mut driver);
            if let Err(error) = result {
                let _ = driver.tx.blocking_send(Err(error));
            }
        })?;
    Ok(link)
}

fn pump(device: HidDevice, driver: &mut crate::link::Driver) -> Result<()> {
    let mut input = [0; FRAME_LEN + 1];
    while !*driver.stop.borrow() && !driver.tx.is_closed() {
        for _ in 0..2 {
            match driver.rx.try_recv() {
                Ok(frame) => {
                    // HIDAPI includes report ID zero in writes; the wire report remains 64 bytes.
                    let mut report = [0; FRAME_LEN + 1];
                    report[1..].copy_from_slice(&frame);
                    ensure!(
                        device.write(&report).context("USB write failed")? == report.len(),
                        "short USB write"
                    );
                }
                Err(tokio::sync::mpsc::error::TryRecvError::Empty) => break,
                Err(tokio::sync::mpsc::error::TryRecvError::Disconnected) => return Ok(()),
            }
        }
        let count = device
            .read_timeout(&mut input, 2)
            .context("USB read failed")?;
        if count == 0 {
            continue;
        }
        let bytes = if count == FRAME_LEN + 1 && input[0] == 0 {
            &input[1..]
        } else {
            &input[..count]
        };
        ensure!(
            bytes.len() == FRAME_LEN,
            "unexpected HID input report length {}",
            bytes.len()
        );
        if driver.tx.blocking_send(Ok(bytes.try_into()?)).is_err() {
            break;
        }
    }
    Ok(())
}
