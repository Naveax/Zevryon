from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"


def test_io_window_is_bounded_to_64_kib() -> None:
    header = (SRC / "massivedoc_store.hpp").read_text(encoding="utf-8")
    assert "constexpr std::size_t kIoWindowBytes = 64U * 1024U;" in header


def test_io_windows_are_not_automatic_arrays() -> None:
    combined = "\n".join(
        path.read_text(encoding="utf-8")
        for path in sorted(SRC.glob("massivedoc_store_part*.inc"))
    )
    assert "std::array<std::byte, kIoWindowBytes>" not in combined
    assert combined.count("std::vector<std::byte> buffer(kIoWindowBytes);") >= 3
    assert "std::vector<std::byte> io_buffer(kIoWindowBytes);" in combined


def test_corpus_import_streams_directly_into_writer_window() -> None:
    source = (SRC / "massivedoc_store_part05.inc").read_text(encoding="utf-8")
    assert "[&corpus, &remaining](std::span<std::byte> destination)" in source
    assert "std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(destination.size()))" in source
