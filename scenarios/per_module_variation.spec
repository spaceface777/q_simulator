let m5 = module(5);
let m12 = module(12);
let t5 = tub(5);
let t12 = tub(12);
let right_arrival_5 = entered(t5, m5.right_laser);
let entry_arrival_5 = entered(t5, m5.entry);
let right_arrival_12 = entered(t12, m12.right_laser);
let entry_arrival_12 = entered(t12, m12.entry);

// === Safety: calibration changes timing, never ownership or valid commands ===

prop each_tub_remains_on_its_own_module =
    always (at_module(t5, m5) && at_module(t12, m12));

prop every_present_tub_belongs_to_a_layout_module =
    always (
        forall tub: Tub:
            present(tub)
            -> (exists owner: Module: at_module(tub, owner))
    );

prop exactly_the_two_scheduled_tubs_remain_present =
    always (
        count tub: Tub where (present(tub)) == 2
    );

prop every_module_keeps_both_laser_emitters_enabled_while_moving =
    always (
        forall module: Module:
            (module.small_belt_speed != 0.0 || module.big_belt_speed != 0.0)
            -> (module.left_laser_enabled && module.right_laser_enabled)
    );

prop every_module_uses_only_the_applets_command_set =
    always (
        forall module: Module:
            (
                (module.small_belt_speed == -100.0
                    || module.small_belt_speed == 0.0
                    || module.small_belt_speed == 100.0)
                && (module.big_belt_speed == -100.0
                    || module.big_belt_speed == 0.0
                    || module.big_belt_speed == 100.0)
                && (module.servo_angle == 0.0
                    || module.servo_angle == 60.0
                    || module.servo_angle == 120.0)
                && module.servo_physical_angle >= 0.0
                && module.servo_physical_angle <= 120.0
            )
    );

prop every_delivered_response_is_successful_and_correlated =
    always (
        forall module: Module:
            (forall response in rpc_responses(module):
                response.status == .ok
                && response.module == module
                && response.request.module == module
                && response.cmd == response.request.cmd)
    );

prop every_route_event_matches_the_modules_physical_input =
    always (
        forall module: Module:
            (forall event in rpc_events(module):
                (
                    (event.kind == .rpc_event_laser_left_detect -> module.left_laser_blocked)
                    && (event.kind == .rpc_event_laser_right_detect -> module.right_laser_blocked)
                    && (event.kind == .rpc_event_rfid_detect -> module.rfid_active)
                ))
    );

// === Liveness: both independent routes close a loop despite physical variation ===

prop module_5_completes_a_loop_and_starts_again =
    eventually (
        right_arrival_5
        && (eventually (
            entry_arrival_5
            && (eventually (
                entered(t5, m5.left_laser)
                && (eventually (
                    entry_arrival_5
                    && (eventually right_arrival_5)
                ))
            ))
        ))
    );

prop module_12_completes_a_loop_and_starts_again =
    eventually (
        right_arrival_12
        && (eventually (
            entry_arrival_12
            && (eventually (
                entered(t12, m12.left_laser)
                && (eventually (
                    entry_arrival_12
                    && (eventually right_arrival_12)
                ))
            ))
        ))
    );

// Cross-seed orchestration is intentionally outside the property language. The scenario suite
// runs this same property file over its stable seed corpus separately.
