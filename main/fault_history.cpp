#include "fault_history.h"

#include <stdlib.h>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

namespace {

constexpr size_t kMaxOpen = 16;

struct OpenSpan {
    uint16_t site;
    uint32_t start;
};

struct Builder {
    uint32_t window_start;
    uint32_t window_end;
    fault_interval_t* out;
    size_t cap;
    size_t count = 0;
    size_t total = 0;

    void emit(uint16_t site, uint32_t start, uint32_t end) {
        if (start > window_end) return;
        if (end != 0 && end < window_start) return;
        ++total;
        if (count < cap) {
            out[count++] = {start, end, site};
            return;
        }
        // Full: the newest faults matter most, so evict the oldest.
        size_t oldest = 0;
        for (size_t i = 1; i < count; ++i) {
            if (out[i].start < out[oldest].start) oldest = i;
        }
        if (start > out[oldest].start) out[oldest] = {start, end, site};
    }
};

enum class SpanKind { Fault, Defrost };

size_t pair_spans(SpanKind kind, const event_entry_t* events, size_t n,
                  uint32_t window_start, uint32_t window_end,
                  uint32_t current_boot_id, fault_interval_t* out, size_t cap,
                  size_t* total) {
    const event_type_t open_type =
        kind == SpanKind::Fault ? EVENT_ERROR_APPEARED : EVENT_DEFROST_START;
    const event_type_t close_type =
        kind == SpanKind::Fault ? EVENT_ERROR_CLEARED : EVENT_DEFROST_END;
    if (total) *total = 0;
    if (!out || cap == 0) return 0;
    Builder b{window_start, window_end, out, cap};
    OpenSpan open[kMaxOpen];
    size_t open_n = 0;
    uint32_t boot = 0;
    uint32_t last_ts = 0;
    bool have_boot = false;

    auto close_all = [&](uint32_t end) {
        for (size_t i = 0; i < open_n; ++i) {
            b.emit(open[i].site, open[i].start,
                   end < open[i].start ? open[i].start : end);
        }
        open_n = 0;
    };

    for (size_t i = 0; i < n; ++i) {
        const event_entry_t& e = events[i];
        if (e.timestamp == 0) continue;
        if (have_boot && e.boot_id != boot) close_all(last_ts);
        boot = e.boot_id;
        have_boot = true;
        last_ts = e.timestamp;

        const uint16_t site = kind == SpanKind::Fault ? (uint16_t)e.payload : 0;
        if (e.type == open_type) {
            bool already = false;
            for (size_t j = 0; j < open_n; ++j) {
                if (open[j].site == site) { already = true; break; }
            }
            if (!already && open_n < kMaxOpen) open[open_n++] = {site, e.timestamp};
        } else if (e.type == close_type) {
            for (size_t j = 0; j < open_n; ++j) {
                if (open[j].site != site) continue;
                b.emit(site, open[j].start, e.timestamp);
                open[j] = open[--open_n];
                break;
            }
        }
    }

    if (have_boot && boot == current_boot_id) {
        for (size_t i = 0; i < open_n; ++i) b.emit(open[i].site, open[i].start, 0);
        open_n = 0;
    } else {
        close_all(last_ts);
    }

    // Spans are emitted as they close; charts want them by start time.
    for (size_t i = 1; i < b.count; ++i) {
        fault_interval_t v = out[i];
        size_t j = i;
        while (j > 0 && out[j - 1].start > v.start) { out[j] = out[j - 1]; --j; }
        out[j] = v;
    }
    if (total) *total = b.total;
    return b.count;
}

}  // namespace

size_t fault_intervals_from_events(const event_entry_t* events, size_t n,
                                   uint32_t window_start, uint32_t window_end,
                                   uint32_t current_boot_id,
                                   fault_interval_t* out, size_t cap,
                                   size_t* total) {
    return pair_spans(SpanKind::Fault, events, n, window_start, window_end,
                      current_boot_id, out, cap, total);
}

size_t defrost_intervals_from_events(const event_entry_t* events, size_t n,
                                     uint32_t window_start, uint32_t window_end,
                                     uint32_t current_boot_id,
                                     fault_interval_t* out, size_t cap) {
    return pair_spans(SpanKind::Defrost, events, n, window_start, window_end,
                      current_boot_id, out, cap, nullptr);
}

size_t fault_history_query(uint32_t window_start, uint32_t window_end,
                           fault_interval_t* out, size_t cap, size_t* total) {
    return chart_spans_query(window_start, window_end, out, cap, total, nullptr,
                             0, nullptr);
}

size_t chart_spans_query(uint32_t window_start, uint32_t window_end,
                         fault_interval_t* faults, size_t fault_cap,
                         size_t* fault_total, fault_interval_t* defrosts,
                         size_t defrost_cap, size_t* defrost_count) {
    if (fault_total) *fault_total = 0;
    if (defrost_count) *defrost_count = 0;
    const size_t bytes = sizeof(event_entry_t) * EVENT_LOG_MAX_ENTRIES;
#ifdef ESP_PLATFORM
    event_entry_t* events = (event_entry_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!events) events = (event_entry_t*)malloc(bytes);
#else
    event_entry_t* events = (event_entry_t*)malloc(bytes);
#endif
    if (!events) return 0;

    const int got = event_log_get(events, EVENT_LOG_MAX_ENTRIES, 0);
    // event_log_get is newest first; the pairing walks forward in time.
    for (int i = 0, j = got - 1; i < j; ++i, --j) {
        event_entry_t t = events[i];
        events[i] = events[j];
        events[j] = t;
    }
    const size_t n = got > 0 ? (size_t)got : 0;
    const uint32_t boot = event_log_current_boot_id();
    const size_t count = fault_intervals_from_events(
        events, n, window_start, window_end, boot, faults, fault_cap, fault_total);
    if (defrosts && defrost_count) {
        *defrost_count = defrost_intervals_from_events(
            events, n, window_start, window_end, boot, defrosts, defrost_cap);
    }
    free(events);
    return count;
}
