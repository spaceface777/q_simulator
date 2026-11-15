prop both_modules_ask_for_their_id =
    forall m: Module:
        eventually (exists request in rpc_requests(m): request.cmd == .rpc_cmd_get_own_id);

prop both_modules_receive_a_greeting =
    forall m: Module:
        eventually (exists event in rpc_events(m): event.kind == .rpc_event_message_received);

prop every_send_is_answered_ok =
    always (forall m: Module:
        forall request in rpc_requests(m):
            request.cmd == .rpc_cmd_send_packet
                -> (eventually[0ms, 50ms] (rpc_response_to(request).status == .ok)));
