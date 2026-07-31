#ifndef LOGGER_HPP
# define LOGGER_HPP

# include <string>

// Severity levels, ordered so that a threshold comparison filters messages.
enum LogLevel
{
	LOG_DEBUG = 0,
	LOG_INFO  = 1,
	LOG_WARN  = 2,
	LOG_ERROR = 3,
	LOG_NONE  = 4
};

// Process-wide logger. Writes to stderr so that stdout stays free for any
// tooling that wants to pipe server output somewhere else.
class Logger
{
	public:
		static void		setLevel(LogLevel level);
		static LogLevel	getLevel(void);

		// Colour is disabled automatically when stderr is not a terminal,
		// so redirected logs stay free of escape sequences.
		static void	setColour(bool enabled);

		static void	debug(const std::string& message);
		static void	info(const std::string& message);
		static void	warn(const std::string& message);
		static void	error(const std::string& message);

	private:
		Logger();
		Logger(const Logger& other);
		Logger&	operator=(const Logger& other);

		static void	emit(LogLevel level, const std::string& message);

		static LogLevel	_level;
		static bool		_colour;
		static bool		_colourResolved;
};

#endif
