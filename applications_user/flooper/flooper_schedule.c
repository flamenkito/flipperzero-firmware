#include "flooper_schedule.h"
#include <math.h>
#include <string.h>

typedef struct {
    FlooperInterval interval;
    uint16_t event;
    uint8_t slice, priority;
} Candidate;

static bool add64(uint64_t a, uint64_t b, uint64_t* result) {
    if(a > UINT64_MAX - b) return false;
    *result = a + b;
    return true;
}

static bool mul64(uint64_t a, uint64_t b, uint64_t* result) {
    if(b && a > UINT64_MAX / b) return false;
    *result = a * b;
    return true;
}

static FlooperScheduleError validate(const FlooperPattern* m) {
    if(!m || !m->pulse_us || !m->count_count || m->count_count > FLOOPER_COUNTS_MAX ||
       !m->layer_count || m->layer_count > FLOOPER_ITEMS_MAX || !m->voice_count ||
       m->voice_count > FLOOPER_ITEMS_MAX || !m->pattern_count ||
       m->pattern_count > FLOOPER_ITEMS_MAX || !m->section_count ||
       m->section_count > FLOOPER_ITEMS_MAX || m->event_count > FLOOPER_EVENTS_MAX ||
       !isfinite(m->master_volume))
        return FlooperScheduleInvalid;
    if(m->start_section >= m->section_count) return FlooperScheduleReference;
    uint64_t cycle;
    if(!mul64(m->pulse_us, m->count_count, &cycle)) return FlooperScheduleOverflow;
    if(m->cycle_us != cycle) return FlooperScheduleInvalid;
    for(unsigned i = 0; i < m->count_count; ++i) {
        if(!m->labels[i] || m->labels[i] > 99) return FlooperScheduleInvalid;
        for(unsigned j = 0; j < i; ++j)
            if(m->labels[i] == m->labels[j]) return FlooperScheduleInvalid;
    }
    for(unsigned i = 0; i < m->voice_count; ++i) {
        const FlooperVoice* v = &m->voices[i];
        if(!v->slice_count || v->slice_count > FLOOPER_SLICES_MAX) return FlooperScheduleInvalid;
        for(unsigned j = 0; j < v->slice_count; ++j) {
            const FlooperSlice* s = &v->slices[j];
            if(!s->duration_us || !isfinite(s->frequency_hz) || s->frequency_hz <= 0 ||
               !isfinite(s->gain))
                return FlooperScheduleInvalid;
        }
    }
    for(unsigned i = 0; i < m->event_count; ++i) {
        const FlooperEvent* e = &m->events[i];
        if(e->count_index >= m->count_count || e->voice >= m->voice_count ||
           e->layer >= m->layer_count)
            return FlooperScheduleReference;
        if(!isfinite(e->velocity)) return FlooperScheduleInvalid;
        const FlooperVoice* v = &m->voices[e->voice];
        for(unsigned j = 0; j < v->slice_count; ++j) {
            uint64_t start, end;
            if(!add64(e->offset_us, v->slices[j].offset_us, &start) ||
               !add64(start, v->slices[j].duration_us, &end) || end > UINT32_MAX)
                return FlooperScheduleOverflow;
            if(end > m->pulse_us) return FlooperScheduleContainment;
        }
    }
    for(unsigned i = 0; i < m->pattern_count; ++i) {
        const FlooperEventPattern* p = &m->patterns[i];
        if(p->event_count > FLOOPER_PATTERN_EVENTS_MAX) return FlooperScheduleInvalid;
        if((uint32_t)p->first_event + p->event_count > m->event_count)
            return FlooperScheduleReference;
    }
    for(unsigned i = 0; i < m->section_count; ++i) {
        const FlooperSection* s = &m->sections[i];
        if(!s->cycles) return FlooperScheduleInvalid;
        if(s->pattern >= m->pattern_count || (s->layer_mask >> m->layer_count))
            return FlooperScheduleReference;
    }
    return FlooperScheduleOk;
}

static bool precedes(const Candidate* a, const Candidate* b) {
    if(a->priority != b->priority) return a->priority > b->priority;
    if(a->interval.volume != b->interval.volume) return a->interval.volume > b->interval.volume;
    if(a->event != b->event) return a->event < b->event;
    return a->slice < b->slice;
}

