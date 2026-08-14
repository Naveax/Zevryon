namespace zevryon::massivedoc {

ChunkedOrderStatisticsSequence
ChunkedOrderStatisticsSequence::fork_shared_root() const {
    ChunkedOrderStatisticsSequence forked(impl_->max_records_per_chunk);
    forked.impl_ = impl_;
    forked.stats_ = stats_;
    return forked;
}

} // namespace zevryon::massivedoc
