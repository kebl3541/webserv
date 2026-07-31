#include "CgiProcess.hpp"
#include "Config.hpp"
#include "HttpRequest.hpp"
#include "HttpResponse.hpp"
#include "Logger.hpp"
#include "MimeTypes.hpp"
#include "Utils.hpp"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
	const size_t	CGI_READ_BUFFER = 65536;
	const size_t	CGI_MAX_OUTPUT = 16 * 1024 * 1024;

	// Turns "Accept-Language" into "HTTP_ACCEPT_LANGUAGE" as CGI/1.1 requires.
	std::string	toEnvName(const std::string& headerName)
	{
		std::string	result = "HTTP_";

		for (size_t i = 0; i < headerName.size(); ++i)
		{
			char	c = headerName[i];
			if (c == '-')
				result += '_';
			else
				result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		}
		return result;
	}

	bool	setNonBlocking(int fd)
	{
		int	flags = fcntl(fd, F_GETFL, 0);

		if (flags == -1)
			return false;
		return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
	}

	// Frees the char* array handed to execve.
	void	releaseArray(std::vector<char*>& array)
	{
		for (size_t i = 0; i < array.size(); ++i)
			std::free(array[i]);
		array.clear();
	}

	char*	duplicate(const std::string& text)
	{
		char*	copy = static_cast<char*>(std::malloc(text.size() + 1));

		if (!copy)
			return NULL;
		std::memcpy(copy, text.c_str(), text.size() + 1);
		return copy;
	}
}

CgiProcess::CgiProcess()
	: _inputFd(-1),
	  _outputFd(-1),
	  _pid(-1),
	  _state(RUNNING),
	  _status(200),
	  _reaped(false),
	  _inputOffset(0),
	  _startedAt(0)
{
}

CgiProcess::~CgiProcess()
{
	// A destructor that leaked the pipes or left the child running would turn
	// every aborted request into a permanent fd and process leak.
	closeInput();
	closeOutput();
	terminate();
}

int	CgiProcess::inputFd(void) const	{ return _inputFd; }
int	CgiProcess::outputFd(void) const{ return _outputFd; }
pid_t	CgiProcess::pid(void) const	{ return _pid; }
CgiProcess::State	CgiProcess::state(void) const	{ return _state; }
int	CgiProcess::status(void) const	{ return _status; }

bool	CgiProcess::wantsWrite(void) const
{
	return _inputFd != -1 && _inputOffset < _pendingInput.size();
}

void	CgiProcess::closeInput(void)
{
	if (_inputFd != -1)
	{
		close(_inputFd);
		_inputFd = -1;
	}
}

void	CgiProcess::closeOutput(void)
{
	if (_outputFd != -1)
	{
		close(_outputFd);
		_outputFd = -1;
	}
}

std::map<std::string, std::string>	CgiProcess::buildEnvironment(
	const HttpRequest& request,
	const ServerConfig& server,
	const LocationConfig& location,
	const std::string& scriptPath,
	const std::string& pathInfo)
{
	std::map<std::string, std::string>	env;

	(void)location;

	env["GATEWAY_INTERFACE"] = "CGI/1.1";
	env["SERVER_SOFTWARE"] = "webserv/1.0";
	env["SERVER_PROTOCOL"] = "HTTP/1.1";
	env["SERVER_NAME"] = server.serverNames.empty() ? server.host : server.serverNames[0];
	env["SERVER_PORT"] = server.port;
	env["REQUEST_METHOD"] = request.method();
	env["REQUEST_URI"] = request.target();
	env["SCRIPT_NAME"] = request.path();
	env["SCRIPT_FILENAME"] = scriptPath;
	env["PATH_INFO"] = pathInfo;
	env["QUERY_STRING"] = request.query();
	env["DOCUMENT_ROOT"] = server.root;
	env["REDIRECT_STATUS"] = "200";	// php-cgi refuses to run without it

	if (!pathInfo.empty())
		env["PATH_TRANSLATED"] = Utils::joinPath(server.root, pathInfo);

	if (request.hasHeader("content-length"))
		env["CONTENT_LENGTH"] = request.header("content-length");
	else if (!request.body().empty())
		// A chunked body arrives without Content-Length, but the script still
		// needs to know how much to read from stdin.
		env["CONTENT_LENGTH"] = Utils::toString(request.body().size());

	if (request.hasHeader("content-type"))
		env["CONTENT_TYPE"] = request.header("content-type");

	const std::map<std::string, std::string>&	headers = request.headers();
	for (std::map<std::string, std::string>::const_iterator it = headers.begin();
		 it != headers.end(); ++it)
	{
		// These two are passed in their unprefixed CGI form above; duplicating
		// them as HTTP_* is redundant and confuses some scripts.
		if (it->first == "content-length" || it->first == "content-type")
			continue ;
		env[toEnvName(it->first)] = it->second;
	}

	return env;
}

