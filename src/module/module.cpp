#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <iostream>
#include <string>
#include <cstring>
#include <fstream>

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

			// file for id

			const std::filesystem::path id_path = "/tmp/qaid";
			std::ofstream id_of(id_path);
			if (!id_of.is_open())
			{
				throw std::runtime_error("Failed to create file for " + std::to_string(_id));
			}
			id_of << std::to_string(_id);
			id_of.flush();
			id_of.close();

			// exec

			const std::string module_name = "Module " + std::to_string(_id);
			const int success = ::execl(path.c_str(), module_name.c_str(), (char *)0);

			if (success < 0)
			{
				throw std::runtime_error("Failed to execl: " + std::string(::strerror(errno)));
			}
		}

		_running = true;
	}

	void Module::send(const packet::Packet &packet)
	{
		server::send(_id, packet);
	}

	void Module::receive(const packet::Packet &packet)
	{
		for (const std::unique_ptr<tub::Tub> &tub : tubs_) 
		{
			
		}
	}

}