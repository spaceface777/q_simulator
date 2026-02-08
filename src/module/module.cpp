#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <iostream>
#include <string>
#include <cstring>

#include "module.h"
#include "../tcp/server.h"

namespace module
{

	void Module::run(std::filesystem::path path)
	{
		_pid = ::fork();
		if (_pid == -1)
		{
			throw std::runtime_error("Failed to fork: " + std::string(std::strerror(errno)));
		}

		const bool child = _pid == 0;
		if (child)
		{
			std::cout << "Starting process " << path << " at " << ::getpid() << std::endl;

			const std::string module_name = "Module " + std::to_string(_id);
			const int success = ::execl(path.c_str(), module_name.c_str(), (char *)0);

			if (success < 0)
			{
				throw std::runtime_error("Failed to execl: " + std::string(::strerror(errno)));
			}
		}

		_running = true;
	}

	void Module::send(packet::Packet packet)
	{
		server::send(_id, packet);
	}

}