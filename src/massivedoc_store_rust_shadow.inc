#if defined(ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW)
std::vector<std::byte> serialize_record_with_rust_shadow(
    const RecordDescriptor& descriptor) {
    const auto cpp_bytes = serialize_record(descriptor);
#if defined(ZEVRYON_RUST_MASSIVEDOC_CODEC_AUTHORITATIVE)
    const auto rust_bytes = encode_massivedoc_record_authoritative(
        MassiveDocRecordDescriptorValue{
            descriptor.logical_id,
            descriptor.first_chunk,
            descriptor.length,
            descriptor.chunk_count,
            descriptor.crc32},
        cpp_bytes);
    return std::vector<std::byte>(rust_bytes.begin(), rust_bytes.end());
#else
    verify_massivedoc_record_encoding(
        descriptor.logical_id,
        descriptor.first_chunk,
        descriptor.length,
        descriptor.chunk_count,
        descriptor.crc32,
        cpp_bytes);
    return cpp_bytes;
#endif
}

std::optional<RecordDescriptor> parse_record_with_rust_shadow(
    std::span<const std::byte> bytes) {
    const auto cpp_descriptor = parse_record(bytes);
#if defined(ZEVRYON_RUST_MASSIVEDOC_CODEC_AUTHORITATIVE)
    std::optional<MassiveDocRecordDescriptorValue> cpp_reverse_shadow;
    if (cpp_descriptor) {
        cpp_reverse_shadow = MassiveDocRecordDescriptorValue{
            cpp_descriptor->logical_id,
            cpp_descriptor->first_chunk,
            cpp_descriptor->length,
            cpp_descriptor->chunk_count,
            cpp_descriptor->crc32};
    }
    const auto rust_descriptor =
        decode_massivedoc_record_authoritative(bytes, cpp_reverse_shadow);
    if (!rust_descriptor) {
        return std::nullopt;
    }
    return RecordDescriptor{
        rust_descriptor->logical_id,
        rust_descriptor->first_chunk,
        rust_descriptor->length,
        rust_descriptor->chunk_count,
        rust_descriptor->crc32};
#else
    if (cpp_descriptor) {
        verify_massivedoc_record_decoding(
            bytes,
            cpp_descriptor->logical_id,
            cpp_descriptor->first_chunk,
            cpp_descriptor->length,
            cpp_descriptor->chunk_count,
            cpp_descriptor->crc32);
    }
    return cpp_descriptor;
#endif
}

std::vector<std::byte> serialize_chunk_with_rust_shadow(
    const ChunkDescriptor& descriptor) {
    const auto cpp_bytes = serialize_chunk(descriptor);
#if defined(ZEVRYON_RUST_MASSIVEDOC_CODEC_AUTHORITATIVE)
    const auto rust_bytes = encode_massivedoc_chunk_authoritative(
        MassiveDocChunkDescriptorValue{
            descriptor.segment_id, descriptor.offset, descriptor.length},
        cpp_bytes);
    return std::vector<std::byte>(rust_bytes.begin(), rust_bytes.end());
#else
    verify_massivedoc_chunk_encoding(
        descriptor.segment_id, descriptor.offset, descriptor.length, cpp_bytes);
    return cpp_bytes;
#endif
}

std::optional<ChunkDescriptor> parse_chunk_with_rust_shadow(
    std::span<const std::byte> bytes) {
    const auto cpp_descriptor = parse_chunk(bytes);
#if defined(ZEVRYON_RUST_MASSIVEDOC_CODEC_AUTHORITATIVE)
    std::optional<MassiveDocChunkDescriptorValue> cpp_reverse_shadow;
    if (cpp_descriptor) {
        cpp_reverse_shadow = MassiveDocChunkDescriptorValue{
            cpp_descriptor->segment_id,
            cpp_descriptor->offset,
            cpp_descriptor->length};
    }
    const auto rust_descriptor =
        decode_massivedoc_chunk_authoritative(bytes, cpp_reverse_shadow);
    if (!rust_descriptor) {
        return std::nullopt;
    }
    return ChunkDescriptor{
        rust_descriptor->segment_id,
        rust_descriptor->offset,
        rust_descriptor->length};
#else
    if (cpp_descriptor) {
        verify_massivedoc_chunk_decoding(
            bytes,
            cpp_descriptor->segment_id,
            cpp_descriptor->offset,
            cpp_descriptor->length);
    }
    return cpp_descriptor;
#endif
}

#define serialize_record(descriptor) serialize_record_with_rust_shadow(descriptor)
#define parse_record(bytes) parse_record_with_rust_shadow(bytes)
#define serialize_chunk(descriptor) serialize_chunk_with_rust_shadow(descriptor)
#define parse_chunk(bytes) parse_chunk_with_rust_shadow(bytes)
#endif
