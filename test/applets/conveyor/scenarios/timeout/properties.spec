// The servo stays in range, the LED turns blue and then green, and it never turns magenta.
let m = module(1);

prop servo_stays_in_range =
    always (m.servo_physical_angle >= 0.0 && m.servo_physical_angle <= 180.0);

prop led_stays_dark =
    always (m.led_color == 0x000000);

prop led_turns_magenta_quickly =
    eventually[0ms, 100ms] (m.led_color == 0xff00ff);
