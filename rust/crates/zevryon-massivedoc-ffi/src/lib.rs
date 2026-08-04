#![deny(unsafe_op_in_unsafe_fn)]
#![deny(warnings)]
// Every foreign pointer is checked for null and alignment before the audited
// unsafe block. Writability and lifetime remain the caller's ABI contract.
#![allow(clippy::not_unsafe_ptr_arg_deref)]

use core::mem::{align_of, size_of};
use core::{ptr, slice};
use zevryon_massivedoc::{
    chunk_descriptor_offset, chunk_range_within_segment, plan_record_slice,
    record_chunk_span_within_table, record_descriptor_offset, ChunkDescriptor, RecordDescriptor,
    SlicePlan, CHUNK_DESCRIPTOR_BYTES, MASSIVEDOC_ABI_VERSION, RECORD_DESCRIPTOR_BYTES,
};

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ZrMassiveDocRecordDescriptor {
    pub logical_id: u64,
    pub first_chunk: u64,
    pub length: u64,
    pub chunk_count: u32,
    pub crc32: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ZrMassiveDocChunkDescriptor {
    pub segment_id: u32,
    pub reserved: u32,
    pub offset: u64,
    pub length: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ZrMassiveDocSlicePlan {
    pub offset: u64,
    pub length: u64,
}

const _: [(); RECORD_DESCRIPTOR_BYTES] = [(); size_of::<ZrMassiveDocRecordDescriptor>()];
const _: [(); CHUNK_DESCRIPTOR_BYTES] = [(); size_of::<ZrMassiveDocChunkDescriptor>()];
const _: [(); 16] = [(); size_of::<ZrMassiveDocSlicePlan>()];

fn aligned<T>(value: *const T) -> bool {
    !value.is_null() && (value as usize).is_multiple_of(align_of::<T>())
}

fn record_from_abi(value: ZrMassiveDocRecordDescriptor) -> RecordDescriptor {
    RecordDescriptor {
        logical_id: value.logical_id,
        first_chunk: value.first_chunk,
        length: value.length,
        chunk_count: value.chunk_count,
        crc32: value.crc32,
    }
}

fn record_to_abi(value: RecordDescriptor) -> ZrMassiveDocRecordDescriptor {
    ZrMassiveDocRecordDescriptor {
        logical_id: value.logical_id,
        first_chunk: value.first_chunk,
        length: value.length,
        chunk_count: value.chunk_count,
        crc32: value.crc32,
    }
}

fn chunk_from_abi(value: ZrMassiveDocChunkDescriptor) -> ChunkDescriptor {
    ChunkDescriptor {
        segment_id: value.segment_id,
        offset: value.offset,
        length: value.length,
    }
}

fn chunk_to_abi(value: ChunkDescriptor) -> ZrMassiveDocChunkDescriptor {
    ZrMassiveDocChunkDescriptor {
        segment_id: value.segment_id,
        reserved: 0,
        offset: value.offset,
        length: value.length,
    }
}

fn slice_to_abi(value: SlicePlan) -> ZrMassiveDocSlicePlan {
    ZrMassiveDocSlicePlan {
        offset: value.offset,
        length: value.length,
    }
}

#[no_mangle]
pub extern "C" fn zr_massivedoc_abi_version() -> u32 {
    MASSIVEDOC_ABI_VERSION
}

#[no_mangle]
pub extern "C" fn zr_massivedoc_record_descriptor_size() -> usize {
    RECORD_DESCRIPTOR_BYTES
}

#[no_mangle]
pub extern "C" fn zr_massivedoc_chunk_descriptor_size() -> usize {
    CHUNK_DESCRIPTOR_BYTES
}

#[no_mangle]
pub extern "C" fn zr_massivedoc_encode_record_descriptor(
    descriptor: *const ZrMassiveDocRecordDescriptor,
    output: *mut u8,
    output_len: usize,
) -> u8 {
    if !aligned(descriptor) || output.is_null() || output_len != RECORD_DESCRIPTOR_BYTES {
        return 0;
    }
    let descriptor = unsafe {
        // SAFETY: The descriptor pointer is non-null and aligned. The caller keeps
        // the pointed-to record alive for this non-mutating call.
        descriptor.read()
    };
    let encoded = record_from_abi(descriptor).encode();
    unsafe {
        // SAFETY: The ABI requires output_len writable bytes and the exact size was checked.
        ptr::copy_nonoverlapping(encoded.as_ptr(), output, encoded.len());
    }
    1
}

#[no_mangle]
pub extern "C" fn zr_massivedoc_decode_record_descriptor(
    input: *const u8,
    input_len: usize,
    output: *mut ZrMassiveDocRecordDescriptor,
) -> u8 {
    if input.is_null() || input_len != RECORD_DESCRIPTOR_BYTES || !aligned(output) {
        return 0;
    }
    let bytes = unsafe {
        // SAFETY: The caller provides input_len readable bytes and the exact size was checked.
        slice::from_raw_parts(input, input_len)
    };
    let Some(descriptor) = RecordDescriptor::decode(bytes) else {
        return 0;
    };
    unsafe {
        // SAFETY: The output pointer is non-null, aligned, and writable by contract.
        output.write(record_to_abi(descriptor));
    }
    1
}

#[no_mangle]
pub extern "C" fn zr_massivedoc_encode_chunk_descriptor(
    descriptor: *const ZrMassiveDocChunkDescriptor,
    output: *mut u8,
    output_len: usize,
) -> u8 {
    if !aligned(descriptor) || output.is_null() || output_len != CHUNK_DESCRIPTOR_BYTES {
        return 0;
    }
    let descriptor = unsafe {
        // SAFETY: The descriptor pointer is non-null and aligned. The caller keeps
        // the pointed-to record alive for this non-mutating call.
        descriptor.read()
    };
    let encoded = chunk_from_abi(descriptor).encode();
    unsafe {
        // SAFETY: The ABI requires output_len writable bytes and the exact size was checked.
        ptr::copy_nonoverlapping(encoded.as_ptr(), output, encoded.len());
    }
    1
}

#[no_mangle]
pub extern "C" fn zr_massivedoc_decode_chunk_descriptor(
    input: *const u8,
    input_len: usize,
    output: *mut ZrMassiveDocChunkDescriptor,
) -> u8 {
    if input.is_null() || input_len != CHUNK_DESCRIPTOR_BYTES || !aligned(output) {
        return 0;
    }
    let bytes = unsafe {
        // SAFETY: The caller provides input_len readable bytes and the exact size was checked.
        slice::from_raw_parts(input, input_len)
    };
    let Some(descriptor) = ChunkDescriptor::decode(bytes) else {
        return 0;
    };
    unsafe {
        // SAFETY: The output pointer is non-null, aligned, and writable by contract.
        output.write(chunk_to_abi(descriptor));
    }
    1
}

#[no_mangle]
pub extern "C" fn zr_massivedoc_record_descriptor_offset(
    record_index: u64,
    output: *mut u64,
) -> u8 {
    if !aligned(output) {
        return 0;
    }
    let Some(offset) = record_descriptor_offset(record_index) else {
        return 0;
    };
    unsafe {
        // SAFETY: The output pointer is non-null, aligned, and writable by contract.
        output.write(offset);
    }
    1
}

#[no_mangle]
pub extern "C" fn zr_massivedoc_chunk_descriptor_offset(chunk_index: u64, output: *mut u64) -> u8 {
    if !aligned(output) {
        return 0;
    }
    let Some(offset) = chunk_descriptor_offset(chunk_index) else {
        return 0;
    };
    unsafe {
        // SAFETY: The output pointer is non-null, aligned, and writable by contract.
        output.write(offset);
    }
    1
}

#[no_mangle]
pub extern "C" fn zr_massivedoc_plan_record_slice(
    record_length: u64,
    byte_offset: u64,
    max_bytes: u64,
    output: *mut ZrMassiveDocSlicePlan,
) -> u8 {
    if !aligned(output) {
        return 0;
    }
    let Some(plan) = plan_record_slice(record_length, byte_offset, max_bytes) else {
        return 0;
    };
    unsafe {
        // SAFETY: The output pointer is non-null, aligned, and writable by contract.
        output.write(slice_to_abi(plan));
    }
    1
}

#[no_mangle]
pub extern "C" fn zr_massivedoc_chunk_range_within_segment(
    segment_bytes: u64,
    offset: u64,
    length: u64,
) -> u8 {
    u8::from(chunk_range_within_segment(segment_bytes, offset, length))
}

#[no_mangle]
pub extern "C" fn zr_massivedoc_record_chunk_span_within_table(
    first_chunk: u64,
    chunk_count: u32,
    total_chunks: u64,
) -> u8 {
    u8::from(record_chunk_span_within_table(
        first_chunk,
        chunk_count,
        total_chunks,
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ffi_codec_and_range_contracts_are_fail_closed() {
        let record = ZrMassiveDocRecordDescriptor {
            logical_id: 9,
            first_chunk: 11,
            length: 4096,
            chunk_count: 3,
            crc32: 0x1234_5678,
        };
        let mut record_bytes = [0_u8; RECORD_DESCRIPTOR_BYTES];
        assert_eq!(
            zr_massivedoc_encode_record_descriptor(
                &record,
                record_bytes.as_mut_ptr(),
                record_bytes.len(),
            ),
            1
        );
        let mut decoded_record = ZrMassiveDocRecordDescriptor::default();
        assert_eq!(
            zr_massivedoc_decode_record_descriptor(
                record_bytes.as_ptr(),
                record_bytes.len(),
                &mut decoded_record,
            ),
            1
        );
        assert_eq!(decoded_record, record);
        assert_eq!(
            zr_massivedoc_encode_record_descriptor(
                &record,
                record_bytes.as_mut_ptr(),
                record_bytes.len() - 1,
            ),
            0
        );

        let mut plan = ZrMassiveDocSlicePlan::default();
        assert_eq!(zr_massivedoc_plan_record_slice(100, 90, 40, &mut plan), 1);
        assert_eq!(plan.offset, 90);
        assert_eq!(plan.length, 10);
        assert_eq!(zr_massivedoc_plan_record_slice(100, 101, 1, &mut plan), 0);
        assert_eq!(zr_massivedoc_chunk_range_within_segment(1024, 1000, 24), 1);
        assert_eq!(zr_massivedoc_chunk_range_within_segment(1024, 1000, 25), 0);
    }
}
