#include "Logger.hpp"

#include <unistd.h>
#include <cstdio>
#include <ctime>
#include <string>

LogLevel	Logger::_level = LOG_INFO;
bool		Logger::_colour = false;
bool		Logger::_colourResolved = false;

namespace
{
	const char*	RESET  = "\033[0m";
	const char*	GREY   = "\033[90m";
	const char*	GREEN  = "\033[32m";
	const char*	YELLOW = "\033[33m";
	const char*	RED    = "\033[31m";

	const char*	levelName(LogLevel level)
	{
		switch (level)
		{
			case LOG_DEBUG: return "DEBUG";
			case LOG_INFO:  return "INFO ";
			case LOG_WARN:  return "WARN ";
			case LOG_ERROR: return "ERROR";
			default:        return "?????";
		}
	}

	const char*	levelColour(LogLevel level)
	{
		switch (level)
		{
			case LOG_DEBUG: return GREY;
			case LOG_INFO:  return GREEN;
			case LOG_WARN:  return YELLOW;
			case LOG_ERROR: return RED;
			default:        return RESET;
		}
	}

	// Local wall-clock time, for humans reading the console.
	std::string	timestamp(void)
	{
		std::time_t	now = std::time(NULL);
		std::tm*	tm = std::localtime(&now);
		char		buffer[32];

		if (!tm)
			return "??:??:??";
		std::strftime(buffer, sizeof(buffer), "%H:%M:%S", tm);
		return std::string(buffer);
	}
}

void	Logger::setLevel(LogLevel level)
{
	_level = level;
}

LogLevel	Logger::getLevel(void)
{
	return _level;
}

void	Logger::setColour(bool enabled)
{
	_colour = enabled;
	_colourResolved = true;
}

void	Logger::emit(LogLevel level, const std::string& message)
{
	if (level < _level)
		return ;
	if (!_colourResolved)
	{
		_colour = (isatty(STDERR_FILENO) == 1);
		_colourResolved = true;
	}
	if (_colour)
	{
		std::fprintf(stderr, "%s[%s] %s%s %s\n", GREY, timestamp().c_str(),
			levelColour(level), levelName(level), message.c_str());
		std::fprintf(stderr, "%s", RESET);
	}
	else
		std::fprintf(stderr, "[%s] %s %s\n", timestamp().c_str(),
			levelName(level), message.c_str());
	std::fflush(stderr);
}

void	Logger::debug(const std::string& message)
{
	emit(LOG_DEBUG, message);
}

void	Logger::info(const std::string& message)
{
	emit(LOG_INFO, message);
}

void	Logger::warn(const std::string& message)
{
	emit(LOG_WARN, message);
}

void	Logger::error(const std::string& message)
{
	emit(LOG_ERROR, message);
}
