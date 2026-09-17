use crate::frame::{FRAME_LEN, Report};
use anyhow::{Result, ensure};

pub fn control(count: usize, commit: bool, nonce: &[u8; 8]) -> Vec<u8> {
    let mut bytes = vec![0xa5; if commit { FRAME_LEN } else { count * FRAME_LEN }];
    bytes[..8].copy_from_slice(&[
        0xf0,
        b'A',
        b'B',
        b'P',
        1,
        if commit { 2 } else { 1 },
        count as u8,
        0,
    ]);
    bytes[8..16].copy_from_slice(nonce);
    bytes
}

pub fn is_control(bytes: &[u8]) -> bool {
    bytes.starts_with(&[0xf0, b'A', b'B', b'P'])
}

pub fn split(bytes: &[u8], count: usize) -> Result<Vec<Report>> {
    ensure!(
        !bytes.is_empty()
            && bytes.len() <= count * FRAME_LEN
            && bytes.len().is_multiple_of(FRAME_LEN),
        "invalid BLE packet length {}",
        bytes.len()
    );
    Ok(bytes
        .chunks_exact(FRAME_LEN)
        .map(|chunk| chunk.try_into().unwrap())
        .collect())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn probe_matches_browser_wire_format() {
        let bytes = control(2, false, &[0, 1, 2, 3, 4, 5, 6, 7]);
        assert_eq!(bytes.len(), 128);
        assert_eq!(
            &bytes[..16],
            &[240, 65, 66, 80, 1, 1, 2, 0, 0, 1, 2, 3, 4, 5, 6, 7]
        );
        assert!(bytes[16..].iter().all(|b| *b == 165));
        assert_eq!(control(3, true, &[0; 8]).len(), 64);
    }

    #[test]
    fn split_preserves_frames_and_rejects_unnegotiated_lengths() {
        let mut data = [0; 192];
        for (index, frame) in data.chunks_mut(64).enumerate() {
            frame.fill(index as u8);
        }
        let frames = split(&data, 3).unwrap();
        assert_eq!(frames, vec![[0; 64], [1; 64], [2; 64]]);
        for (bytes, count) in [(&data[..0], 3), (&data[..65], 3), (&data[..], 2)] {
            assert!(split(bytes, count).is_err());
        }
    }
}
