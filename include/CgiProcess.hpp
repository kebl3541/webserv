#ifndef CGIPROCESS_HPP
# define CGIPROCESS_HPP

# include <sys/types.h>
# include <ctime>
# include <map>
# include <string>

class HttpRequest;
class HttpResponse;
struct LocationConfig;
struct ServerConfig;

// Runs a CGI/1.1 script and exchanges data with it without ever blocking the
// event loop.
//
// Both pipes are non-blocking and registered with poll(): the request body is
// written in whatever slices the pipe accepts, and output is read as it
// appears. The original code wrote the entire body in one blocking loop, which
// deadlocks as soon as a body exceeds the 64 KiB pipe buffer and the child is
// waiting on output the parent is not yet reading.
class CgiProcess
{
	public:
		enum State
		{
			RUNNING,
			FINISHED,
			FAILED
		};

		CgiProcess();
		~CgiProcess();

		// Forks the interpreter. Returns false if the script cannot be run, in
		// which case status() carries the code to report.
		bool	start(const HttpRequest& request,
					  const ServerConfig& server,
					  const LocationConfig& location,
					  const std::string& scriptPath,
					  const std::string& pathInfo);

		// -1 when that direction is closed and should leave the poll set.
		int		inputFd(void) const;
		int		outputFd(void) const;

		bool	wantsWrite(void) const;

		// Both return false when the pipe died and the exchange must be torn down.
		bool	writeChunk(void);
		bool	readChunk(void);

		// Non-blocking reap. Returns true once the child has been collected.
		bool	reap(void);

		// True when the child overran its wall-clock budget.
		bool	hasTimedOut(time_t now, long limitSeconds) const;

		// Sends SIGKILL and collects the child. Safe to call more than once.
		void	terminate(void);

		State			state(void) const;
		int				status(void) const;
		pid_t			pid(void) const;

		// Splits the script's output into its header block and body, then maps
		// it onto a real HTTP response.
		bool	buildResponse(HttpResponse& response) const;

	private:
		CgiProcess(const CgiProcess& other);
		CgiProcess&	operator=(const CgiProcess& other);

		void	closeInput(void);
		void	closeOutput(void);

		static std::map<std::string, std::string>	buildEnvironment(
			const HttpRequest& request,
			const ServerConfig& server,
			const LocationConfig& location,
			const std::string& scriptPath,
			const std::string& pathInfo);

		int			_inputFd;		// parent writes the request body here
		int			_outputFd;		// parent reads the script's output here
		pid_t		_pid;
		State		_state;
		int			_status;
		bool		_reaped;

		std::string	_pendingInput;	// body bytes not yet accepted by the pipe
		size_t		_inputOffset;
		std::string	_output;
		time_t		_startedAt;
};

#endif
