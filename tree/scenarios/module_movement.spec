let m = module(1);
let t = tub(1);
let led = m.led_color;
let servo = m.servo_angle;
let small_belt = m.small_belt_speed;
let big_belt = m.big_belt_speed;
let servo_right = servo == 60.0;
let small_reverse = small_belt == -100.0;
let big_reverse = big_belt == -100.0;
let commands_changed =
    changed(led)
    || changed(servo)
    || changed(small_belt)
    || changed(big_belt);
let blue_right_commands =
    led == 0x0000ff
    && servo_right
    && small_reverse
    && big_reverse;
let red_return_commands =
    led == 0xff0000
    && servo_right
    && small_belt == 100.0
    && big_belt == 100.0;
let green_cross_commands =
    led == 0x00ff00
    && servo == 0.0
    && small_reverse
    && big_belt == 100.0;
let right_arrival = entered(t, m.right_laser);
let entry_arrival = entered(t, m.entry);
let left_arrival = entered(t, m.left_laser);
let route_arrival = right_arrival || entry_arrival || left_arrival;
let before_next_route_arrival = !route_arrival;
let reader_arrival = entry_arrival && m.rfid_active;
let left_laser_event =
    exists event in rpc_events(m):
        event.kind == .rpc_event_laser_left_detect;
let right_laser_event =
    exists event in rpc_events(m):
        event.kind == .rpc_event_laser_right_detect;
let rfid_event =
    exists event in rpc_events(m):
        event.kind == .rpc_event_rfid_detect;

// === Safety: the route never issues an invalid or locally unsafe command ===

prop never_hands_the_tub_to_another_module =
    always at_module(t, m);

prop every_present_tub_belongs_to_a_layout_module =
    always (forall tub: Tub:
        present(tub) -> (
            exists owner: Module:
                at_module(tub, owner)
        )
    );

prop never_contains_more_than_one_present_tub =
    always (count tub: Tub where (present(tub)) <= 1);

prop startup_tub_remains_present_on_the_next_logical_tick =
    next present(t);

prop belt_motion_requires_both_laser_emitters =
    always (
        (small_belt != 0.0 || big_belt != 0.0)
        -> (m.left_laser_enabled && m.right_laser_enabled)
    );

prop actuator_commands_stay_in_the_applets_command_set =
    always (
        (small_reverse || small_belt == 0.0 || small_belt == 100.0)
        && (big_reverse || big_belt == 0.0 || big_belt == 100.0)
        && (servo == 0.0 || servo_right || servo == 120.0)
    );

prop physical_servo_stays_inside_the_applets_sweep =
    always (m.servo_physical_angle >= 0.0 && m.servo_physical_angle <= 120.0);

prop every_delivered_rpc_response_is_successful_and_correlated =
    always (forall response in rpc_responses(m):
            response.status == .ok
            && response.module == m
            && response.request.module == m
            && response.cmd == response.request.cmd
    );

prop every_confirmable_request_is_initially_undelivered =
    always (forall request in rpc_requests(m):
            request.confirmable
            -> (rpc_response_to(request).status == .undelivered)
    );

prop every_request_has_valid_and_unique_monitor_metadata =
    always (forall request in rpc_requests(m):
        request.id > 0
        && request.call_id >= 0
        && request.call_id <= 65535
        && request.module == m
        && count r2 in rpc_requests(m) where (r2.id == request.id) == 1
    );

prop every_confirmable_request_receives_its_correlated_response_promptly =
    always (
        forall request in rpc_requests(m):
            request.confirmable
            -> (eventually[0ms, 50ms] (rpc_response_to(request).status == .ok))
    );

prop at_most_one_left_laser_event_is_delivered_per_tick =
    always (
        count event in rpc_events(m) where (
            event.kind == .rpc_event_laser_left_detect
        ) <= 1
    );

prop at_most_one_event_of_each_route_kind_is_delivered_per_tick =
    always (
        (count event in rpc_events(m) where (
            event.kind == .rpc_event_laser_left_detect
        ) <= 1)
        && (count event in rpc_events(m) where (
            event.kind == .rpc_event_laser_right_detect
        ) <= 1)
        && (count event in rpc_events(m) where (
            event.kind == .rpc_event_rfid_detect
        ) <= 1)
    );

prop left_laser_events_have_a_five_second_quiet_period =
    always (left_laser_event -> (always[1ms, 5s] !left_laser_event));

