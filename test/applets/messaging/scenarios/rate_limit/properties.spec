// The platform refuses sends over the budget rather than queueing them, and fails them no other way.
prop some_send_is_refused =
    exists m: Module:
        eventually (exists response in rpc_responses(m):
            response.cmd == .rpc_cmd_send_packet && response.status == .try_again);

prop sends_are_accepted_or_refused =
    always (forall m: Module:
        forall response in rpc_responses(m):
            response.cmd == .rpc_cmd_send_packet -> (response.status == .ok || response.status == .try_again));
