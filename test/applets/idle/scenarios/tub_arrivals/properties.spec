prop no_tub_before_the_first_arrival =
    always[0ms, 499ms] (count tub: Tub where (present(tub)) == 0);

prop each_tub_arrives_at_its_module =
    eventually[1500ms, 1600ms] (
        at_module(tub(1), module(1)) && at_module(tub(2), module(2)) && at_module(tub(3), module(3)));

prop tubs_stay_where_they_arrived =
    always (forall tub: Tub: forall m: Module: (at_module(tub, m) -> (always at_module(tub, m))));
