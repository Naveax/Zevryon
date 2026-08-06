#if defined(ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW)
std::vector<std::byte> serialize_record_with_rust_shadow(
    const RecordDescriptor& descriptor) {
    auto bytes = serialize_record(descriptor);
    verify_massivedoc_record_encoding(
        descriptor.logical_id,
        descriptor.first_chunk,
        descriptor.length,
        descriptor.chunk_count,
        descriptor.crc32,
        bytes);
    return bytes;
}

std::optional<RecordDescriptor> parse_record_with_rust_shadow(
    std::span<const std::byte> bytes) {
    auto descriptor = parse_record(bytes);
    if (descriptor) {
        verify_massivedoc_record_decoding(
            bytes,
            descriptor->logical_id,
            descriptor->first_chunk,
            descriptor->length,
            descriptor->chunk_count,
            descriptor->crc32);
    }
    return descriptor;
}

std::vector<std::byte> serialize_chunk_with_rust_shadow(
    const ChunkDescriptor& descriptor) {
    auto bytes = serialize_chunk(descriptor);
    verify_massivedoc_chunk_encoding(
        descriptor.segment_id, descriptor.offset, descriptor.length, bytes);
    return bytes;
}

std::optional<ChunkDescriptor> parse_chunk_with_rust_shadow(
    std::span<const std::byte> bytes) {
    auto descriptor = parse_chunk(bytes);
    if (descriptor) {
        verify_massivedoc_chunk_decoding(
            bytes, descriptor->segment_id, descriptor->offset, descriptor->length);
    }
    return descriptor;
}

#define serialize_record(descriptor) serialize_record_with_rust_shadow(descriptor)
#define parse_record(bytes) parse_record_with_rust_shadow(bytes)
#define serialize_chunk(descriptor) serialize_chunk_with_rust_shadow(descriptor)
#define parse_chunk(bytes) parse_chunk_with_rust_shadow(bytes)
#endif
