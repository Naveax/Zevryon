# Z7 bounded large-document parser smoke

## Purpose

This slice adds a deterministic Windows/Linux CTest smoke for the Z7 `bounded_large_document_parse` direction. It verifies that the strict streaming HTML v2 parser can preserve one multi-record browser text-node source span whose payload is much larger than the parser's charged resident working-set limit.

It is implementation authority only. An 8 MiB CI fixture is not final large-document certification and does not mark Z7 implemented.

## Fixture

The smoke builds a real authoritative native store containing one `<style>` RAWTEXT element:

- 128 physical records;
- 64 KiB RAWTEXT payload per record;
- 8 MiB logical RAWTEXT payload in total;
- `<style>` in the first record;
- `</style>` in the final record;
- exactly three logical browser nodes: `#document`, `style`, `#text`.

The parser uses:

- a 4 KiB StoreReader input window;
- a 256 KiB parser working-set hard limit.

The 8 MiB payload is therefore 32 times larger than the configured parser ledger cap. Passing cannot depend on buffering the full text node in parser-owned memory.

## Required properties

The smoke requires:

- exactly one element and one text node in addition to `#document`;
- the RAWTEXT payload to remain one logical cross-record text span rather than being split by physical storage boundaries;
- exact parser source-record and source-byte accounting;
- parser working-set peak at or below the 256 KiB hard limit;
- parser working-set peak below one sixteenth of the 8 MiB payload;
- zero charged working set after parser destruction;
- zero ResourceLedger accounting errors;
- exact `style` start-tag source identity;
- an exact 8 MiB `#text` source span beginning immediately after `<style>` in record 0 and continuing through all physical records;
- authoritative `ZVNSRC01` v2 validation against the native store;
- validator streamed-node bytes equal to the start-tag bytes plus the 8 MiB text span, without incorrectly including the closing tag.

## Admission boundary

This smoke materially strengthens the implementation side of `bounded_large_document_parse`, but it is not the final configured Z7 gate. Remaining certification work includes a frozen certification envelope, candidate/source provenance, larger and more varied documents, malformed/failure cases, timing/resource receipts and admitted evidence produced by the final certification procedure.

Tokenizer/tree-builder conformance, parser fuzzing, script-data, PLAINTEXT, full HTML input preprocessing and other outstanding Z7 work remain separate. Z7 therefore stays `planned`.