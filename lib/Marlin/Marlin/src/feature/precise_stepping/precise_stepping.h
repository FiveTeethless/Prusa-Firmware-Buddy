/**
 * Based on the implementation in Klipper [https://github.com/Klipper3d/klipper].
 * Copyright (C) Kevin O'Connor <kevin@koconnor.net>
 *
 * Our implementation takes inspiration from the work of Kevin O'Connor <kevin@koconnor.net> for Klipper
 * in used data structures, and some computations.
 */
#pragma once
#include "common.h"
#include "bsod.h"

// Next steps are generated only if number of free slots in event buffer queue is bigger that this value.
constexpr const uint8_t MIN_STEP_EVENT_FREE_SLOT = 0;

// Minimum number of free slots in the move segment queue must be available in the queue in all circumstances.
// 1 free slot is required to ensure that we can add the empty ending move anytime.
constexpr const uint8_t MOVE_SEGMENT_QUEUE_MIN_FREE_SLOTS = 1;

// Maximum number of step events produced in on move interrupt to limit time spent by move interrupt handler
// when the step event queue is empty.
constexpr const uint16_t MAX_STEP_EVENTS_PRODUCED_PER_ONE_CALL = 256;

struct move_t;
struct step_generator_state_t;

class PreciseStepping {

public:
    static step_event_queue_t step_event_queue;
    static move_segment_queue_t move_segment_queue;
    static step_generator_state_t step_generator_state;

    // Preallocated collection of all step event generators for all axis and all generator types (classic, input shaper, pressure advance).
    static step_generators_pool_t step_generators_pool;
    // Indicate which type of step event generator is enabled on which axis.
    static uint8_t step_generator_types;

    // Total number of ticks until the next step event will be processed.
    // Or number of ticks to next call of stepper ISR when step event queue is empty.
    static uint32_t left_ticks_to_next_step_event;

    // Precomputed period of calling PreciseStepping::isr() when there is no queued step event.
    static uint32_t stepper_isr_period_in_ticks;
    // Precomputed conversion rate from seconds to timer ticks.
    static float ticks_per_sec;

    // Indicate which direction bits are inverted.
    static uint16_t inverted_dirs;

    static double global_print_time;
    static xyze_double_t global_start_pos;

    PreciseStepping() = default;

    static void init();

    // Reset the motion/stepper generator state from halt
    static void reset_from_halt();

    // The ISR scheduler
    static void isr();

    static void process_queue_of_blocks();

    static void process_queue_of_move_segments();

    // Generate step pulses for the stepper motors.
    // Returns time to the next step event or ISR call.
    static uint32_t process_one_step_event_from_queue();

    // Returns the index of the next move segment in the queue.
    static constexpr uint8_t move_segment_queue_next_index(const uint8_t move_segment_index) { return MOVE_SEGMENT_QUEUE_MOD(move_segment_index + 1); }

    // Returns the index of the previous move segment in the queue.
    static constexpr uint8_t move_segment_queue_prev_index(const uint8_t move_segment_index) { return MOVE_SEGMENT_QUEUE_MOD(move_segment_index - 1); }

    // Remove all move segments from the queue.
    FORCE_INLINE static void move_segment_queue_clear() { move_segment_queue.head = move_segment_queue.tail = move_segment_queue.unprocessed = 0; }

    // Check if the queue has any move segments queued.
    FORCE_INLINE static bool has_move_segments_queued() { return (move_segment_queue.head != move_segment_queue.tail); }

    // Check if the queue has any unprocessed move segments queued.
    FORCE_INLINE static bool has_unprocessed_move_segments_queued() { return (move_segment_queue.head != move_segment_queue.unprocessed); }

    // Check if the queue of move segment is full.
    FORCE_INLINE static bool is_move_segment_queue_full() { return move_segment_queue.tail == move_segment_queue_next_index(move_segment_queue.head); }

    // Number of move segments in the queue.
    FORCE_INLINE static uint8_t move_segment_queue_size() { return MOVE_SEGMENT_QUEUE_MOD(move_segment_queue.head - move_segment_queue.tail); }

    // Returns number of free slots in the move segment queue.
    FORCE_INLINE static uint8_t move_segment_queue_free_slots() { return MOVE_SEGMENT_QUEUE_SIZE - 1 - move_segment_queue_size(); }

    // Returns the current move segment, nullptr if the queue is empty.
    FORCE_INLINE static move_t *get_current_move_segment() {
        if (has_move_segments_queued())
            return &move_segment_queue.data[move_segment_queue.tail];

        return nullptr;
    }

    // Returns the current move segment that isn't processed by PreciseStepping::process_queue_of_move_segments(), nullptr if the queue is empty.
    FORCE_INLINE static move_t *get_current_unprocessed_move_segment() {
        if (has_unprocessed_move_segments_queued())
            return &move_segment_queue.data[move_segment_queue.unprocessed];

        return nullptr;
    }

    // Returns the last move segment inside the queue (at the bottom of the queue), nullptr if the queue is empty.
    FORCE_INLINE static move_t *get_last_move_segment() {
        if (has_move_segments_queued())
            return &move_segment_queue.data[move_segment_queue_prev_index(move_segment_queue.head)];

        return nullptr;
    }

