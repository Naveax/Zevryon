#![deny(unsafe_op_in_unsafe_fn)]
#![deny(warnings)]
// Pointer validity is checked for null and alignment before every audited
// unsafe block. Writability and lifetime remain the foreign caller's contract.
#![allow(clippy::not_unsafe_ptr_arg_deref)]

use core::mem::{align_of, size_of};
use core::{ptr, slice};
use zevryon_unicode_abi::{
    ZrDecodedCodePoint, ZrUtf8DecodeError, ZrUtf8DecodeStats, ZrUtf8DecoderStorage,
    ZR_UTF8_ABI_VERSION, ZR_UTF8_DECODER_STORAGE_ALIGN, ZR_UTF8_DECODER_STORAGE_BYTES,
};
use zevryon_unicode_stream::{
    DecodeError, DecodeStats, DecodedCodePoint, ErrorPolicy, Utf8StreamDecoder,
};

const _: [(); 1] = [(); (size_of::<Utf8StreamDecoder>() <= ZR_UTF8_DECODER_STORAGE_BYTES) as usize];
const _: [(); 1] =
    [(); (align_of::<Utf8StreamDecoder>() <= ZR_UTF8_DECODER_STORAGE_ALIGN) as usize];

fn pointer_aligned<T>(pointer: *const T) -> bool {
    !pointer.is_null() && (pointer as usize).is_multiple_of(align_of::<T>())
}

fn storage_aligned(storage: *const ZrUtf8DecoderStorage) -> bool {
    pointer_aligned(storage) && (storage as usize).is_multiple_of(align_of::<Utf8StreamDecoder>())
}

fn with_decoder<R>(
    storage: *const ZrUtf8DecoderStorage,
    operation: impl FnOnce(&Utf8StreamDecoder) -> R,
) -> Option<R> {
    if !storage_aligned(storage) {
        return None;
    }
    let decoder = unsafe {
        // SAFETY: The storage is non-null and aligned. The caller must have
        // initialized it through zr_utf8_decoder_init and keep it alive here.
        &*storage.cast::<Utf8StreamDecoder>()
    };
    decoder.is_initialized().then(|| operation(decoder))
}

fn with_decoder_mut<R>(
    storage: *mut ZrUtf8DecoderStorage,
    operation: impl FnOnce(&mut Utf8StreamDecoder) -> R,
) -> Option<R> {
    if !storage_aligned(storage) {
        return None;
    }
    let decoder = unsafe {
        // SAFETY: The storage is non-null, aligned, initialized, and exclusively
        // borrowed by the foreign caller for this mutating operation.
        &mut *storage.cast::<Utf8StreamDecoder>()
    };
    decoder.is_initialized().then(|| operation(decoder))
}

fn abi_point(point: DecodedCodePoint) -> ZrDecodedCodePoint {
    ZrDecodedCodePoint {
        source_start: point.source_start,
        value: point.value,
        source_length: point.source_length,
        replacement: u8::from(point.replacement),
        reserved: [0; 2],
    }
}

fn abi_stats(stats: DecodeStats) -> ZrUtf8DecodeStats {
    ZrUtf8DecodeStats {
        source_bytes: stats.source_bytes,
        emitted_codepoints: stats.emitted_codepoints,
        invalid_sequences: stats.invalid_sequences,
        replacements: stats.replacements,
        chunks: stats.chunks,
        maximum_pending_continuations: stats.maximum_pending_continuations,
        reserved: [0; 7],
    }
}

fn clear_error(error: *mut ZrUtf8DecodeError) {
    if pointer_aligned(error) {
        unsafe {
            // SAFETY: The pointer is non-null, aligned, and writable by contract.
            error.write(ZrUtf8DecodeError::default());
        }
    }
}

fn write_error(error: *mut ZrUtf8DecodeError, value: DecodeError) {
    if pointer_aligned(error) {
        unsafe {
            // SAFETY: The pointer is non-null, aligned, and writable by contract.
            error.write(ZrUtf8DecodeError {
                kind: value.kind as u32,
                reserved: 0,
                source_offset: value.source_offset,
            });
        }
    }
}

fn write_count(written: *mut usize, value: usize) {
    if pointer_aligned(written) {
        unsafe {
            // SAFETY: The pointer is non-null, aligned, and writable by contract.
            written.write(value);
        }
    }
}

