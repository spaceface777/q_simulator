let m = module(1);
let t = tub(1);
let left_beam_event =
    exists event in rpc_events(m): event.kind == .rpc_event_laser_left_detect;

prop tub_stays_on_its_module =
    always at_module(t, m);

prop servo_stays_in_range =
    always (m.servo_physical_angle >= 0.0 && m.servo_physical_angle <= 180.0);

prop led_turns_blue_before_the_belts_run =
    always (m.big_belt_speed != 0.0 -> m.led_color == 0x0000ff);

prop tub_reaches_the_left_laser =
    eventually entered(t, m.left_laser);

prop the_left_beam_event_stops_the_belts_within_50ms =
    always (left_beam_event -> (eventually[0ms, 50ms] (m.big_belt_speed == 0.0 && m.small_belt_speed == 0.0)));

prop led_turns_green_at_the_end =
    eventually (always (m.led_color == 0x00ff00));

prop every_led_request_is_answered_ok =
    always (forall request in rpc_requests(m):
        request.cmd == .rpc_cmd_led_set_color
            -> (eventually[0ms, 50ms] (rpc_response_to(request).status == .ok)));
