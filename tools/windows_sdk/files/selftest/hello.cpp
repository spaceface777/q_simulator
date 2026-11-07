#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

extern "C" const char *greeting(void);

int main()
{
	std::string from_thread;
	std::thread worker([&] { from_thread = "thread"; });
	worker.join();
	try {
		throw std::runtime_error("exception");
	} catch (const std::exception &error) {
		std::cout << greeting() << ", " << from_thread << " and " << error.what() << " work\n";
	}
	return sizeof(void *) == 4 ? 0 : 1;
}