prop route_events_are_delivered_with_their_physical_inputs_active =
    always (forall event in rpc_events(m):
        (
            (event.kind == .rpc_event_laser_left_detect -> m.left_laser_blocked)
            && (event.kind == .rpc_event_laser_right_detect -> m.right_laser_blocked)
            && (event.kind == .rpc_event_rfid_detect -> m.rfid_active)
        )
    );

prop every_delivered_event_belongs_to_the_route_vocabulary =
    always (forall event in rpc_events(m):
        event.module == m
        && event.id > 0
        && (
            event.kind == .rpc_event_laser_left_detect
            || event.kind == .rpc_event_laser_right_detect
            || event.kind == .rpc_event_rfid_detect
        )
    );

prop every_rfid_event_carries_the_route_tubs_generated_uid =
    always (forall event in rpc_events(m):
        event.kind == .rpc_event_rfid_detect
        -> event.payload == "\x01\x00\x00\x00\x00\x00\x00"
    );

// Core 1 clocks may miss ticks, so the applet does not necessarily run on the next global tick.
// `next` follows this module's own steps rather than the global clock, which is what the route
// actually claims: the very next time this applet runs, it starts the phase.
prop each_left_laser_delivery_starts_the_next_route_phase_on_the_next_step =
    always (left_laser_event -> (next (
        count request in rpc_requests(m) where (
            request.cmd == .rpc_cmd_led_set_color
        ) >= 1))
    );

prop each_right_laser_delivery_starts_the_next_route_phase_on_the_next_step =
    always (right_laser_event -> (next (
        count request in rpc_requests(m) where (
            request.cmd == .rpc_cmd_led_set_color
        ) >= 1))
    );

// Reaching RFID performs three synchronous operations before the next phase: stop the small
// belt, read the UID, and restart the belt. It therefore has a bounded rather than next-tick LED
// reaction.
prop each_rfid_delivery_starts_the_next_route_phase_promptly =
    always (
        rfid_event
        -> (eventually[1ms, 500ms] (
            count request in rpc_requests(m) where (
                request.cmd == .rpc_cmd_led_set_color
            ) >= 1
        ))
    );

// Entering the complete command state is the externally visible start of a leg. Watching every
// relevant field matters because a redundant final RPC may leave its field unchanged: the first
// blue leg becomes complete when the big belt changes, while the second becomes complete when the
// servo changes because the big belt is already running backwards.
prop every_blue_right_leg_keeps_its_commands_until_arrival =
    always (
        (commands_changed && blue_right_commands)
        -> (blue_right_commands until[0ms, 10s] right_arrival)
    );

// Not every blue right arrival starts the same next leg. Follow the route phase so each nested
// metric obligation is activated by the correct occurrence: red return, green crossing, red return.
prop each_right_arrival_starts_its_phase_specific_next_leg_promptly =
    eventually (
        right_arrival
        && (eventually[0ms, 500ms] red_return_commands)
        && (eventually (
            right_arrival
            && (eventually[0ms, 500ms] green_cross_commands)
            && (eventually (
                right_arrival
                && (eventually[0ms, 500ms] red_return_commands)
            ))
        ))
    );

// A production controller should stop all actuators if an expected detector event never arrives.
// The demonstration applet intentionally waits forever. Requires an observable fault state:
// prop a_stalled_leg_enters_a_safe_failure_state =
//     always (leg_started(m, t, destination) -> eventually[0s, 5s] safe_failure(m));

// === Liveness: the complete eight-leg demonstration route occurs in order ===

prop completes_the_demonstration_route = sequence {
    right_arrival;
    entry_arrival;
    left_arrival;
    entry_arrival;
    right_arrival;
    left_arrival;
    right_arrival;
    entry_arrival;
};

// The same eight arrivals with the left and right legs swapped. Unlike `sequence`, this follows
// consecutive route arrivals: an unbounded sequence may skip intervening arrivals and would find
// this permutation across two repetitions of the correct route.
prop does_not_complete_the_demonstration_route_out_of_order = !eventually (
    left_arrival
    && (next (before_next_route_arrival until (entry_arrival
        && (next (before_next_route_arrival until (right_arrival
            && (next (before_next_route_arrival until (entry_arrival
                && (next (before_next_route_arrival until (left_arrival
                    && (next (before_next_route_arrival until (right_arrival
                        && (next (before_next_route_arrival until (left_arrival
                            && (next (before_next_route_arrival until (entry_arrival)))
                        )))
                    )))
                )))
            )))
        )))
    )))
);

prop returns_to_the_reader_with_the_tag_visible =
    eventually (right_arrival
        && (eventually (reader_arrival
            && (eventually (reader_arrival
                && (eventually (reader_arrival
                ))
            ))
        ))
    );
