let m = module(1);
let tag = tub(1);
let obstruction = tub(2);
let led = m.led_color;
let servo = m.servo_angle;
let servo_left = servo == 0.0;
let servo_center = servo == 90.0;
let green_dim = led == 0x003f00;
let green_bright = led == 0x00ff00;
let blue_bright = led == 0x0000ff;
let aligned_dim = servo_center && green_dim;
let aligned_bright = servo_center && green_bright;
let left_blocked_bright = servo_left && blue_bright;
let confirmable_request_succeeds = !q.confirmable || (F[0ms, 50ms] (rpc_response_to(q).status == .ok));

// === Safety: this diagnostic only observes tubs and drives display actuators ===

prop alignment_never_drives_either_belt = G (m.big_belt_speed == 0.0 && m.small_belt_speed == 0.0);

prop physical_servo_stays_inside_its_hardware_range =
    G (m.servo_physical_angle >= 0.0 && m.servo_physical_angle <= 180.0);

prop alignment_outputs_stay_in_the_documented_table = G (
    (servo_left || servo_center || servo == 180.0)
    && (led == 0x000000 || green_dim || green_bright || led == 0x00003f || blue_bright || led == 0x3f0000
        || led == 0xff0000 || led == 0x3f3f3f || led == 0xffffff)
);

prop scenario_tubs_stay_at_their_observation_points = G (
    (present(tag) -> at_position(tag, m.entry))
    && (present(obstruction) -> at_position(obstruction, m.left_laser))
);

prop removing_a_scenario_tub_emits_its_location_exit_edge = G (
    (disappeared(tag) -> left(tag, m.entry))
    && (disappeared(obstruction) -> left(obstruction, m.left_laser))
);

prop every_response_is_successful_and_correlated_to_this_module = G (
    \A r in rpc_responses(m): r.status == .ok && r.module == m && r.request.module == m && r.cmd == r.request.cmd
);

prop every_confirmable_request_receives_its_response_promptly =
    G (\A q in rpc_requests(m): confirmable_request_succeeds);

prop detector_responses_contain_exactly_one_boolean_byte = G (
    \A r in rpc_responses(m):
        (r.cmd == .rpc_cmd_laser_left_detect || r.cmd == .rpc_cmd_laser_right_detect
            || r.cmd == .rpc_cmd_rfid_check_tag)
        -> (r.payload == "\x00" || r.payload == "\x01")
);

prop alignment_does_not_subscribe_to_platform_events = G (
    (# e in rpc_events(m) where (true) == 0)
    && (\A q in rpc_requests(m): q.cmd != .rpc_cmd_subscribe_to_event)
);

// === Liveness: every input change produces its corresponding output and recovery ===

prop enables_both_laser_emitters = F (G (m.left_laser_enabled && m.right_laser_enabled));

prop starts_all_three_periodic_rpc_streams =
    (F (\E q in rpc_requests(m): q.cmd == .rpc_cmd_heartbeat))
    && (F (\E q in rpc_requests(m): q.cmd == .rpc_cmd_rfid_check_tag))
    && (F (\E q in rpc_requests(m): q.cmd == .rpc_cmd_laser_left_detect));

// Each step is looked for from one observation after the step before it matched, so the five
// display states are required to occur in order and on distinct ticks. The steps carry no bounds:
// the gaps between them are however long the scenario waits before moving a tub, and the reaction
// budget belongs to the edges below rather than between two states.
prop follows_alignment_and_brightness_changes_in_order = sequence {
    aligned_dim;
    aligned_bright;
    left_blocked_bright;
    aligned_bright;
    aligned_dim;
};

prop responds_to_each_scenario_edge = G (
    (appeared(tag) -> (F[0ms, 250ms] aligned_bright))
    && (appeared(obstruction) -> (F[0ms, 250ms] left_blocked_bright))
    && (disappeared(obstruction) -> (F[0ms, 250ms] aligned_bright))
    && (disappeared(tag) -> (F[0ms, 250ms] aligned_dim))
);

// RPC read errors are deliberately collapsed into the same booleans as blocked beams. The
// successful, one-byte detector-response properties above rule that error path out. The output
// sequence and bounded edge reactions then prove that blue is the observed left obstruction.

// The source budgets laser, RFID, and heartbeat request rates independently. A bounded G alone
// checks one interval relative to its activation; it does not count a sliding event window.
// This requires network observations and a future rolling event-count operator:
// prop polling_stays_within_platform_rate_limits = G (
//     count_events[0s, 1s](laser_rpc(m)) <= 50
//     && count_events[0s, 1s](rfid_rpc(m)) <= 10
//     && count_events[0s, 1s](heartbeat(m)) <= 25
// );