fn input_slice<'a>(bytes: *const u8, byte_count: usize) -> Option<&'a [u8]> {
    if byte_count == 0 {
        return Some(&[]);
    }
    if bytes.is_null() {
        return None;
    }
    Some(unsafe {
        // SAFETY: The pointer is non-null and the caller promises byte_count
        // readable bytes for the duration of this call.
        slice::from_raw_parts(bytes, byte_count)
    })
}

fn output_valid(output: *mut ZrDecodedCodePoint, capacity: usize) -> bool {
    capacity == 0 || pointer_aligned(output)
}

#[no_mangle]
pub extern "C" fn zr_utf8_abi_version() -> u32 {
    ZR_UTF8_ABI_VERSION
}

#[no_mangle]
pub extern "C" fn zr_utf8_decoder_storage_size() -> usize {
    ZR_UTF8_DECODER_STORAGE_BYTES
}

#[no_mangle]
pub extern "C" fn zr_utf8_decoder_storage_alignment() -> usize {
    ZR_UTF8_DECODER_STORAGE_ALIGN
}

#[no_mangle]
pub extern "C" fn zr_utf8_decoder_init(storage: *mut ZrUtf8DecoderStorage, policy: u32) -> u8 {
    if !storage_aligned(storage) {
        return 0;
    }
    let Some(policy) = ErrorPolicy::from_abi(policy) else {
        return 0;
    };
    unsafe {
        // SAFETY: The opaque storage is non-null, aligned, and large enough by
        // the compile-time assertions above.
        storage
            .cast::<Utf8StreamDecoder>()
            .write(Utf8StreamDecoder::new(policy));
    }
    1
}

#[no_mangle]
pub extern "C" fn zr_utf8_decoder_clear(storage: *mut ZrUtf8DecoderStorage) {
    if !storage_aligned(storage) {
        return;
    }
    unsafe {
        // SAFETY: The storage points to exactly the fixed writable ABI record.
        ptr::write_bytes(storage.cast::<u8>(), 0, ZR_UTF8_DECODER_STORAGE_BYTES);
    }
}

#[no_mangle]
pub extern "C" fn zr_utf8_decoder_valid(storage: *const ZrUtf8DecoderStorage) -> u8 {
    u8::from(with_decoder(storage, |_| ()).is_some())
}

#[no_mangle]
pub extern "C" fn zr_utf8_decoder_feed(
    storage: *mut ZrUtf8DecoderStorage,
    bytes: *const u8,
    byte_count: usize,
    absolute_source_offset: u64,
    output: *mut ZrDecodedCodePoint,
    output_capacity: usize,
    written: *mut usize,
    error: *mut ZrUtf8DecodeError,
) -> u8 {
    if !pointer_aligned(written)
        || !pointer_aligned(error)
        || !output_valid(output, output_capacity)
    {
        return 0;
    }
    let Some(input) = input_slice(bytes, byte_count) else {
        return 0;
    };

    clear_error(error);
    write_count(written, 0);
    let mut produced = 0usize;
    let Some(result) = with_decoder_mut(storage, |decoder| {
        decoder.feed(input, absolute_source_offset, |point| {
            if produced >= output_capacity {
                return Err(());
            }
            unsafe {
                // SAFETY: output is aligned when capacity is non-zero and the
                // index is bounded by output_capacity.
                output.add(produced).write(abi_point(point));
            }
            produced += 1;
            Ok(())
        })
    }) else {
        return 0;
    };
    write_count(written, produced);
    match result {
        Ok(()) => 1,
        Err(value) => {
            write_error(error, value);
            0
        }
    }
}

#[no_mangle]
pub extern "C" fn zr_utf8_decoder_finish(
    storage: *mut ZrUtf8DecoderStorage,
    output: *mut ZrDecodedCodePoint,
    output_capacity: usize,
    written: *mut usize,
    error: *mut ZrUtf8DecodeError,
) -> u8 {
    if !pointer_aligned(written)
        || !pointer_aligned(error)
        || !output_valid(output, output_capacity)
    {
        return 0;
    }

    clear_error(error);
    write_count(written, 0);
    let mut produced = 0usize;
    let Some(result) = with_decoder_mut(storage, |decoder| {
        decoder.finish(|point| {
            if produced >= output_capacity {
                return Err(());
            }
            unsafe {
                // SAFETY: output is aligned when capacity is non-zero and the
                // index is bounded by output_capacity.
                output.add(produced).write(abi_point(point));
            }
            produced += 1;
            Ok(())
        })
    }) else {
        return 0;
    };
    write_count(written, produced);
    match result {
        Ok(()) => 1,
        Err(value) => {
            write_error(error, value);
            0
        }
    }
}

