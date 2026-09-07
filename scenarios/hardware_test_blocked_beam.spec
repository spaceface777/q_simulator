let m = module(1);
let obstruction = tub(1);
let led = m.led_color;
let servo = m.servo_angle;
let small_belt = m.small_belt_speed;
let big_belt = m.big_belt_speed;
let left_emitter = m.left_laser_enabled;
let cyan_led = led == 0x00ffff;
let black_led = led == 0x000000;

// === Safety: the detected failure is latched and leaves the mechanism safe ===

prop obstruction_is_present_and_blocks_the_beam_at_startup at 0ms =
    appeared(obstruction)
    && entered(obstruction, m.left_laser)
    && m.left_laser_blocked;

prop obstruction_remains_in_the_left_beam =
    always at_position(obstruction, m.left_laser);

prop actuator_commands_stay_in_hardware_ranges =
    always (
        small_belt >= -100.0
        && small_belt <= 100.0
        && big_belt >= -100.0
        && big_belt <= 100.0
        && servo >= 0.0
        && servo <= 180.0
    );

prop physical_servo_stays_in_its_hardware_range =
    always (m.servo_physical_angle >= 0.0 && m.servo_physical_angle <= 180.0);

prop every_delivered_rpc_response_is_successful =
    always (
        forall response in rpc_responses(m):
            response.status == .ok
    );

prop every_confirmable_request_receives_its_correlated_response_promptly =
    always (
        forall request in rpc_requests(m):
            request.confirmable
            -> (eventually[0ms, 50ms] (rpc_response_to(request).status == .ok))
    );

prop laser_failure_latches_and_parks_the_module =
    eventually (
        cyan_led
        && (always (
            (cyan_led || black_led)
            && big_belt == 0.0
            && small_belt == 0.0
            && servo == 0.0
            && !left_emitter
        ))
    );

// === Liveness: earlier tests run and the laser failure code visibly blinks ===

prop led_self_test_completes_before_the_failure_is_reported =
    eventually (
        led == 0xff0000
        && (eventually (
            led == 0x00ff00
            && (eventually (
                led == 0x0000ff
                && (eventually (led == 0xffffff))
            ))
        ))
    );

prop blocked_beam_reports_a_blinking_cyan_laser_failure =
    eventually (
        cyan_led
        && (eventually (
            black_led
            && (eventually cyan_led)
        ))
    );

// A sensor read error and a genuinely blocked beam share the cyan display. This property proves
// the diagnostic instead received a successful left-detector response whose Boolean payload says
// that the beam was not clear.
prop blocked_beam_is_classified_from_a_successful_detector_read =
    eventually (
        exists request in rpc_requests(m):
            request.cmd == .rpc_cmd_laser_left_detect
            && (eventually[0ms, 50ms] (
                rpc_response_to(request).status == .ok
                && rpc_response_to(request).payload == "\x00"
            ))
    );