static FlooperScheduleError
    compile_range(const FlooperPattern* m, FlooperSchedule* out, FlooperCountRange* range) {
    Candidate candidates[FLOOPER_CANDIDATES_MAX];
    unsigned count = 0, expanded = 0;
    const FlooperEventPattern* p = &m->patterns[range->pattern];
    for(unsigned i = p->first_event; i < (unsigned)p->first_event + p->event_count; ++i) {
        const FlooperEvent* e = &m->events[i];
        if(e->count_index != range->count_index || !(range->layer_mask & (1u << e->layer)))
            continue;
        const FlooperVoice* v = &m->voices[e->voice];
        for(unsigned j = 0; j < v->slice_count; ++j) {
            if(++expanded > FLOOPER_CANDIDATES_MAX) return FlooperScheduleCandidateLimit;
            const FlooperSlice* s = &v->slices[j];
            double product = (double)m->master_volume * (double)e->velocity * (double)s->gain;
            float volume = product <= 0 ? 0 : product >= 1 ? 1 : (float)product;
            if(volume == 0) continue;
            uint32_t start = e->offset_us + s->offset_us;
            candidates[count++] = (Candidate){
                {start, start + s->duration_us, s->frequency_hz, volume}, i, j, v->priority};
        }
    }
    range->first_interval = out->interval_count;
    /* Sweep unique endpoints without a sort or a second scratch array. Each
     * half-open span reconsiders every live candidate, allowing resumption. */
    uint32_t at = 0;
    while(at < m->pulse_us) {
        uint32_t next = m->pulse_us;
        const Candidate* winner = NULL;
        for(unsigned i = 0; i < count; ++i) {
            const Candidate* c = &candidates[i];
            if(c->interval.start_us > at && c->interval.start_us < next)
                next = c->interval.start_us;
            if(c->interval.end_us > at && c->interval.end_us < next) next = c->interval.end_us;
            if(c->interval.start_us <= at && at < c->interval.end_us &&
               (!winner || precedes(c, winner)))
                winner = c;
        }
        if(winner) {
            FlooperInterval span = winner->interval;
            span.start_us = at;
            span.end_us = next;
            FlooperInterval* last =
                range->interval_count ? &out->intervals[out->interval_count - 1] : NULL;
            if(last && last->end_us == at && last->frequency_hz == span.frequency_hz &&
               last->volume == span.volume) {
                last->end_us = next;
            } else {
                if(out->interval_count == FLOOPER_INTERVALS_MAX)
                    return FlooperScheduleIntervalLimit;
                out->intervals[out->interval_count++] = span;
                ++range->interval_count;
            }
        }
        at = next;
    }
    return FlooperScheduleOk;
}

FlooperScheduleError flooper_schedule_compile(const FlooperPattern* m, FlooperSchedule* out) {
    if(!out) return FlooperScheduleInvalid;
    memset(out, 0, sizeof(*out));
    FlooperScheduleError error = validate(m);
    if(error != FlooperScheduleOk) return error;
    out->pulse_us = m->pulse_us;
    out->cycle_us = m->cycle_us;
    out->count_count = m->count_count;
    out->section_count = m->section_count;
    out->start_section = m->start_section;
    out->repeat = m->repeat;
    memcpy(out->labels, m->labels, m->count_count);
    for(unsigned i = 0; i < m->section_count; ++i) {
        const FlooperSection* s = &m->sections[i];
        FlooperScheduleSection* dest = &out->sections[i];
        dest->cycles = s->cycles;
        dest->start_us = out->form_us;
        dest->first_count = out->total_counts;
        uint64_t counts;
        if(!mul64(out->cycle_us, s->cycles, &dest->duration_us) ||
           !add64(out->form_us, dest->duration_us, &out->form_us) ||
           !mul64(m->count_count, s->cycles, &counts) ||
           !add64(out->total_counts, counts, &out->total_counts)) {
            error = FlooperScheduleOverflow;
            break;
        }
        unsigned reuse = 0;
        while(reuse < i && (m->sections[reuse].pattern != s->pattern ||
                            m->sections[reuse].layer_mask != s->layer_mask))
            ++reuse;
        if(reuse < i) {
            dest->first_range = out->sections[reuse].first_range;
            continue;
        }
        dest->first_range = out->range_count;
        for(unsigned c = 0; c < m->count_count; ++c) {
            FlooperCountRange* r = &out->ranges[out->range_count++];
            r->pattern = s->pattern;
            r->layer_mask = s->layer_mask;
            r->count_index = c;
            error = compile_range(m, out, r);
            if(error != FlooperScheduleOk) break;
        }
        if(error != FlooperScheduleOk) break;
    }
    if(error != FlooperScheduleOk) memset(out, 0, sizeof(*out));
    return error;
}

FlooperScheduleError flooper_schedule_position(const FlooperSchedule* s, FlooperPosition* p) {
    if(!p) return FlooperScheduleInvalid;
    uint64_t elapsed = p->elapsed_count;
    memset(p, 0, sizeof(*p));
    p->elapsed_count = elapsed;
    if(!s || !s->total_counts || !s->count_count || !s->pulse_us || !s->section_count ||
       s->section_count > FLOOPER_ITEMS_MAX)
        return FlooperScheduleInvalid;
    if(!s->repeat && elapsed >= s->total_counts) return FlooperSchedulePositionEnd;
    uint64_t start, end;
    if(!mul64(elapsed, s->pulse_us, &start) || !add64(start, s->pulse_us, &end))
        return FlooperScheduleOverflow;
    uint64_t local = elapsed % s->total_counts;
    unsigned section = s->section_count - 1;
    while(section && local < s->sections[section].first_count)
        --section;
    uint64_t within = local - s->sections[section].first_count;
    p->form_index = elapsed / s->total_counts;
    p->count_start_us = start;
    p->section_index = section;
    p->cycle_index = within / s->count_count;
    p->count_index = within % s->count_count;
    p->range_index = s->sections[section].first_range + p->count_index;
    p->label = s->labels[p->count_index];
    return FlooperScheduleOk;
}
