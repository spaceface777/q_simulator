// The tub reaches the left laser, but the LED never turns magenta, so that stays undecided.
let m = module(1);

prop tub_reaches_the_left_laser =
    eventually entered(tub(1), m.left_laser);

prop led_turns_magenta =
    eventually (m.led_color == 0xff00ff);