bool	CgiProcess::start(const HttpRequest& request,
						  const ServerConfig& server,
						  const LocationConfig& location,
						  const std::string& scriptPath,
						  const std::string& pathInfo)
{
	if (!Utils::isRegularFile(scriptPath))
	{
		_state = FAILED;
		_status = 404;
		return false;
	}

	// The child changes directory so the script can use relative paths, which
	// means the path handed to execve must be absolute: a relative one would be
	// resolved against the new working directory and no longer exist.
	char	resolved[PATH_MAX];
	if (realpath(scriptPath.c_str(), resolved) == NULL)
	{
		Logger::error("cannot resolve CGI script path: " + scriptPath);
		_state = FAILED;
		_status = 404;
		return false;
	}
	const std::string	absoluteScript(resolved);
	if (access(location.cgiInterpreter.c_str(), X_OK) != 0)
	{
		Logger::error("CGI interpreter is not executable: " + location.cgiInterpreter);
		_state = FAILED;
		_status = 500;
		return false;
	}

	int	inputPipe[2];
	int	outputPipe[2];

	if (pipe(inputPipe) == -1)
	{
		Logger::error("pipe() failed for CGI input: " + std::string(strerror(errno)));
		_state = FAILED;
		_status = 500;
		return false;
	}
	if (pipe(outputPipe) == -1)
	{
		Logger::error("pipe() failed for CGI output: " + std::string(strerror(errno)));
		close(inputPipe[0]);
		close(inputPipe[1]);
		_state = FAILED;
		_status = 500;
		return false;
	}

	std::map<std::string, std::string>	env = buildEnvironment(
		request, server, location, absoluteScript, pathInfo);

	// The execve arrays are built before fork() because allocation in the child
	// of a multi-threaded process is not async-signal-safe.
	std::vector<char*>	argv;
	argv.push_back(duplicate(location.cgiInterpreter));
	argv.push_back(duplicate(absoluteScript));
	argv.push_back(NULL);

	std::vector<char*>	envp;
	for (std::map<std::string, std::string>::const_iterator it = env.begin();
		 it != env.end(); ++it)
		envp.push_back(duplicate(it->first + "=" + it->second));
	envp.push_back(NULL);

	_pid = fork();
	if (_pid == -1)
	{
		Logger::error("fork() failed for CGI: " + std::string(strerror(errno)));
		close(inputPipe[0]);
		close(inputPipe[1]);
		close(outputPipe[0]);
		close(outputPipe[1]);
		releaseArray(argv);
		releaseArray(envp);
		_state = FAILED;
		_status = 500;
		return false;
	}

	if (_pid == 0)
	{
		// --- child ---------------------------------------------------------
		close(inputPipe[1]);
		close(outputPipe[0]);

		if (dup2(inputPipe[0], STDIN_FILENO) == -1)
			_exit(1);
		if (dup2(outputPipe[1], STDOUT_FILENO) == -1)
			_exit(1);
		close(inputPipe[0]);
		close(outputPipe[1]);

		// The parent ignores SIGPIPE, and that disposition survives execve;
		// restoring the default means a script writing to a closed pipe dies
		// normally instead of looping on EPIPE.
		signal(SIGPIPE, SIG_DFL);

		// Scripts routinely use relative paths against their own directory.
		std::string	directory = absoluteScript.substr(0, absoluteScript.find_last_of('/'));
		if (!directory.empty())
		{
			if (chdir(directory.c_str()) != 0)
				_exit(1);
		}

		execve(argv[0], &argv[0], &envp[0]);
		// Only reachable if execve failed; _exit avoids flushing the parent's
		// stdio buffers a second time.
		_exit(1);
	}

	// --- parent ------------------------------------------------------------
	close(inputPipe[0]);
	close(outputPipe[1]);
	releaseArray(argv);
	releaseArray(envp);

	_inputFd = inputPipe[1];
	_outputFd = outputPipe[0];

	if (!setNonBlocking(_inputFd) || !setNonBlocking(_outputFd))
	{
		Logger::error("could not set CGI pipes non-blocking");
		terminate();
		_state = FAILED;
		_status = 500;
		return false;
	}

	_pendingInput = request.body();
	_inputOffset = 0;
	_startedAt = std::time(NULL);
	_state = RUNNING;

	// With no body to send there is nothing to wait for on stdin, and closing
	// it now is what lets a script blocked on read() proceed.
	if (_pendingInput.empty())
		closeInput();

	return true;
}

bool	CgiProcess::writeChunk(void)
{
	if (_inputFd == -1)
		return true;

	size_t	remaining = _pendingInput.size() - _inputOffset;
	if (remaining == 0)
	{
		closeInput();
		return true;
	}

	ssize_t	written = write(_inputFd, _pendingInput.data() + _inputOffset, remaining);
	if (written > 0)
	{
		_inputOffset += static_cast<size_t>(written);
		if (_inputOffset >= _pendingInput.size())
			closeInput();
		return true;
	}
	if (written == -1)
	{
		// A script that exits without reading its body closes the pipe. That is
		// legitimate, so stop writing rather than failing the request.
		closeInput();
		return true;
	}
	return true;
}