#[no_mangle]
pub extern "C" fn zr_utf8_decoder_reset(storage: *mut ZrUtf8DecoderStorage) -> u8 {
    u8::from(with_decoder_mut(storage, Utf8StreamDecoder::reset).is_some())
}

#[no_mangle]
pub extern "C" fn zr_utf8_decoder_policy(storage: *const ZrUtf8DecoderStorage) -> u32 {
    with_decoder(storage, |decoder| decoder.policy() as u32).unwrap_or(u32::MAX)
}

#[no_mangle]
pub extern "C" fn zr_utf8_decoder_stats(
    storage: *const ZrUtf8DecoderStorage,
    output: *mut ZrUtf8DecodeStats,
) -> u8 {
    if !pointer_aligned(output) {
        return 0;
    }
    let Some(stats) = with_decoder(storage, |decoder| abi_stats(decoder.stats())) else {
        return 0;
    };
    unsafe {
        // SAFETY: The output pointer is non-null, aligned, and writable by contract.
        output.write(stats);
    }
    1
}

#[no_mangle]
pub extern "C" fn zr_utf8_decoder_next_source_offset(storage: *const ZrUtf8DecoderStorage) -> u64 {
    with_decoder(storage, Utf8StreamDecoder::next_source_offset).unwrap_or(0)
}

#[no_mangle]
pub extern "C" fn zr_utf8_decoder_failed(storage: *const ZrUtf8DecoderStorage) -> u8 {
    u8::from(with_decoder(storage, Utf8StreamDecoder::failed).unwrap_or(true))
}

#[cfg(test)]
mod tests {
    use super::*;
    use zevryon_unicode_abi::{
        ZR_UTF8_ERROR_OUTPUT_BUDGET_EXCEEDED, ZR_UTF8_POLICY_REPLACE, ZR_UTF8_POLICY_STRICT,
    };

    #[test]
    fn ffi_lifecycle_and_output_budget_are_fail_closed() {
        let mut storage = ZrUtf8DecoderStorage::default();
        assert_eq!(zr_utf8_decoder_valid(&storage), 0);
        assert_eq!(zr_utf8_decoder_init(&mut storage, ZR_UTF8_POLICY_STRICT), 1);
        assert_eq!(zr_utf8_decoder_valid(&storage), 1);

        let input = [0x41u8, 0xc5, 0x9f];
        let mut output = [ZrDecodedCodePoint::default(); 2];
        let mut written = 0usize;
        let mut error = ZrUtf8DecodeError::default();
        assert_eq!(
            zr_utf8_decoder_feed(
                &mut storage,
                input.as_ptr(),
                input.len(),
                100,
                output.as_mut_ptr(),
                output.len(),
                &mut written,
                &mut error,
            ),
            1
        );
        assert_eq!(written, 2);
        assert_eq!(output[0].value, 0x41);
        assert_eq!(output[1].value, 0x15f);
        assert_eq!(
            zr_utf8_decoder_finish(
                &mut storage,
                output.as_mut_ptr(),
                output.len(),
                &mut written,
                &mut error,
            ),
            1
        );

        assert_eq!(zr_utf8_decoder_reset(&mut storage), 1);
        assert_eq!(zr_utf8_decoder_policy(&storage), ZR_UTF8_POLICY_STRICT);
        assert_eq!(
            zr_utf8_decoder_feed(
                &mut storage,
                input.as_ptr(),
                input.len(),
                0,
                core::ptr::null_mut(),
                0,
                &mut written,
                &mut error,
            ),
            0
        );
        assert_eq!(error.kind, ZR_UTF8_ERROR_OUTPUT_BUDGET_EXCEEDED);
        assert_eq!(zr_utf8_decoder_failed(&storage), 1);

        zr_utf8_decoder_clear(&mut storage);
        assert_eq!(zr_utf8_decoder_valid(&storage), 0);
        assert_eq!(
            zr_utf8_decoder_init(&mut storage, ZR_UTF8_POLICY_REPLACE),
            1
        );
        assert_eq!(zr_utf8_decoder_policy(&storage), ZR_UTF8_POLICY_REPLACE);
    }
}
