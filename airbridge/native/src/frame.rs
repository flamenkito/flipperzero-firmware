use anyhow::{Result, bail, ensure};

pub const FRAME_LEN: usize = 64;
pub const DATA_LEN: usize = 40;
pub const WINDOW: usize = 2;
pub type Report = [u8; FRAME_LEN];
const MAGIC: &[u8; 4] = b"ABT1";

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum Kind {
    Open = 1,
    Accept = 2,
    Data = 3,
    Ack = 4,
    Fin = 5,
    Reset = 6,
    Ping = 7,
    Pong = 8,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Frame {
    pub kind: Kind,
    pub session: u64,
    pub sequence: u32,
    pub payload: Vec<u8>,
}

impl Frame {
    pub fn control(kind: Kind, session: u64, sequence: u32) -> Self {
        let payload = if matches!(kind, Kind::Open | Kind::Accept) {
            vec![WINDOW as u8, DATA_LEN as u8]
        } else {
            Vec::new()
        };
        Self {
            kind,
            session,
            sequence,
            payload,
        }
    }

    pub fn data(session: u64, sequence: u32, payload: &[u8]) -> Self {
        Self {
            kind: Kind::Data,
            session,
            sequence,
            payload: payload.to_vec(),
        }
    }

    fn validate(&self) -> Result<()> {
        ensure!(self.session != 0, "zero session identifier");
        ensure!(self.payload.len() <= DATA_LEN, "oversized frame");
        match self.kind {
            Kind::Open | Kind::Accept => {
                ensure!(
                    self.payload == [WINDOW as u8, DATA_LEN as u8],
                    "unsupported stream capability"
                );
            }
            Kind::Data => ensure!(!self.payload.is_empty(), "empty DATA"),
            _ => ensure!(self.payload.is_empty(), "unexpected control payload"),
        }
        if !matches!(self.kind, Kind::Data | Kind::Ack | Kind::Fin) {
            ensure!(self.sequence == 0, "unexpected control sequence");
        }
        Ok(())
    }

    pub fn encode(&self) -> Result<Report> {
        self.validate()?;
        let mut bytes = [0; FRAME_LEN];
        bytes[..4].copy_from_slice(MAGIC);
        bytes[4] = self.kind as u8;
        bytes[6] = self.payload.len() as u8;
        bytes[8..16].copy_from_slice(&self.session.to_be_bytes());
        bytes[16..20].copy_from_slice(&self.sequence.to_be_bytes());
        bytes[20..20 + self.payload.len()].copy_from_slice(&self.payload);
        let crc = crc32fast::hash(&bytes[..60]);
        bytes[60..].copy_from_slice(&crc.to_be_bytes());
        Ok(bytes)
    }

    pub fn decode(bytes: &[u8]) -> Result<Self> {
        ensure!(bytes.len() == FRAME_LEN, "incorrect report length");
        ensure!(&bytes[..4] == MAGIC, "not an ABT1 stream frame");
        ensure!(
            crc32fast::hash(&bytes[..60]) == u32::from_be_bytes(bytes[60..64].try_into()?),
            "frame CRC mismatch"
        );
        ensure!(bytes[5] == 0 && bytes[7] == 0, "reserved bits set");
        let len = usize::from(bytes[6]);
        ensure!(len <= DATA_LEN, "oversized frame");
        ensure!(
            bytes[20 + len..60].iter().all(|b| *b == 0),
            "nonzero padding"
        );
        let kind = match bytes[4] {
            1 => Kind::Open,
            2 => Kind::Accept,
            3 => Kind::Data,
            4 => Kind::Ack,
            5 => Kind::Fin,
            6 => Kind::Reset,
            7 => Kind::Ping,
            8 => Kind::Pong,
            _ => bail!("unknown frame type"),
        };
        let frame = Self {
            kind,
            session: u64::from_be_bytes(bytes[8..16].try_into()?),
            sequence: u32::from_be_bytes(bytes[16..20].try_into()?),
            payload: bytes[20..20 + len].to_vec(),
        };
        frame.validate()?;
        Ok(frame)
    }
}
