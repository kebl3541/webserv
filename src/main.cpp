#include "Config.hpp"
#include "EventLoop.hpp"
#include "Logger.hpp"

#include <signal.h>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
	void	onSignal(int signal)
	{
		(void)signal;
		// Nothing else belongs in a signal handler: only async-signal-safe work
		// is permitted, and setting a flag is the whole of it. The loop notices
		// on its next pass and unwinds normally, running every destructor.
		EventLoop::requestStop();
	}

	void	usage(const char* program)
	{
		std::cerr << "usage: " << program << " [configuration file]\n"
				  << "\n"
				  << "  The configuration defaults to conf/default.conf.\n"
				  << "  Set WEBSERV_LOG to debug, info, warn or error to pick a\n"
				  << "  log level (default: info).\n";
	}

	LogLevel	levelFromEnvironment(void)
	{
		const char*	value = std::getenv("WEBSERV_LOG");

		if (!value)
			return LOG_INFO;

		const std::string	text(value);
		if (text == "debug")
			return LOG_DEBUG;
		if (text == "info")
			return LOG_INFO;
		if (text == "warn")
			return LOG_WARN;
		if (text == "error")
			return LOG_ERROR;
		if (text == "none")
			return LOG_NONE;
		return LOG_INFO;
	}
}

int	main(int argc, char** argv)
{
	if (argc > 2)
	{
		usage(argv[0]);
		return 1;
	}
	if (argc == 2 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help"))
	{
		usage(argv[0]);
		return 0;
	}

	Logger::setLevel(levelFromEnvironment());

	const std::string	configPath = (argc == 2) ? argv[1] : "conf/default.conf";

	Config	config;
	if (!config.loadFromFile(configPath))
	{
		Logger::error("configuration error: " + config.error());
		return 1;
	}
	Logger::info("loaded " + configPath);

	// SIGPIPE would otherwise kill the process the first time a client
	// disappears mid-response. Ignoring it turns that into an error return from
	// send(), which the connection code already handles.
	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, onSignal);
	signal(SIGTERM, onSignal);

	// The loop lives in its own scope so that its destructor runs, closing every
	// socket and reaping every child, before main returns.
	EventLoop	loop(config);
	if (!loop.setup())
	{
		Logger::error("could not bind the configured endpoints");
		return 1;
	}

	return loop.run();
}
