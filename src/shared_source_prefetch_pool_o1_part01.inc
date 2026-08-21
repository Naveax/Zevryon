            execution->canonical_request = request_value;
            return succeeded;
        }
        return execute_default(
            session,
            request_value,
            execution,
            error);
    }

    void remove_ready_locked(const std::shared_ptr<SessionState>& session) noexcept {
        if (!session->ready.has_value()) {
            return;
        }
        if (session->ready_charge_bytes <= ready_bytes) {
            ready_bytes -= session->ready_charge_bytes;
        } else {
            ready_bytes = 0U;
        }
        if (session->counted && ready_result_count != 0U) {
            --ready_result_count;
        }
        session->ready.reset();
        session->ready_charge_bytes = 0U;
    }

    void invalidate_queue_locked(const std::shared_ptr<SessionState>& session) noexcept {
        if (!session->queued) {
            return;
        }
        if (session->counted && queued_session_count != 0U) {
            --queued_session_count;
        }
        session->queued = false;
        session->queue_epoch = saturating_increment(session->queue_epoch);
    }

    void enqueue_locked(const std::shared_ptr<SessionState>& session) {
        if (session->queued || session->closed || !session->active ||
            !session->pending.has_value()) {
            return;
        }
        session->queue_epoch = saturating_increment(session->queue_epoch);
        session->queued = true;
        if (session->counted) {
            ++queued_session_count;
        }
        queue.push_back(QueueEntry{session, session->queue_epoch});
        if (queue.size() > maximum_queue_depth) {
            maximum_queue_depth = queue.size();
        }
    }

    bool ensure_workers_locked() noexcept {
        if (stop_requested) {
            return false;
        }
        while (workers.size() < config.worker_count) {
            try {
                State* const self = this;
                workers.emplace_back([self] { self->run(); });
                thread_starts = saturating_increment(thread_starts);
            } catch (...) {
                thread_start_failures = saturating_increment(thread_start_failures);
                break;
            }
        }
        return !workers.empty();
    }

    bool idle_locked() const noexcept {
        for (const auto& [id, session] : sessions) {
            static_cast<void>(id);
            if (session->running || session->queued || session->pending.has_value()) {
                return false;
            }
        }
        return true;
    }

    void requeue_after_run_locked(const std::shared_ptr<SessionState>& session) {
        if (!session->closed && session->active && session->pending.has_value()) {
            enqueue_locked(session);
            fairness_requeues = saturating_increment(fairness_requeues);
        }
    }

    void run() {
        for (;;) {
            std::shared_ptr<SessionState> session;
            SourceWindowPrefetchRequest request_value;

            {
                std::unique_lock<std::mutex> lock(mutex);
                work_cv.wait(lock, [this] { return stop_requested || !queue.empty(); });
                if (stop_requested) {
                    idle_cv.notify_all();
                    return;
                }

                while (!queue.empty()) {
                    QueueEntry entry = std::move(queue.front());
                    queue.pop_front();
                    if (!entry.session || entry.session->closed ||
                        !entry.session->queued ||
                        entry.queue_epoch != entry.session->queue_epoch) {
                        continue;
                    }
                    if (entry.session->counted && queued_session_count != 0U) {
                        --queued_session_count;
                    }
                    entry.session->queued = false;
                    if (!entry.session->active || !entry.session->pending.has_value()) {
                        continue;
                    }
                    session = std::move(entry.session);
                    request_value = *session->pending;
                    session->pending.reset();
                    session->running = true;
                    if (session->counted) {
                        ++running_session_count;
                    }
                    runs_started = saturating_increment(runs_started);
                    break;
                }
                if (!session) {
                    idle_cv.notify_all();
                    continue;
                }
            }

            SharedSourcePrefetchExecution execution;
            std::string error;
            bool succeeded = false;
            try {
                succeeded = execute(
                    session,
                    request_value,
                    &execution,
                    &error);
            } catch (const std::exception& exception) {
                error = std::string("shared source prefetch executor threw: ") +
                        exception.what();
            } catch (...) {
                error = "shared source prefetch executor threw";
            }

            if (!canonical_execution_valid(request_value, execution, succeeded)) {
                succeeded = false;
                execution = {};
                execution.canonical_request = request_value;
                error = "shared source prefetch executor returned invalid canonical request";
            } else if (!succeeded && error.empty()) {
                error = "shared source prefetch failed without diagnostic";
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                if (session->running && session->counted &&
                    running_session_count != 0U) {
                    --running_session_count;
                }
                session->running = false;
                if (execution.record_length_resolve_failed) {
                    record_length_resolve_failures =
                        saturating_increment(record_length_resolve_failures);
                }
                if (execution.record_length_learned) {
                    record_length_learns =
                        saturating_increment(record_length_learns);
                }

                if (execution.suppressed) {
                    worker_eof_suppressions =
                        saturating_increment(worker_eof_suppressions);
                    runs_suppressed = saturating_increment(runs_suppressed);
                } else if (succeeded) {
                    runs_succeeded = saturating_increment(runs_succeeded);
                } else {
                    runs_failed = saturating_increment(runs_failed);
                }

                const auto found = sessions.find(session->id);
                const bool still_registered =
                    found != sessions.end() && found->second.get() == session.get();
                if (!still_registered || session->closed) {
                    closed_results_dropped = saturating_increment(closed_results_dropped);
                } else if (!session->active) {
                    inactive_results_dropped = saturating_increment(inactive_results_dropped);
                } else if (request_value.ticket.direction == 0 ||
                           request_value.ticket != session->authority) {
                    stale_results_dropped = saturating_increment(stale_results_dropped);
                } else if (!execution.suppressed) {
                    const SourceWindowPrefetchRequest& canonical =
                        execution.canonical_request;
                    if (!(canonical == request_value)) {
                        canonicalized_results =
                            saturating_increment(canonicalized_results);
                    }
                    const std::size_t charge = execution.bytes.capacity();
