let m = module(1);
let tag = tub(1);
let distraction = tub(2);
let servo = m.servo_angle;
let small_belt = m.small_belt_speed;
let big_belt = m.big_belt_speed;
let actuators_parked = big_belt == 0.0 && small_belt == 0.0 && servo == 0.0;
let tag_present = present(tag);
let distraction_present = present(distraction);
let tag_appeared = appeared(tag);
let response_to_request = rpc_response_to(request);
let left_laser_event =
    exists event in rpc_events(m):
        event.kind == .rpc_event_laser_left_detect;
let rfid_event =
    exists event in rpc_events(m):
        event.kind == .rpc_event_rfid_detect;

// === Safety: unrelated detector events cannot start the route ===

prop mechanism_remains_parked_until_the_tag_appears =
    always (
        !tag_present
        -> actuators_parked
    );

// This absolute scope is deliberately tied to the scenario's injected laser distraction. Unlike
// the implication below, it cannot pass vacuously if that tub event is accidentally omitted.
prop laser_distraction_window_keeps_the_mechanism_parked during [4s, 7s] =
    always actuators_parked;

prop a_blocked_left_beam_is_observed_without_motion =
    always (
        distraction_present
        -> (
            at_position(distraction, m.left_laser)
            && actuators_parked
        )
    );

prop detector_events_are_real_while_the_applet_waits =
    always (
        distraction_present
        -> (m.left_laser_enabled && m.right_laser_enabled)
    );

prop every_present_tub_remains_on_the_selected_module =
    always (
        (tag_present -> at_module(tag, m))
        && (distraction_present -> at_module(distraction, m))
    );

prop delivered_detector_events_match_the_observed_hardware_state =
    always (
        (left_laser_event -> m.left_laser_blocked)
        && (rfid_event -> m.rfid_active)
    );

prop the_distracting_laser_event_cannot_start_the_route_on_the_next_tick =
    always (left_laser_event -> (next actuators_parked));

prop the_real_rfid_event_carries_the_scheduled_uid =
    always (
        forall event in rpc_events(m):
            event.kind == .rpc_event_rfid_detect
            -> event.payload == "\x01\x00\x00\x00\x00\x00\x00"
    );

prop every_delivered_rpc_response_is_successful =
    always (
        forall response in rpc_responses(m):
            response.status == .ok
    );

prop every_confirmable_request_receives_its_correlated_response_promptly =
    always (
        forall request in rpc_requests(m):
            request.confirmable
            -> (eventually[0ms, 50ms] (response_to_request.status == .ok))
    );

// === Liveness: only the RFID tag starts real route progress ===

prop the_tag_starts_the_first_rightward_leg =
    always (
        tag_appeared
        -> (eventually (
            m.led_color == 0x0000ff
            && small_belt == -100.0
            && servo == 60.0
        ))
    );

prop rfid_delivery_starts_the_route_on_the_next_tick =
    always (
        // Later RFID arrivals deliberately stop/read/restart before selecting the next phase.
        // Only the scheduled tag's first delivery starts the route with an immediate blue LED.
        (tag_appeared && rfid_event)
        -> (next (
            count request in rpc_requests(m) where (
                request.cmd == .rpc_cmd_led_set_color
                && request.payload == "\xff\x00\x00\x00"
            ) >= 1
        ))
    );

prop the_tag_reaches_the_right_laser =
    always (
        tag_appeared
        -> (eventually entered(tag, m.right_laser))
    );

// The two next-tick properties above jointly exercise `wait_for_event`: the delivered laser event
// leaves the mechanism parked, while the later RFID delivery produces the first route command.
