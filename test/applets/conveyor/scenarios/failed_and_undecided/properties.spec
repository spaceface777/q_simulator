// One property fails and one stays undecided. --keep-going reports both.
let m = module(1);

prop led_stays_dark =
    always (m.led_color == 0x000000);

prop led_turns_magenta =
    eventually (m.led_color == 0xff00ff);