bool	CgiProcess::readChunk(void)
{
	if (_outputFd == -1)
		return true;

	char	buffer[CGI_READ_BUFFER];
	ssize_t	count = read(_outputFd, buffer, sizeof(buffer));

	if (count > 0)
	{
		if (_output.size() + static_cast<size_t>(count) > CGI_MAX_OUTPUT)
		{
			Logger::warn("CGI output exceeded the size cap; killing the script");
			terminate();
			closeOutput();
			_state = FAILED;
			_status = 502;
			return false;
		}
		_output.append(buffer, static_cast<size_t>(count));
		return true;
	}

	if (count == 0)
	{
		// EOF: the child closed stdout, so its output is complete.
		closeOutput();
		_state = FINISHED;
		return true;
	}

	// read() returned -1. Because poll() said the fd was ready, this is a real
	// pipe error rather than "no data yet". Appending here with a negative
	// count is precisely the crash the original readStdout() had.
	closeOutput();
	_state = FAILED;
	_status = 502;
	return false;
}

bool	CgiProcess::reap(void)
{
	if (_pid <= 0 || _reaped)
		return true;

	int		status = 0;
	pid_t	result = waitpid(_pid, &status, WNOHANG);

	if (result == 0)
		return false;		// still running
	if (result == -1)
	{
		_reaped = true;		// already collected, or never existed
		return true;
	}

	_reaped = true;
	if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
	{
		Logger::warn("CGI script exited with code "
			+ Utils::toString(static_cast<long>(WEXITSTATUS(status))));
		if (_output.empty())
		{
			_state = FAILED;
			_status = 502;
		}
	}
	else if (WIFSIGNALED(status))
	{
		Logger::warn("CGI script was killed by signal "
			+ Utils::toString(static_cast<long>(WTERMSIG(status))));
		if (_output.empty())
		{
			_state = FAILED;
			_status = 502;
		}
	}
	return true;
}

bool	CgiProcess::hasTimedOut(time_t now, long limitSeconds) const
{
	if (_startedAt == 0)
		return false;
	return (now - _startedAt) >= limitSeconds;
}

void	CgiProcess::terminate(void)
{
	if (_pid <= 0 || _reaped)
		return ;

	// SIGKILL rather than SIGTERM: a script that installed a handler could
	// otherwise ignore the request to stop and keep the slot occupied.
	kill(_pid, SIGKILL);

	int	status = 0;
	// The child is guaranteed to die, so this blocking wait cannot hang, and it
	// is what stops killed scripts from becoming zombies.
	waitpid(_pid, &status, 0);
	_reaped = true;
}

bool	CgiProcess::buildResponse(HttpResponse& response) const
{
	// A CGI script emits its own header block, terminated by a blank line.
	// Both CRLF and bare LF appear in the wild, so accept whichever comes first.
	size_t	separator = std::string::npos;
	size_t	skip = 0;

	size_t	crlf = _output.find("\r\n\r\n");
	size_t	lf = _output.find("\n\n");

	if (crlf != std::string::npos && (lf == std::string::npos || crlf <= lf))
	{
		separator = crlf;
		skip = 4;
	}
	else if (lf != std::string::npos)
	{
		separator = lf;
		skip = 2;
	}

	if (separator == std::string::npos)
	{
		// No header block at all. Treating the whole output as a body would
		// invent a Content-Type the script never chose, so this is a 502.
		Logger::warn("CGI output contained no header block");
		return false;
	}

	const std::string	headerBlock = _output.substr(0, separator);
	const std::string	body = _output.substr(separator + skip);

	int			statusCode = 200;
	std::string	contentType;
	std::string	location;
	std::map<std::string, std::string>	extraHeaders;

	std::vector<std::string>	lines = Utils::split(headerBlock, "\n");
	for (size_t i = 0; i < lines.size(); ++i)
	{
		std::string	line = Utils::trim(lines[i]);
		if (line.empty())
			continue ;

		size_t	colon = line.find(':');
		if (colon == std::string::npos)
			continue ;

		std::string	name = Utils::trim(line.substr(0, colon));
		std::string	value = Utils::trim(line.substr(colon + 1));
		std::string	lowered = Utils::toLower(name);

		if (lowered == "status")
		{
			// "Status: 404 Not Found" — only the numeric part matters.
			size_t	space = value.find(' ');
			std::string	codeText = (space == std::string::npos) ? value : value.substr(0, space);
			long	parsed = 0;
			if (Utils::parseLong(codeText, parsed) && parsed >= 100 && parsed <= 599)
				statusCode = static_cast<int>(parsed);
		}
		else if (lowered == "content-type")
			contentType = value;
		else if (lowered == "location")
			location = value;
		else if (lowered == "content-length")
			// Recomputed from the actual body, so a script that miscounts
			// cannot desynchronise the connection.
			continue ;
		else
			extraHeaders[name] = value;
	}

	// A Location header with no explicit Status means a 302 redirect.
	if (!location.empty() && statusCode == 200)
		statusCode = 302;

	response.setStatus(statusCode);
	if (!location.empty())
		response.setHeader("Location", location);
	for (std::map<std::string, std::string>::const_iterator it = extraHeaders.begin();
		 it != extraHeaders.end(); ++it)
		response.setHeader(it->first, it->second);

	response.setBody(body, contentType.empty() ? "text/html; charset=utf-8" : contentType);
	return true;
}
