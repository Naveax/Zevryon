#![forbid(unsafe_code)]
#![deny(warnings)]

pub const ZR_UTF8_ABI_VERSION: u32 = 0x0001_0000;
pub const ZR_UTF8_DECODER_STORAGE_BYTES: usize = 128;
pub const ZR_UTF8_DECODER_STORAGE_ALIGN: usize = 8;

pub const ZR_UTF8_POLICY_STRICT: u32 = 0;
pub const ZR_UTF8_POLICY_REPLACE: u32 = 1;

pub const ZR_UTF8_ERROR_NONE: u32 = 0;
pub const ZR_UTF8_ERROR_DISCONTINUOUS_INPUT: u32 = 1;
pub const ZR_UTF8_ERROR_INVALID_LEAD_BYTE: u32 = 2;
pub const ZR_UTF8_ERROR_UNEXPECTED_CONTINUATION: u32 = 3;
pub const ZR_UTF8_ERROR_INVALID_CONTINUATION: u32 = 4;
pub const ZR_UTF8_ERROR_OVERLONG_ENCODING: u32 = 5;
pub const ZR_UTF8_ERROR_SURROGATE_CODE_POINT: u32 = 6;
pub const ZR_UTF8_ERROR_CODE_POINT_OUT_OF_RANGE: u32 = 7;
pub const ZR_UTF8_ERROR_TRUNCATED_SEQUENCE: u32 = 8;
pub const ZR_UTF8_ERROR_OUTPUT_BUDGET_EXCEEDED: u32 = 9;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ZrDecodedCodePoint {
    pub source_start: u64,
    pub value: u32,
    pub source_length: u8,
    pub replacement: u8,
    pub reserved: [u8; 2],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ZrUtf8DecodeStats {
    pub source_bytes: u64,
    pub emitted_codepoints: u64,
    pub invalid_sequences: u64,
    pub replacements: u64,
    pub chunks: u64,
    pub maximum_pending_continuations: u8,
    pub reserved: [u8; 7],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ZrUtf8DecodeError {
    pub kind: u32,
    pub reserved: u32,
    pub source_offset: u64,
}

#[repr(C, align(8))]
#[derive(Clone, Copy)]
pub struct ZrUtf8DecoderStorage {
    pub bytes: [u8; ZR_UTF8_DECODER_STORAGE_BYTES],
}

impl Default for ZrUtf8DecoderStorage {
    fn default() -> Self {
        Self {
            bytes: [0; ZR_UTF8_DECODER_STORAGE_BYTES],
        }
    }
}

const _: [(); 16] = [(); core::mem::size_of::<ZrDecodedCodePoint>()];
const _: [(); 48] = [(); core::mem::size_of::<ZrUtf8DecodeStats>()];
const _: [(); 16] = [(); core::mem::size_of::<ZrUtf8DecodeError>()];
const _: [(); ZR_UTF8_DECODER_STORAGE_BYTES] = [(); core::mem::size_of::<ZrUtf8DecoderStorage>()];
const _: [(); ZR_UTF8_DECODER_STORAGE_ALIGN] = [(); core::mem::align_of::<ZrUtf8DecoderStorage>()];

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn utf8_abi_records_are_stable() {
        assert_eq!(core::mem::size_of::<ZrDecodedCodePoint>(), 16);
        assert_eq!(core::mem::size_of::<ZrUtf8DecodeStats>(), 48);
        assert_eq!(core::mem::size_of::<ZrUtf8DecodeError>(), 16);
        assert_eq!(
            core::mem::size_of::<ZrUtf8DecoderStorage>(),
            ZR_UTF8_DECODER_STORAGE_BYTES
        );
        assert_eq!(
            core::mem::align_of::<ZrUtf8DecoderStorage>(),
            ZR_UTF8_DECODER_STORAGE_ALIGN
        );
    }
}