    // Returns the first head move segment, nullptr if the queue is full.
    // Also, it returns the next move segment queue head index (passed by reference).
    FORCE_INLINE static move_t *get_next_free_move_segment(uint8_t &next_move_segment_queue_head) {
        if (is_move_segment_queue_full())
            return nullptr;

        // Return the first available move segment.
        next_move_segment_queue_head = move_segment_queue_next_index(move_segment_queue.head);
        return &move_segment_queue.data[move_segment_queue.head];
    }

    // Discard the current move segment.
    // Caller must ensure that there is something to discard.
    FORCE_INLINE static void discard_current_move_segment() {
        assert(has_move_segments_queued());
        move_segment_queue.tail = move_segment_queue_next_index(move_segment_queue.tail);
    }

    // Discard the current unprocessed move segment.
    // Caller must ensure that there is something to discard.
    FORCE_INLINE static void discard_current_unprocessed_move_segment() {
        assert(has_unprocessed_move_segments_queued());
        move_segment_queue.unprocessed = move_segment_queue_next_index(move_segment_queue.unprocessed);
    }

    // Returns the index of the next step event in the queue.
    static constexpr uint16_t step_event_queue_next_index(const uint16_t step_event_index) { return STEP_EVENT_QUEUE_MOD(step_event_index + 1); }

    // Returns the index of the previous step event in the queue.
    static constexpr uint16_t step_event_queue_prev_index(const uint16_t step_event_index) { return STEP_EVENT_QUEUE_MOD(step_event_index - 1); }

    // Remove all step events from the queue.
    FORCE_INLINE static void step_event_queue_clear() { step_event_queue.head = step_event_queue.tail = 0; }

    // Check if the queue have any step events queued.
    FORCE_INLINE static bool has_step_events_queued() { return (step_event_queue.head != step_event_queue.tail); }

    // Check if the queue of step events is full.
    FORCE_INLINE static bool is_step_event_queue_full() { return step_event_queue.tail == step_event_queue_next_index(step_event_queue.head); }

    // Number of step events in the queue.
    FORCE_INLINE static uint16_t step_event_queue_size() { return STEP_EVENT_QUEUE_MOD(step_event_queue.head - step_event_queue.tail); }

    // Returns number of free slots in the step event queue.
    FORCE_INLINE static uint16_t step_event_queue_free_slots() { return STEP_EVENT_QUEUE_SIZE - 1 - step_event_queue_size(); }

    // Returns the current step event, nullptr if the queue is empty.
    static step_event_t *get_current_step_event() {
        if (has_step_events_queued())
            return &step_event_queue.data[step_event_queue.tail];

        return nullptr;
    }

    // Returns the first head step event, nullptr if the queue is full.
    // Also, it returns the next step event queue head index (passed by reference).
    FORCE_INLINE static step_event_t *get_next_free_step_event(uint16_t &next_step_event_queue_head) {
        if (is_step_event_queue_full())
            return nullptr;

        // Return the first available step event.
        next_step_event_queue_head = step_event_queue_next_index(step_event_queue.head);
        return &step_event_queue.data[step_event_queue.head];
    }

    // Discard the current step event.
    FORCE_INLINE static void discard_current_step_event() {
        if (has_step_events_queued())
            step_event_queue.tail = step_event_queue_next_index(step_event_queue.tail);
    }

    FORCE_INLINE static void step_generator_state_clear() {
        for (step_event_info_t &step_event_info : step_generator_state.step_events) {
            step_event_info.time = 0.;
            step_event_info.flags = 0;
        }

        step_generator_state.nearest_step_event_idx = 0;
        step_generator_state.previous_step_time = 0.;
        step_generator_state.initialized = false;
        step_generator_state.current_distance = { 0., 0., 0., 0. };
    }

    FORCE_INLINE static move_t *move_segment_queue_next_move(const move_t &move) {
        int64_t move_idx = &move - &move_segment_queue.data[0];
        if (move_idx < 0 || move_idx >= MOVE_SEGMENT_QUEUE_SIZE)
            fatal_error("move_idx out of bounds.", "move_segment_queue_next_move");
        else if (move_idx == move_segment_queue.head)
            fatal_error("Input move segment is out of the queue.", "move_segment_queue_next_move");

        if (uint8_t next_move_idx = move_segment_queue_next_index(uint8_t(move_idx)); next_move_idx == move_segment_queue.head)
            return nullptr;
        else
            return &PreciseStepping::move_segment_queue.data[next_move_idx];
    }

    // This function must be called after the whole actual move segment is processed or the artificially
    // created move segment is processed, as in the input shaper case.
    static void move_segment_processed_handler();

    // Reset the step/move queue
    static void quick_stop() { stop_pending = true; }

    // Return true if the motion is being stopped
    static bool stopping() { return stop_pending; }

    // Return if any of queues have blocks pending
    static bool has_blocks_queued() { return has_move_segments_queued() || has_step_events_queued(); }

    // Return if some processing is still pending before all queues are flushed
    static bool processing() { return has_blocks_queued() || stop_pending; }

    static volatile uint8_t step_dl_miss; // stepper deadline misses
    static volatile uint8_t step_ev_miss; // stepper event misses

private:
    static uint32_t waiting_before_delivering_start_time;

    static void step_generator_state_init(move_t *move);

    static std::atomic<bool> stop_pending;
    static void reset_queues();
    static bool is_waiting_before_delivering();
};
