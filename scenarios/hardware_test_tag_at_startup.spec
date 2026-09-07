let m = module(1);
let tag = tub(1);
let led = m.led_color;
let left_emitter = m.left_laser_enabled;
let left_off = !left_emitter;
let magenta_led = led == 0xff00ff;
let black_led = led == 0x000000;
let white_led = led == 0xffffff;

// === Safety: the startup violation is latched and leaves the mechanism safe ===

prop tag_appears_under_the_reader_at_startup at 0ms =
    appeared(tag)
    && entered(tag, m.entry)
    && m.rfid_active;

prop startup_tag_remains_under_the_reader =
    always (at_position(tag, m.entry) && m.rfid_active);

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

prop rfid_failure_latches_and_parks_the_module =
    eventually (
        magenta_led
        && (always (
            (magenta_led || black_led)
            && m.big_belt_speed == 0.0
            && m.small_belt_speed == 0.0
            && m.servo_angle == 0.0
            && left_off
        ))
    );

// === Liveness: unaffected stages finish before the RFID failure is reported ===

prop led_self_test_completes_before_the_failure_is_reported =
    eventually (
        led == 0xff0000
        && (eventually (
            led == 0x00ff00
            && (eventually (
                led == 0x0000ff
                && (eventually white_led)
            ))
        ))
    );

prop laser_test_toggles_and_restores_the_left_emitter =
    eventually (
        white_led
        && (eventually (
            left_off
            && (eventually left_emitter)
        ))
    );

prop startup_tag_reports_a_blinking_magenta_rfid_failure =
    eventually (
        magenta_led
        && (eventually (
            black_led
            && (eventually magenta_led)
        ))
    );

// The generated simulator UID is seven bytes: the tub id followed by six zero bytes. Observing it
// in a successful response proves the magenta failure was caused by real startup presence rather
// than an RFID transport or platform error.
prop startup_tag_is_classified_from_a_successful_rfid_read =
    eventually (
        exists request in rpc_requests(m):
            request.cmd == .rpc_cmd_rfid_get_uid
            && (eventually[0ms, 50ms] (
                rpc_response_to(request).status == .ok
                && rpc_response_to(request).payload == "\x01\x00\x00\x00\x00\x00\x00"
            ))
    );
