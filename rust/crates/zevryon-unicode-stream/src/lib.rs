#![forbid(unsafe_code)]
#![deny(warnings)]

const MAGIC: u64 = 0x5a52_5554_4638_0001;
const REPLACEMENT_CHARACTER: u32 = 0xfffd;

#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ErrorPolicy {
    Strict = 0,
    Replace = 1,
}

impl ErrorPolicy {
    pub fn from_abi(value: u32) -> Option<Self> {
        match value {
            0 => Some(Self::Strict),
            1 => Some(Self::Replace),
            _ => None,
        }
    }
}

#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ErrorKind {
    None = 0,
    DiscontinuousInput = 1,
    InvalidLeadByte = 2,
    UnexpectedContinuation = 3,
    InvalidContinuation = 4,
    OverlongEncoding = 5,
    SurrogateCodePoint = 6,
    CodePointOutOfRange = 7,
    TruncatedSequence = 8,
    OutputBudgetExceeded = 9,
}

#[repr(u32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ErrorDetail {
    None = 0,
    DecoderFailed = 1,
    DecoderFinished = 2,
    DiscontinuousOffset = 3,
    SourceRangeOverflow = 4,
    OutputCapacity = 5,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct DecodeStats {
    pub source_bytes: u64,
    pub emitted_codepoints: u64,
    pub invalid_sequences: u64,
    pub replacements: u64,
    pub chunks: u64,
    pub maximum_pending_continuations: u8,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DecodedCodePoint {
    pub source_start: u64,
    pub value: u32,
    pub source_length: u8,
    pub replacement: bool,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DecodeError {
    pub kind: ErrorKind,
    pub detail: ErrorDetail,
    pub source_offset: u64,
}

#[derive(Clone, Copy)]
pub struct Utf8StreamDecoder {
    magic: u64,
    policy: ErrorPolicy,
    stats: DecodeStats,
    started: bool,
    finished: bool,
    failed: bool,
    next_source_offset: u64,
    sequence_start: u64,
    accumulator: u32,
    minimum_value: u32,
    pending_continuations: u8,
}

impl Utf8StreamDecoder {
    pub fn new(policy: ErrorPolicy) -> Self {
        Self {
            magic: MAGIC,
            policy,
            stats: DecodeStats::default(),
            started: false,
            finished: false,
            failed: false,
            next_source_offset: 0,
            sequence_start: 0,
            accumulator: 0,
            minimum_value: 0,
            pending_continuations: 0,
        }
    }

    pub fn is_initialized(&self) -> bool {
        self.magic == MAGIC
    }

    pub fn policy(&self) -> ErrorPolicy {
        self.policy
    }

    pub fn stats(&self) -> DecodeStats {
        self.stats
    }

    pub fn next_source_offset(&self) -> u64 {
        self.next_source_offset
    }

    pub fn failed(&self) -> bool {
        self.failed
    }

    pub fn reset(&mut self) {
        let policy = self.policy;
        *self = Self::new(policy);
    }

    pub fn feed<F>(
        &mut self,
        bytes: &[u8],
        absolute_source_offset: u64,
        mut emit: F,
    ) -> Result<(), DecodeError>
    where
        F: FnMut(DecodedCodePoint) -> Result<(), ()>,
    {
        if self.failed {
            return self.fail(
                ErrorKind::DiscontinuousInput,
                ErrorDetail::DecoderFailed,
                absolute_source_offset,
            );
        }
        if self.finished {
            return self.fail(
                ErrorKind::DiscontinuousInput,
                ErrorDetail::DecoderFinished,
                absolute_source_offset,
            );
        }
        if self.started && absolute_source_offset != self.next_source_offset {
            return self.fail(
                ErrorKind::DiscontinuousInput,
                ErrorDetail::DiscontinuousOffset,
                absolute_source_offset,
            );
        }

        let byte_count = match u64::try_from(bytes.len()) {
            Ok(value) => value,
            Err(_) => {
                return self.fail(
                    ErrorKind::DiscontinuousInput,
                    ErrorDetail::SourceRangeOverflow,
                    absolute_source_offset,
                );
            }
        };
        let Some(next_source_offset) = absolute_source_offset.checked_add(byte_count) else {
            return self.fail(
                ErrorKind::DiscontinuousInput,
                ErrorDetail::SourceRangeOverflow,
                absolute_source_offset,
            );
        };

        if !self.started {
            self.started = true;
            self.next_source_offset = absolute_source_offset;
        }
        self.stats.chunks = self.stats.chunks.wrapping_add(1);
        self.stats.source_bytes = self.stats.source_bytes.saturating_add(byte_count);

        for (index, byte) in bytes.iter().copied().enumerate() {
            let source_offset =
                absolute_source_offset + u64::try_from(index).expect("slice index fits in u64");
            let mut retry = true;
            while retry {
                retry = false;
                if self.pending_continuations != 0 {
                    if byte & 0xc0 == 0x80 {
                        self.accumulator = (self.accumulator << 6) | u32::from(byte & 0x3f);
                        self.pending_continuations -= 1;
                        if self.pending_continuations == 0 {
                            let source_end = source_offset + 1;
                            if self.accumulator < self.minimum_value {
                                self.handle_invalid_sequence(
                                    ErrorKind::OverlongEncoding,
                                    self.sequence_start,
                                    source_end,
                                    &mut emit,
                                )?;
                            } else if (0xd800..=0xdfff).contains(&self.accumulator) {
                                self.handle_invalid_sequence(
                                    ErrorKind::SurrogateCodePoint,
                                    self.sequence_start,
                                    source_end,
                                    &mut emit,
                                )?;
                            } else if self.accumulator > 0x10ffff {
                                self.handle_invalid_sequence(
                                    ErrorKind::CodePointOutOfRange,
                                    self.sequence_start,
                                    source_end,
                                    &mut emit,
                                )?;
                            } else {
                                let value = self.accumulator;
                                let source_start = self.sequence_start;
                                self.clear_sequence();
                                self.emit(value, source_start, source_end, false, &mut emit)?;
                            }
                        }
                        continue;
                    }

                    self.handle_invalid_sequence(
                        ErrorKind::InvalidContinuation,
                        source_offset,
                        source_offset,
                        &mut emit,
                    )?;
                    retry = true;
                    continue;
                }

                match byte {
                    0x00..=0x7f => {
                        self.emit(
                            u32::from(byte),
                            source_offset,
                            source_offset + 1,
                            false,
                            &mut emit,
                        )?;
                    }
                    0xc2..=0xdf => {
                        self.start_sequence(1, u32::from(byte & 0x1f), 0x80, source_offset);
                    }
                    0xe0..=0xef => {
                        self.start_sequence(2, u32::from(byte & 0x0f), 0x800, source_offset);
                    }
                    0xf0..=0xf4 => {
                        self.start_sequence(3, u32::from(byte & 0x07), 0x10000, source_offset);
                    }
                    _ => {
                        self.sequence_start = source_offset;
                        let kind = if byte & 0xc0 == 0x80 {
                            ErrorKind::UnexpectedContinuation
                        } else {
                            ErrorKind::InvalidLeadByte
                        };
                        self.handle_invalid_sequence(
                            kind,
                            source_offset,
                            source_offset + 1,
                            &mut emit,
                        )?;
                    }
                }
            }
        }

        self.next_source_offset = next_source_offset;
        Ok(())
    }

    pub fn finish<F>(&mut self, mut emit: F) -> Result<(), DecodeError>
    where
        F: FnMut(DecodedCodePoint) -> Result<(), ()>,
    {
        if self.failed {
            return self.fail(
                ErrorKind::DiscontinuousInput,
                ErrorDetail::DecoderFailed,
                self.next_source_offset,
            );
        }
        if self.finished {
            return Ok(());
        }
        if self.pending_continuations != 0 {
            self.handle_invalid_sequence(
                ErrorKind::TruncatedSequence,
                self.next_source_offset,
                self.next_source_offset,
                &mut emit,
            )?;
        }
        self.finished = true;
        Ok(())
    }

    fn emit<F>(
        &mut self,
        value: u32,
        source_start: u64,
        source_end: u64,
        replacement: bool,
        emit: &mut F,
    ) -> Result<(), DecodeError>
    where
        F: FnMut(DecodedCodePoint) -> Result<(), ()>,
    {
        let source_length = source_end
            .checked_sub(source_start)
            .filter(|length| *length <= u64::from(u8::MAX))
            .and_then(|length| u8::try_from(length).ok())
            .unwrap_or(0);
        let point = DecodedCodePoint {
            source_start,
            value,
            source_length,
            replacement,
        };
        if emit(point).is_err() {
            return self.fail(
                ErrorKind::OutputBudgetExceeded,
                ErrorDetail::OutputCapacity,
                source_start,
            );
        }
        self.stats.emitted_codepoints = self.stats.emitted_codepoints.wrapping_add(1);
        if replacement {
            self.stats.replacements = self.stats.replacements.wrapping_add(1);
        }
        Ok(())
    }

    fn fail(
        &mut self,
        kind: ErrorKind,
        detail: ErrorDetail,
        source_offset: u64,
    ) -> Result<(), DecodeError> {
        self.failed = true;
        Err(DecodeError {
            kind,
            detail,
            source_offset,
        })
    }

    fn handle_invalid_sequence<F>(
        &mut self,
        kind: ErrorKind,
        error_offset: u64,
        replacement_end: u64,
        emit: &mut F,
    ) -> Result<(), DecodeError>
    where
        F: FnMut(DecodedCodePoint) -> Result<(), ()>,
    {
        self.stats.invalid_sequences = self.stats.invalid_sequences.wrapping_add(1);
        if self.policy == ErrorPolicy::Strict {
            return self.fail(kind, ErrorDetail::None, error_offset);
        }

        let replacement_start = self.sequence_start;
        self.clear_sequence();
        self.emit(
            REPLACEMENT_CHARACTER,
            replacement_start,
            replacement_end,
            true,
            emit,
        )
    }

    fn start_sequence(
        &mut self,
        expected_continuations: u8,
        accumulator: u32,
        minimum_value: u32,
        source_start: u64,
    ) {
        self.pending_continuations = expected_continuations;
        self.accumulator = accumulator;
        self.minimum_value = minimum_value;
        self.sequence_start = source_start;
        self.stats.maximum_pending_continuations = self
            .stats
            .maximum_pending_continuations
            .max(self.pending_continuations);
    }

    fn clear_sequence(&mut self) {
        self.sequence_start = 0;
        self.accumulator = 0;
        self.minimum_value = 0;
        self.pending_continuations = 0;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn collect(
        decoder: &mut Utf8StreamDecoder,
        bytes: &[u8],
        offset: u64,
    ) -> Result<Vec<DecodedCodePoint>, DecodeError> {
        let mut output = Vec::new();
        decoder.feed(bytes, offset, |point| {
            output.push(point);
            Ok(())
        })?;
        Ok(output)
    }

    #[test]
    fn valid_multibyte_input_is_chunk_invariant() {
        let input = [
            0x41, 0xc5, 0x9f, 0x65, 0xcc, 0x81, 0xf0, 0x9f, 0x98, 0x80, 0x0a, 0xe4, 0xb8, 0xad,
        ];
        let mut one_shot = Utf8StreamDecoder::new(ErrorPolicy::Strict);
        let reference = collect(&mut one_shot, &input, 1000).expect("valid UTF-8");
        one_shot.finish(|_| Ok(())).expect("finish");

        for chunk_size in 1..=input.len() {
            let mut decoder = Utf8StreamDecoder::new(ErrorPolicy::Strict);
            let mut output = Vec::new();
            let mut consumed = 0;
            while consumed < input.len() {
                let end = (consumed + chunk_size).min(input.len());
                decoder
                    .feed(&input[consumed..end], 1000 + consumed as u64, |point| {
                        output.push(point);
                        Ok(())
                    })
                    .expect("chunk");
                consumed = end;
            }
            decoder
                .finish(|point| {
                    output.push(point);
                    Ok(())
                })
                .expect("finish");
            assert_eq!(output, reference);
        }
    }

    #[test]
    fn strict_errors_match_the_cpp_contract() {
        let cases: &[(&[u8], u64, ErrorKind, u64)] = &[
            (&[0xe2, 0x28, 0xa1], 0, ErrorKind::InvalidContinuation, 1),
            (&[0xed, 0xa0, 0x80], 50, ErrorKind::SurrogateCodePoint, 50),
            (
                &[0xf4, 0x90, 0x80, 0x80],
                100,
                ErrorKind::CodePointOutOfRange,
                100,
            ),
            (&[0xe0, 0x80, 0x80], 0, ErrorKind::OverlongEncoding, 0),
            (&[0x80], 500, ErrorKind::UnexpectedContinuation, 500),
            (&[0xff], 700, ErrorKind::InvalidLeadByte, 700),
        ];
        for (input, base, kind, offset) in cases {
            let mut decoder = Utf8StreamDecoder::new(ErrorPolicy::Strict);
            let error = decoder
                .feed(input, *base, |_| Ok(()))
                .expect_err("must reject");
            assert_eq!(error.kind, *kind);
            assert_eq!(error.detail, ErrorDetail::None);
            assert_eq!(error.source_offset, *offset);
            assert!(decoder.failed());
        }
    }

    #[test]
    fn replacement_policy_retries_non_continuation_bytes() {
        let mut decoder = Utf8StreamDecoder::new(ErrorPolicy::Replace);
        let mut output = Vec::new();
        for (index, byte) in [0xe2, 0x28, 0xa1, 0x41].into_iter().enumerate() {
            decoder
                .feed(&[byte], 200 + index as u64, |point| {
                    output.push(point);
                    Ok(())
                })
                .expect("replacement feed");
        }
        decoder
            .finish(|point| {
                output.push(point);
                Ok(())
            })
            .expect("finish");

        assert_eq!(output.len(), 4);
        assert!(output[0].replacement);
        assert_eq!(output[0].source_start, 200);
        assert_eq!(output[0].source_length, 1);
        assert_eq!(output[1].value, 0x28);
        assert!(output[2].replacement);
        assert_eq!(output[3].value, 0x41);
    }

    #[test]
    fn truncated_sequence_is_replaced_once() {
        let mut decoder = Utf8StreamDecoder::new(ErrorPolicy::Replace);
        let mut output = Vec::new();
        decoder
            .feed(&[0xf0, 0x9f], 300, |point| {
                output.push(point);
                Ok(())
            })
            .expect("feed");
        decoder
            .finish(|point| {
                output.push(point);
                Ok(())
            })
            .expect("finish");
        assert_eq!(output.len(), 1);
        assert_eq!(output[0].source_start, 300);
        assert_eq!(output[0].source_length, 2);
        assert!(output[0].replacement);
    }

    #[test]
    fn output_failure_is_fail_closed() {
        let mut decoder = Utf8StreamDecoder::new(ErrorPolicy::Strict);
        let error = decoder
            .feed(&[0x41], 0, |_| Err(()))
            .expect_err("output budget must fail");
        assert_eq!(error.kind, ErrorKind::OutputBudgetExceeded);
        assert_eq!(error.detail, ErrorDetail::OutputCapacity);
        assert_eq!(error.source_offset, 0);
        assert!(decoder.failed());
        assert_eq!(decoder.stats().emitted_codepoints, 0);
    }

    #[test]
    fn discontinuity_and_reset_follow_lifecycle_contract() {
        let mut decoder = Utf8StreamDecoder::new(ErrorPolicy::Strict);
        decoder.feed(&[0x41], 10, |_| Ok(())).expect("first");
        let discontinuous = decoder
            .feed(&[0x42], 12, |_| Ok(()))
            .expect_err("discontinuous");
        assert_eq!(discontinuous.kind, ErrorKind::DiscontinuousInput);
        assert_eq!(discontinuous.detail, ErrorDetail::DiscontinuousOffset);

        let failed = decoder
            .feed(&[0x42], 11, |_| Ok(()))
            .expect_err("failed decoder");
        assert_eq!(failed.kind, ErrorKind::DiscontinuousInput);
        assert_eq!(failed.detail, ErrorDetail::DecoderFailed);

        decoder.reset();
        assert!(!decoder.failed());
        assert_eq!(decoder.next_source_offset(), 0);
        decoder.feed(&[0x41], 0, |_| Ok(())).expect("after reset");
        decoder.finish(|_| Ok(())).expect("finish");
        decoder.finish(|_| Ok(())).expect("idempotent finish");
        let finished = decoder
            .feed(&[0x42], 1, |_| Ok(()))
            .expect_err("finished decoder");
        assert_eq!(finished.kind, ErrorKind::DiscontinuousInput);
        assert_eq!(finished.detail, ErrorDetail::DecoderFinished);
    }

    #[test]
    fn source_range_overflow_has_stable_detail() {
        let mut decoder = Utf8StreamDecoder::new(ErrorPolicy::Strict);
        let error = decoder
            .feed(&[0x41], u64::MAX, |_| Ok(()))
            .expect_err("source range overflow");
        assert_eq!(error.kind, ErrorKind::DiscontinuousInput);
        assert_eq!(error.detail, ErrorDetail::SourceRangeOverflow);
        assert_eq!(error.source_offset, u64::MAX);
    }
}
