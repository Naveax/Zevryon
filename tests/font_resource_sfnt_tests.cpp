#include "font_resource_sfnt.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

namespace {

using namespace zevryon::text;
using Bytes = std::vector<std::byte>;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

void put_u16(Bytes* bytes, std::size_t offset, std::uint16_t value) {
    (*bytes)[offset] = std::byte{static_cast<unsigned char>(value >> 8U)};
    (*bytes)[offset + 1U] = std::byte{static_cast<unsigned char>(value)};
}

void put_u32(Bytes* bytes, std::size_t offset, std::uint32_t value) {
    (*bytes)[offset] = std::byte{static_cast<unsigned char>(value >> 24U)};
    (*bytes)[offset + 1U] = std::byte{static_cast<unsigned char>(value >> 16U)};
    (*bytes)[offset + 2U] = std::byte{static_cast<unsigned char>(value >> 8U)};
    (*bytes)[offset + 3U] = std::byte{static_cast<unsigned char>(value)};
}

void put_record(
    Bytes* bytes,
    std::size_t offset,
    std::uint32_t tag,
    std::uint32_t checksum,
    std::uint32_t table_offset,
    std::uint32_t length) {
    put_u32(bytes, offset, tag);
    put_u32(bytes, offset + 4U, checksum);
    put_u32(bytes, offset + 8U, table_offset);
    put_u32(bytes, offset + 12U, length);
}

Bytes make_single_font(std::uint32_t version = 0x00010000U) {
    Bytes bytes(80U, std::byte{0});
    put_u32(&bytes, 0U, version);
    put_u16(&bytes, 4U, 3U);
    put_u16(&bytes, 6U, 32U);
    put_u16(&bytes, 8U, 1U);
    put_u16(&bytes, 10U, 16U);
    put_record(&bytes, 12U, sfnt_tag('c', 'm', 'a', 'p'), 0x11111111U, 64U, 4U);
    put_record(&bytes, 28U, sfnt_tag('h', 'e', 'a', 'd'), 0x22222222U, 68U, 4U);
    put_record(&bytes, 44U, sfnt_tag('m', 'a', 'x', 'p'), 0x33333333U, 72U, 8U);
    for (std::size_t index = 64U; index < bytes.size(); ++index) {
        bytes[index] = std::byte{static_cast<unsigned char>(index)};
    }
    return bytes;
}

Bytes make_collection_v1() {
    Bytes bytes(160U, std::byte{0});
    put_u32(&bytes, 0U, sfnt_tag('t', 't', 'c', 'f'));
    put_u32(&bytes, 4U, 0x00010000U);
    put_u32(&bytes, 8U, 2U);
    put_u32(&bytes, 12U, 32U);
    put_u32(&bytes, 16U, 80U);

    put_u32(&bytes, 32U, 0x00010000U);
    put_u16(&bytes, 36U, 1U);
    put_u16(&bytes, 38U, 16U);
    put_u16(&bytes, 40U, 0U);
    put_u16(&bytes, 42U, 0U);
    put_record(&bytes, 44U, sfnt_tag('h', 'e', 'a', 'd'), 1U, 144U, 4U);

    put_u32(&bytes, 80U, sfnt_tag('O', 'T', 'T', 'O'));
    put_u16(&bytes, 84U, 1U);
    put_u16(&bytes, 86U, 16U);
    put_u16(&bytes, 88U, 0U);
    put_u16(&bytes, 90U, 0U);
    put_record(&bytes, 92U, sfnt_tag('C', 'F', 'F', ' '), 2U, 152U, 8U);
    return bytes;
}

Bytes make_collection_v2() {
    Bytes bytes(80U, std::byte{0});
    put_u32(&bytes, 0U, sfnt_tag('t', 't', 'c', 'f'));
    put_u32(&bytes, 4U, 0x00020000U);
    put_u32(&bytes, 8U, 1U);
    put_u32(&bytes, 12U, 32U);
    put_u32(&bytes, 16U, 0U);
    put_u32(&bytes, 20U, 0U);
    put_u32(&bytes, 24U, 0U);
    put_u32(&bytes, 32U, 0x00010000U);
    put_u16(&bytes, 36U, 1U);
    put_u16(&bytes, 38U, 16U);
    put_u16(&bytes, 40U, 0U);
    put_u16(&bytes, 42U, 0U);
    put_record(&bytes, 44U, sfnt_tag('h', 'e', 'a', 'd'), 0U, 64U, 4U);
    return bytes;
}

bool valid_single_font_view() {
    const Bytes bytes = make_single_font();
    SfntResourceView view;
    SfntParseStats stats;
    SfntParseError error;
    bool ok = expect(open_sfnt_resource(bytes, 0U, &view, &stats, &error),
                     "valid single-font sfnt must open");
    ok &= expect(view.valid(), "successful parse must publish a valid view");
    ok &= expect(view.container_kind() == SfntContainerKind::SingleFont,
                 "single font must report single container kind");
    ok &= expect(view.flavor() == SfntFlavor::TrueType,
                 "0x00010000 must report TrueType flavor");
    ok &= expect(view.face_count() == 1U && view.face_index() == 0U,
                 "single font must expose one selected face");
    ok &= expect(view.table_count() == 3U && stats.table_count == 3U,
                 "single font must expose three tables");
    ok &= expect(stats.table_payload_bytes == 16U,
                 "payload byte accounting must be exact");
    ok &= expect(stats.search_parameters_match,
                 "canonical search parameters must be recognized");

    SfntTableRecord head;
    ok &= expect(view.find_table(sfnt_tag('h', 'e', 'a', 'd'), &head),
                 "binary table lookup must find head");
    ok &= expect(head.offset == 68U && head.length == 4U,
                 "head record must preserve validated offset and length");
    const std::span<const std::byte> head_data = view.table_data(head);
    ok &= expect(head_data.size() == 4U &&
                     std::to_integer<unsigned char>(head_data.front()) == 68U,
                 "table_data must return the exact validated slice");
    SfntTableRecord missing;
    ok &= expect(!view.find_table(sfnt_tag('n', 'a', 'm', 'e'), &missing),
                 "missing table lookup must fail without mutation");
    ok &= expect(!view.table_record(3U, &missing),
                 "out-of-range table index must fail");
    return ok;
}

bool cff_and_search_parameter_behavior() {
    Bytes bytes = make_single_font(sfnt_tag('O', 'T', 'T', 'O'));
    put_u16(&bytes, 6U, 0U);
    put_u16(&bytes, 8U, 0U);
    put_u16(&bytes, 10U, 0U);
    SfntResourceView view;
    SfntParseStats stats;
    bool ok = expect(open_sfnt_resource(bytes, 0U, &view, &stats, nullptr),
                     "CFF sfnt with stale search hints must still open");
    ok &= expect(view.flavor() == SfntFlavor::Cff,
                 "OTTO signature must report CFF flavor");
    ok &= expect(!stats.search_parameters_match,
                 "parser must report but not trust stale search hints");
    return ok;
}

bool collection_selection_and_versions() {
    const Bytes collection = make_collection_v1();
    SfntResourceView first;
    SfntResourceView second;
    SfntParseStats stats;
    bool ok = expect(open_sfnt_resource(collection, 0U, &first, nullptr, nullptr),
                     "first TTC face must open");
    ok &= expect(open_sfnt_resource(collection, 1U, &second, &stats, nullptr),
                 "second TTC face must open");
    ok &= expect(first.flavor() == SfntFlavor::TrueType &&
                     second.flavor() == SfntFlavor::Cff,
                 "TTC faces must retain independent sfnt flavors");
    ok &= expect(second.container_kind() == SfntContainerKind::Collection &&
                     second.face_count() == 2U && second.face_index() == 1U &&
                     second.face_offset() == 80U,
                 "selected TTC metadata must be exact");
    ok &= expect(stats.selected_face_index == 1U && stats.face_count == 2U,
                 "TTC statistics must identify the selected face");

    SfntParseError error;
    SfntResourceView invalid;
    ok &= expect(!open_sfnt_resource(collection, 2U, &invalid, nullptr, &error),
                 "out-of-range TTC face must fail");
    ok &= expect(error.kind == SfntParseErrorKind::InvalidFaceIndex && !invalid.valid(),
                 "invalid TTC face must publish no view");

    const Bytes version2 = make_collection_v2();
    ok &= expect(open_sfnt_resource(version2, 0U, &invalid, nullptr, &error),
                 "TTC version 2 with empty DSIG must open");

    Bytes invalid_dsig = version2;
    put_u32(&invalid_dsig, 16U, sfnt_tag('B', 'A', 'D', '!'));
    ok &= expect(!open_sfnt_resource(invalid_dsig, 0U, &invalid, nullptr, &error),
                 "invalid TTC DSIG tag must fail");
    ok &= expect(error.kind == SfntParseErrorKind::InvalidCollection,
                 "invalid DSIG must report collection failure");
    return ok;
}

bool truncation_and_atomic_failure() {
    const Bytes complete = make_single_font();
    bool ok = true;
    for (std::size_t size = 0U; size < complete.size(); ++size) {
        SfntResourceView view;
        SfntParseError error;
        const bool result = open_sfnt_resource(
            std::span<const std::byte>(complete.data(), size),
            0U,
            &view,
            nullptr,
            &error);
        ok &= expect(!result && !view.valid(),
                     "every strict prefix before the final table byte must fail");
        if (!ok) {
            return false;
        }
    }

    SfntResourceView reused;
    ok &= expect(open_sfnt_resource(complete, 0U, &reused, nullptr, nullptr),
                 "atomic reset fixture must first build a valid view");
    Bytes unsupported = complete;
    put_u32(&unsupported, 0U, sfnt_tag('t', 'r', 'u', 'e'));
    SfntParseError error;
    ok &= expect(!open_sfnt_resource(unsupported, 0U, &reused, nullptr, &error),
                 "unsupported scaler signature must fail");
    ok &= expect(error.kind == SfntParseErrorKind::UnsupportedSfntVersion &&
                     !reused.valid(),
                 "failed reopen must reset the previously valid output");
    ok &= expect(!open_sfnt_resource(complete, 0U, nullptr, nullptr, &error),
                 "null output pointer must fail");
    ok &= expect(error.kind == SfntParseErrorKind::InvalidArgument,
                 "null output must report invalid argument");
    return ok;
}

bool directory_and_range_rejection() {
    const Bytes original = make_single_font();
    SfntResourceView view;
    SfntParseError error;
    bool ok = true;

    Bytes invalid_count = original;
    put_u16(&invalid_count, 4U, 0U);
    ok &= expect(!open_sfnt_resource(invalid_count, 0U, &view, nullptr, &error) &&
                     error.kind == SfntParseErrorKind::InvalidTableCount,
                 "zero table count must fail");
    put_u16(&invalid_count, 4U, 1025U);
    ok &= expect(!open_sfnt_resource(invalid_count, 0U, &view, nullptr, &error) &&
                     error.kind == SfntParseErrorKind::InvalidTableCount,
                 "table count above the bounded parser limit must fail");

    Bytes invalid_tag = original;
    put_u32(&invalid_tag, 12U, 0x006d6170U);
    ok &= expect(!open_sfnt_resource(invalid_tag, 0U, &view, nullptr, &error) &&
                     error.kind == SfntParseErrorKind::InvalidTableTag,
                 "non-printable table tag must fail");

    Bytes duplicate = original;
    put_u32(&duplicate, 28U, sfnt_tag('c', 'm', 'a', 'p'));
    ok &= expect(!open_sfnt_resource(duplicate, 0U, &view, nullptr, &error) &&
                     error.kind == SfntParseErrorKind::DuplicateTableTag,
                 "duplicate table tag must fail");

    Bytes unsorted = original;
    put_u32(&unsorted, 12U, sfnt_tag('z', 'z', 'z', 'z'));
    ok &= expect(!open_sfnt_resource(unsorted, 0U, &view, nullptr, &error) &&
                     error.kind == SfntParseErrorKind::UnsortedTableDirectory,
                 "descending table tag must fail");

    Bytes escaped = original;
    put_u32(&escaped, 20U, 79U);
    put_u32(&escaped, 24U, 4U);
    ok &= expect(!open_sfnt_resource(escaped, 0U, &view, nullptr, &error) &&
                     error.kind == SfntParseErrorKind::InvalidTableRange,
                 "table range escaping the resource must fail");

    Bytes directory_overlap = original;
    put_u32(&directory_overlap, 20U, 16U);
    put_u32(&directory_overlap, 24U, 4U);
    ok &= expect(!open_sfnt_resource(directory_overlap, 0U, &view, nullptr, &error) &&
                     error.kind == SfntParseErrorKind::DirectoryOverlap,
                 "table overlapping its directory must fail");

    Bytes table_overlap = original;
    put_u32(&table_overlap, 36U, 66U);
    put_u32(&table_overlap, 40U, 4U);
    ok &= expect(!open_sfnt_resource(table_overlap, 0U, &view, nullptr, &error) &&
                     error.kind == SfntParseErrorKind::TableOverlap,
                 "overlapping table payloads must fail");
    return ok;
}

bool collection_header_rejection() {
    Bytes collection = make_collection_v1();
    SfntResourceView view;
    SfntParseError error;
    bool ok = true;

    put_u32(&collection, 4U, 0x00030000U);
    ok &= expect(!open_sfnt_resource(collection, 0U, &view, nullptr, &error) &&
                     error.kind == SfntParseErrorKind::UnsupportedContainerVersion,
                 "unknown TTC version must fail");

    collection = make_collection_v1();
    put_u32(&collection, 8U, 0U);
    ok &= expect(!open_sfnt_resource(collection, 0U, &view, nullptr, &error) &&
                     error.kind == SfntParseErrorKind::InvalidCollection,
                 "empty TTC collection must fail");

    collection = make_collection_v1();
    put_u32(&collection, 12U, 16U);
    ok &= expect(!open_sfnt_resource(collection, 0U, &view, nullptr, &error) &&
                     error.kind == SfntParseErrorKind::InvalidCollection,
                 "TTC face directory inside the collection header must fail");

    Bytes version2 = make_collection_v2();
    put_u32(&version2, 16U, sfnt_tag('D', 'S', 'I', 'G'));
    put_u32(&version2, 20U, 8U);
    put_u32(&version2, 24U, 76U);
    ok &= expect(!open_sfnt_resource(version2, 0U, &view, nullptr, &error) &&
                     error.kind == SfntParseErrorKind::InvalidCollection,
                 "TTC DSIG range escaping the file must fail");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= valid_single_font_view();
    ok &= cff_and_search_parameter_behavior();
    ok &= collection_selection_and_versions();
    ok &= truncation_and_atomic_failure();
    ok &= directory_and_range_rejection();
    ok &= collection_header_rejection();
    if (!ok) {
        return 1;
    }
    std::cout << "font-resource-sfnt-tests: PASS\n";
    return 0;
}
