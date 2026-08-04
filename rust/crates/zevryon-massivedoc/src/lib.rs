#![forbid(unsafe_code)]
#![deny(warnings)]

pub const RECORD_DESCRIPTOR_BYTES: usize = 32;
pub const CHUNK_DESCRIPTOR_BYTES: usize = 24;
pub const MASSIVEDOC_ABI_VERSION: u32 = 0x0001_0000;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct RecordDescriptor {
    pub logical_id: u64,
    pub first_chunk: u64,
    pub length: u64,
    pub chunk_count: u32,
    pub crc32: u32,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ChunkDescriptor {
    pub segment_id: u32,
    pub offset: u64,
    pub length: u64,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct SlicePlan {
    pub offset: u64,
    pub length: u64,
}

impl RecordDescriptor {
    #[must_use]
    pub fn encode(self) -> [u8; RECORD_DESCRIPTOR_BYTES] {
        let mut bytes = [0_u8; RECORD_DESCRIPTOR_BYTES];
        bytes[0..8].copy_from_slice(&self.logical_id.to_le_bytes());
        bytes[8..16].copy_from_slice(&self.first_chunk.to_le_bytes());
        bytes[16..24].copy_from_slice(&self.length.to_le_bytes());
        bytes[24..28].copy_from_slice(&self.chunk_count.to_le_bytes());
        bytes[28..32].copy_from_slice(&self.crc32.to_le_bytes());
        bytes
    }

    pub fn decode(bytes: &[u8]) -> Option<Self> {
        let bytes: &[u8; RECORD_DESCRIPTOR_BYTES] = bytes.try_into().ok()?;
        Some(Self {
            logical_id: u64::from_le_bytes(bytes[0..8].try_into().ok()?),
            first_chunk: u64::from_le_bytes(bytes[8..16].try_into().ok()?),
            length: u64::from_le_bytes(bytes[16..24].try_into().ok()?),
            chunk_count: u32::from_le_bytes(bytes[24..28].try_into().ok()?),
            crc32: u32::from_le_bytes(bytes[28..32].try_into().ok()?),
        })
    }
}

impl ChunkDescriptor {
    #[must_use]
    pub fn encode(self) -> [u8; CHUNK_DESCRIPTOR_BYTES] {
        let mut bytes = [0_u8; CHUNK_DESCRIPTOR_BYTES];
        bytes[0..4].copy_from_slice(&self.segment_id.to_le_bytes());
        bytes[8..16].copy_from_slice(&self.offset.to_le_bytes());
        bytes[16..24].copy_from_slice(&self.length.to_le_bytes());
        bytes
    }

    pub fn decode(bytes: &[u8]) -> Option<Self> {
        let bytes: &[u8; CHUNK_DESCRIPTOR_BYTES] = bytes.try_into().ok()?;
        Some(Self {
            segment_id: u32::from_le_bytes(bytes[0..4].try_into().ok()?),
            offset: u64::from_le_bytes(bytes[8..16].try_into().ok()?),
            length: u64::from_le_bytes(bytes[16..24].try_into().ok()?),
        })
    }
}

pub fn record_descriptor_offset(record_index: u64) -> Option<u64> {
    record_index.checked_mul(RECORD_DESCRIPTOR_BYTES as u64)
}

pub fn chunk_descriptor_offset(chunk_index: u64) -> Option<u64> {
    chunk_index.checked_mul(CHUNK_DESCRIPTOR_BYTES as u64)
}

pub fn plan_record_slice(
    record_length: u64,
    byte_offset: u64,
    max_bytes: u64,
) -> Option<SlicePlan> {
    if byte_offset > record_length {
        return None;
    }
    let available = record_length - byte_offset;
    Some(SlicePlan {
        offset: byte_offset,
        length: available.min(max_bytes),
    })
}

#[must_use]
pub fn chunk_range_within_segment(segment_bytes: u64, offset: u64, length: u64) -> bool {
    offset <= segment_bytes && length <= segment_bytes - offset
}

#[must_use]
pub fn record_chunk_span_within_table(
    first_chunk: u64,
    chunk_count: u32,
    total_chunks: u64,
) -> bool {
    first_chunk
        .checked_add(u64::from(chunk_count))
        .is_some_and(|end| end <= total_chunks)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn record_descriptor_matches_the_existing_disk_layout() {
        let descriptor = RecordDescriptor {
            logical_id: 0x0102_0304_0506_0708,
            first_chunk: 0x1112_1314_1516_1718,
            length: 0x2122_2324_2526_2728,
            chunk_count: 0x3132_3334,
            crc32: 0x4142_4344,
        };
        let bytes = descriptor.encode();
        assert_eq!(&bytes[0..8], &descriptor.logical_id.to_le_bytes());
        assert_eq!(&bytes[8..16], &descriptor.first_chunk.to_le_bytes());
        assert_eq!(&bytes[16..24], &descriptor.length.to_le_bytes());
        assert_eq!(&bytes[24..28], &descriptor.chunk_count.to_le_bytes());
        assert_eq!(&bytes[28..32], &descriptor.crc32.to_le_bytes());
        assert_eq!(RecordDescriptor::decode(&bytes), Some(descriptor));
        assert_eq!(RecordDescriptor::decode(&bytes[..31]), None);
    }

    #[test]
    fn chunk_descriptor_preserves_reserved_padding_compatibility() {
        let descriptor = ChunkDescriptor {
            segment_id: 17,
            offset: 64 * 1024,
            length: 4096,
        };
        let mut bytes = descriptor.encode();
        assert_eq!(&bytes[4..8], &[0, 0, 0, 0]);
        bytes[4..8].copy_from_slice(&0xDEAD_BEEFu32.to_le_bytes());
        assert_eq!(ChunkDescriptor::decode(&bytes), Some(descriptor));
        assert_eq!(ChunkDescriptor::decode(&bytes[..23]), None);
    }

    #[test]
    fn descriptor_offsets_fail_closed_on_overflow() {
        assert_eq!(record_descriptor_offset(7), Some(224));
        assert_eq!(chunk_descriptor_offset(7), Some(168));
        assert_eq!(record_descriptor_offset(u64::MAX), None);
        assert_eq!(chunk_descriptor_offset(u64::MAX), None);
    }

    #[test]
    fn bounded_slice_and_chunk_ranges_are_checked() {
        assert_eq!(
            plan_record_slice(100, 25, 40),
            Some(SlicePlan {
                offset: 25,
                length: 40,
            })
        );
        assert_eq!(
            plan_record_slice(100, 90, 40),
            Some(SlicePlan {
                offset: 90,
                length: 10,
            })
        );
        assert_eq!(plan_record_slice(100, 101, 1), None);
        assert!(chunk_range_within_segment(1024, 1000, 24));
        assert!(!chunk_range_within_segment(1024, 1000, 25));
        assert!(record_chunk_span_within_table(8, 4, 12));
        assert!(!record_chunk_span_within_table(8, 5, 12));
        assert!(!record_chunk_span_within_table(u64::MAX, 1, u64::MAX));
    }
}
