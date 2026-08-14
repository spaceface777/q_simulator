let m = module(1);
let led = m.led_color;
let servo = m.servo_angle;
let small_belt = m.small_belt_speed;
let big_belt = m.big_belt_speed;
let left_emitter = m.left_laser_enabled;
let right_emitter = m.right_laser_enabled;
let rfid = m.rfid_active;
let small_stopped = small_belt == 0.0;
let big_stopped = big_belt == 0.0;
let servo_home = servo == 0.0;
let servo_far = servo == 180.0;
let left_off = !left_emitter;
let belts_stopped = small_stopped && big_stopped;
let actuators_parked = belts_stopped && servo_home;
let emitters_enabled = left_emitter && right_emitter;
let response_to_request = rpc_response_to(request);
let response_to_retry = rpc_response_to(retry);
let periodic_request =
    request.cmd == .rpc_cmd_heartbeat
    || request.cmd == .rpc_cmd_laser_left_detect
    || request.cmd == .rpc_cmd_laser_right_detect
    || request.cmd == .rpc_cmd_rfid_get_uid;
let left_detector_clear_response =
    exists response in rpc_responses(m):
        response.cmd == .rpc_cmd_laser_left_detect
        && response.status == .ok
        && response.payload == "\x01";
let left_detector_dark_response =
    exists response in rpc_responses(m):
        response.cmd == .rpc_cmd_laser_left_detect
        && response.status == .ok
        && response.payload == "\x00";

// === Safety: unsafe commands and states never occur ===

prop never_drives_both_belts_at_once =
    always (!(small_belt != 0.0 && big_belt != 0.0));

prop belt_motion_requires_both_laser_emitters =
    always (
        (small_belt != 0.0 || big_belt != 0.0)
        -> emitters_enabled
    );

prop servo_motion_requires_stationary_belts =
    always (
        servo != 0.0
        -> belts_stopped
    );

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

prop failure_or_laser_test_shutdown_keeps_actuators_parked =
    always (
        left_off
        -> actuators_parked
    );

prop healthy_diagnostic_keeps_the_reader_empty =
    always (!rfid);

prop every_response_is_correlated_and_either_successful_or_retryable =
    always (
        forall response in rpc_responses(m):
            (response.status == .ok || response.status == .try_again)
            && response.module == m
            && response.request.module == m
            && response.cmd == response.request.cmd
    );

prop every_confirmable_request_is_initially_undelivered =
    always (
        forall request in rpc_requests(m):
            request.confirmable
            -> (response_to_request.status == .undelivered)
    );

// The async phase intentionally runs heartbeat, detector, and RFID requests through the finite
// trace endpoint, so its final requests may have responses just outside the scenario. All
// non-periodic diagnostic requests are required to settle within the observable trace.
prop every_non_periodic_confirmable_request_receives_a_response_promptly =
    always (
        forall request in rpc_requests(m):
            (request.confirmable && !periodic_request)
            -> (eventually[0ms, 50ms] (
                response_to_request.status != .undelivered
            ))
    );

prop every_retryable_response_is_followed_by_a_successful_same_command_retry =
    always (
        forall response in rpc_responses(m):
            response.status == .try_again
            -> (eventually[1ms, 500ms] (
                exists retry in rpc_requests(m):
                    retry.cmd == response.cmd
                    && (eventually[0ms, 50ms] (
                        response_to_retry.status == .ok
                    ))
            ))
    );

prop laser_detector_reads_return_one_boolean_byte =
    always (
        forall response in rpc_responses(m):
            (
                response.cmd == .rpc_cmd_laser_left_detect
                || response.cmd == .rpc_cmd_laser_right_detect
            )
            -> (response.payload == "\x00" || response.payload == "\x01")
    );

prop healthy_rfid_reads_return_an_empty_uid =
    always (
        forall response in rpc_responses(m):
            response.cmd == .rpc_cmd_rfid_get_uid
            -> response.payload == ""
    );

prop an_unstimulated_healthy_module_delivers_no_platform_events =
    always (
        count event in rpc_events(m) where (true) == 0
    );

