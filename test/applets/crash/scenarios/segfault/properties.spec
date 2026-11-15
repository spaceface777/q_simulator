let healthy = module(2);

prop healthy_module_heartbeats_to_the_end =
    eventually[900ms, 1s] (exists request in rpc_requests(healthy): request.cmd == .rpc_cmd_heartbeat);
