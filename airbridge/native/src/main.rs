use abt::{
    link::Link,
    session::Config,
    tcp,
    transport::{ble, usb},
};
use anyhow::{Result, ensure};
use clap::{Args, Parser, Subcommand, ValueEnum};
use std::{net::SocketAddr, process::ExitCode, time::Duration};

#[derive(Parser)]
#[command(
    version,
    about = "TCP streams over Pocket AirBridge HID/BLE. Use SSH or TLS for encryption."
)]
struct Cli {
    #[command(subcommand)]
    command: Command,
    #[arg(long, global = true, default_value_t = 500, value_parser = clap::value_parser!(u64).range(100..=5000))]
    retry_ms: u64,
    #[arg(long, global = true, default_value_t = 12, value_parser = clap::value_parser!(u64).range(4..=3600))]
    peer_timeout: u64,
    #[arg(long, global = true, default_value_t = 60, value_parser = clap::value_parser!(u64).range(1..=3600))]
    stall_timeout: u64,
    /// Maximum negotiated DATA/FIN window; use 2 to compare the original window.
    #[arg(long, global = true, default_value_t = 4, value_parser = parse_window)]
    window: usize,
    /// Maximum ACK coalescing delay; 0 sends an ACK for each completed frame.
    #[arg(long, global = true, default_value_t = 4, value_parser = clap::value_parser!(u64).range(0..=20))]
    ack_delay_ms: u64,
}

#[derive(Subcommand)]
enum Command {
    /// Open the vendor HID collection; defaults to --listen 127.0.0.1:2222.
    Usb {
        #[command(flatten)]
        endpoint: Endpoint,
        #[command(flatten)]
        identity: UsbIdentity,
        /// Exact HID path from `abt devices usb`.
        #[arg(long)]
        device: Option<String>,
    },
    /// Subscribe to BLE serial; defaults to --connect 127.0.0.1:22.
    Ble {
        #[command(flatten)]
        endpoint: Endpoint,
        /// Exact peripheral identifier or advertised name.
        #[arg(long)]
        device: Option<String>,
        #[arg(long, default_value_t = 5, value_parser = clap::value_parser!(u64).range(1..=30))]
        scan_seconds: u64,
    },
    /// List matching devices without opening a stream.
    Devices {
        #[arg(value_enum)]
        transport: Transport,
        #[command(flatten)]
        identity: UsbIdentity,
        #[arg(long, default_value_t = 5, value_parser = clap::value_parser!(u64).range(1..=30))]
        scan_seconds: u64,
    },
}

#[derive(Clone, Copy, ValueEnum)]
enum Transport {
    Usb,
    Ble,
}

#[derive(Args)]
struct UsbIdentity {
    #[arg(long, default_value = "03f0", value_parser = hex_id)]
    vid: u16,
    #[arg(long, default_value = "5341", value_parser = hex_id)]
    pid: u16,
}

fn hex_id(value: &str) -> Result<u16, std::num::ParseIntError> {
    u16::from_str_radix(value.trim_start_matches("0x"), 16)
}

fn parse_window(value: &str) -> Result<usize, String> {
    match value {
        "2" => Ok(2),
        "4" => Ok(4),
        _ => Err("window must be 2 or 4".into()),
    }
}

#[derive(Args)]
#[group(multiple = false)]
struct Endpoint {
    /// Local TCP listener; loopback addresses only.
    #[arg(long)]
    listen: Option<SocketAddr>,
    /// Fixed target host:port reached from this computer for each incoming stream.
    #[arg(long)]
    connect: Option<String>,
}

impl Endpoint {
    fn validate(&self) -> Result<()> {
        if let Some(address) = self.listen {
            ensure!(
                address.ip().is_loopback(),
                "--listen must be a loopback address"
            );
        }
        if let Some(target) = &self.connect {
            let (host, port) = target
                .rsplit_once(':')
                .ok_or_else(|| anyhow::anyhow!("--connect requires host:port"))?;
            ensure!(
                !host.is_empty() && port.parse::<u16>().is_ok_and(|port| port > 0),
                "--connect requires host:port with a nonzero port"
            );
        }
        Ok(())
    }
}

async fn operate(mut link: Link, endpoint: Endpoint, cfg: &Config) -> Result<()> {
    let result = {
        let run = async {
            match endpoint.listen {
                Some(address) => tcp::listen(&mut link, address, cfg).await,
                None => {
                    tcp::serve(
                        &mut link,
                        endpoint.connect.as_deref().expect("endpoint default"),
                        cfg,
                    )
                    .await
                }
            }
        };
        tokio::select! {
            result = run => result,
            signal = tokio::signal::ctrl_c() => { signal?; eprintln!("stopping abt"); Ok(()) },
        }
    };
    link.shutdown().await;
    result
}

async fn run(cli: Cli) -> Result<()> {
    eprintln!("abt {}", env!("CARGO_PKG_VERSION"));
    let cfg = Config {
        retry: Duration::from_millis(cli.retry_ms),
        peer_timeout: Duration::from_secs(cli.peer_timeout),
        stall_timeout: Duration::from_secs(cli.stall_timeout),
        window: cli.window,
        ack_delay: Duration::from_millis(cli.ack_delay_ms),
        ..Config::default()
    };
    match cli.command {
        Command::Usb {
            mut endpoint,
            identity,
            device,
        } => {
            if endpoint.listen.is_none() && endpoint.connect.is_none() {
                endpoint.listen = Some("127.0.0.1:2222".parse()?);
            }
            endpoint.validate()?;
            let link = usb::open(identity.vid, identity.pid, device.as_deref())?;
            operate(link, endpoint, &cfg).await
        }
        Command::Ble {
            mut endpoint,
            device,
            scan_seconds,
        } => {
            if endpoint.listen.is_none() && endpoint.connect.is_none() {
                endpoint.connect = Some("127.0.0.1:22".into());
            }
            endpoint.validate()?;
            let link = ble::open(device.as_deref(), scan_seconds).await?;
            operate(link, endpoint, &cfg).await
        }
        Command::Devices {
            transport,
            identity,
            scan_seconds,
        } => match transport {
            Transport::Usb => usb::devices(identity.vid, identity.pid),
            Transport::Ble => ble::devices(scan_seconds).await,
        },
    }
}

#[tokio::main]
async fn main() -> ExitCode {
    match run(Cli::parse()).await {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("abt: {error:#}");
            ExitCode::FAILURE
        }
    }
}