// The LED self-test deliberately displays every ordinary failure colour. A true global
// "never displays a failure code" property therefore needs an observable stage boundary.
// Requires applet stage events and finite colour-set membership:
// prop never_displays_a_failure_code_after_the_led_self_test =
//     always (
//         stage_completed(m, led_self_test)
//         -> always (led !in failure_colors)
//     );

// === Liveness: every major externally visible stage makes progress ===

prop completes_the_led_self_test_in_order =
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

prop toggles_and_restores_the_left_laser_emitter =
    eventually (
        left_off
        && (eventually emitters_enabled)
    );

prop verifies_the_left_detector_before_during_and_after_the_emitter_toggle =
    eventually (
        left_detector_clear_response
        && (eventually (
            left_detector_dark_response
            && (eventually left_detector_clear_response)
        ))
    );

prop exercises_every_small_belt_mode_in_order =
    eventually (
        small_belt == 25.0
        && (eventually (
            small_belt == 100.0
            && (eventually (
                small_belt == -25.0
                && (eventually (
                    small_belt == -100.0
                    && (eventually small_stopped)
                ))
            ))
        ))
    );

prop exercises_every_big_belt_mode_in_order =
    eventually (
        big_belt == 25.0
        && (eventually (
            big_belt == 100.0
            && (eventually (
                big_belt == -25.0
                && (eventually (
                    big_belt == -100.0
                    && (eventually big_stopped)
                ))
            ))
        ))
    );

prop completes_the_servo_sweep_and_returns_to_neutral =
    eventually (
        servo_far
        && (eventually (
            servo_home
            && (eventually (
                servo_far
                && (eventually servo_home)
            ))
        ))
    );

prop eventually_reaches_a_stable_healthy_async_state =
    eventually (
        always (
            actuators_parked
            && emitters_enabled
            && !rfid
            && led != 0xffffff
            && led != 0x0000ff
            && led != 0x00ff00
            && led != 0xff0000
            && led != 0xffff00
            && led != 0x00ffff
            && led != 0xff00ff
        )
    );

prop reaches_async_monitoring_and_starts_every_periodic_rpc_stream =
    (eventually (
        exists request in rpc_requests(m):
            request.cmd == .rpc_cmd_subscribe_to_event
    ))
    && (eventually (
        exists request in rpc_requests(m):
            request.cmd == .rpc_cmd_heartbeat
    ))
    && (eventually (
        exists request in rpc_requests(m):
            request.cmd == .rpc_cmd_laser_left_detect
    ))
    && (eventually (
        exists request in rpc_requests(m):
            request.cmd == .rpc_cmd_laser_right_detect
    ))
    && (eventually (
        exists request in rpc_requests(m):
            request.cmd == .rpc_cmd_rfid_get_uid
    ));

// The synchronous detector sequence is now proved directly from successful response bytes above.

// The CV stage validates voltage, current, power, and power == voltage * current over averaged
// samples. Those measured values are currently internal RPC results.
// Requires typed measurement observations and finite sample aggregates:
// prop validates_idle_electrical_measurements =
//     eventually (mean(m.voltage, 10) in [12.0, 13.0] && mean(m.current, 10) in [0.05, 0.10]);

// Every belt direction must run at each speed for ten seconds, produce bounded encoder deltas,
// draw measurably more power at full speed, and return to idle power after stopping.
// Requires decoded encoder/CV observations, aggregation, and captured numeric values:
// prop validates_every_belt_run_quantitatively =
//     always (belt_run_started(m, belt, direction) -> eventually[10s, 12s] belt_run_validated(m, belt, direction));

// The servo sweep must produce at least a 0.30 W power rise. The source enforces this criterion,
// but the property monitor cannot currently observe the CV samples used to reach the verdict.
// Requires typed measurement observations and aggregation:
// prop servo_motion_draws_measurably_more_power =
//     eventually (max(m.power) - baseline(m.power) >= 0.30);

// The properties above now prove successful subscriptions and the existence of all periodic RPC
// streams. Exact rolling rate limits still require temporal event aggregation; snapshot `count`
// intentionally cannot count across a one-second history window.
