#include "Connection.hpp"
#include "Config.hpp"
#include "Logger.hpp"
#include "RequestHandler.hpp"
#include "Utils.hpp"

#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace
{
	const size_t	READ_BUFFER_SIZE = 65536;

	// Bounds how long one TCP connection may be reused. Without it a single
	// client could hold a slot open indefinitely.
	const size_t	MAX_REQUESTS_PER_CONNECTION = 100;
}

Connection::Connection(int fd, const ServerConfig& server)
	: _fd(fd),
	  _server(&server),
	  _state(READING_REQUEST),
	  _outputOffset(0),
	  _interimOffset(0),
	  _continueSent(false),
	  _cgi(NULL),
	  _keepAlive(true),
	  _headOnly(false),
	  _bodyLimitApplied(false),
	  _lastActivity(std::time(NULL)),
	  _requestsServed(0)
{
	_request.setMaxBodySize(server.maxBodySize);
}

Connection::~Connection()
{
	// The CGI child and its pipes belong to this connection; deleting the
	// process object here is what guarantees they go away with it.
	delete _cgi;
}

int	Connection::fd(void) const	{ return _fd; }
Connection::State	Connection::state(void) const	{ return _state; }
const ServerConfig&	Connection::server(void) const	{ return *_server; }
CgiProcess*	Connection::cgi(void)	{ return _cgi; }
time_t	Connection::lastActivity(void) const	{ return _lastActivity; }
bool	Connection::shouldKeepAlive(void) const	{ return _keepAlive; }

void	Connection::touch(void)
{
	_lastActivity = std::time(NULL);
}

bool	Connection::isIdle(void) const
{
	// Between requests: nothing parsed yet and nothing left to write.
	return _state == READING_REQUEST && _outputOffset >= _outputBuffer.size();
}

bool	Connection::onReadable(void)
{
	char	buffer[READ_BUFFER_SIZE];
	ssize_t	count = recv(_fd, buffer, sizeof(buffer), 0);

	if (count == 0)
	{
		// An orderly shutdown by the peer.
		_state = CLOSING;
		return false;
	}
	if (count < 0)
	{
		// poll() reported readability, so this is a real error. Checking errno
		// after a failed socket call is not permitted by the project rules, and
		// it is unnecessary: the fd is unusable either way.
		_state = CLOSING;
		return false;
	}

	touch();

	_request.consume(buffer, static_cast<size_t>(count));

	// Applied as soon as the headers name a path, so an oversized body is
	// rejected while it streams rather than after it has all been buffered.
	applyLocationBodyLimit();

	// The client is holding its body back until it hears that the request is
	// acceptable. Without this it waits out its own timeout, typically a full
	// second, before sending anything, so every large upload starts late.
	if (!_continueSent && !_request.hasFailed() && !_request.isComplete()
		&& _request.headersParsed() && _request.expectsContinue())
	{
		_continueSent = true;
		_interimBuffer = "HTTP/1.1 100 Continue\r\n\r\n";
		_interimOffset = 0;
	}

	if (_request.hasFailed())
	{
		// A malformed request poisons the byte stream, so the connection
		// cannot be reused even to report the error.
		_keepAlive = false;
		sendError(_request.statusCode());
		return true;
	}

	if (_request.isComplete())
		dispatch();
	return true;
}

bool	Connection::hasPendingInterim(void) const
{
	return _interimOffset < _interimBuffer.size();
}

bool	Connection::flushInterim(void)
{
	if (!hasPendingInterim())
		return true;

	size_t	remaining = _interimBuffer.size() - _interimOffset;
	ssize_t	written = send(_fd, _interimBuffer.data() + _interimOffset, remaining, 0);

	if (written <= 0)
	{
		_state = CLOSING;
		return false;
	}
	touch();
	_interimOffset += static_cast<size_t>(written);
	if (!hasPendingInterim())
	{
		// Cleared rather than kept, so that a second request on this connection
		// starts with an empty queue.
		_interimBuffer.clear();
		_interimOffset = 0;
	}
	return true;
}

void	Connection::applyLocationBodyLimit(void)
{
	if (_bodyLimitApplied || !_request.headersParsed() || _request.hasFailed())
		return ;

	_bodyLimitApplied = true;

	const LocationConfig*	location = _server->matchLocation(_request.path());
	if (location && location->maxBodySize > 0)
		_request.setMaxBodySize(location->maxBodySize);
}

void	Connection::dispatch(void)
{
	_keepAlive = _request.wantsKeepAlive();
	_headOnly = (_request.method() == "HEAD");

	++_requestsServed;
	if (_requestsServed >= MAX_REQUESTS_PER_CONNECTION)
		_keepAlive = false;

	HttpResponse	response;
	RequestHandler::Result	result = RequestHandler::route(_request, *_server, response);

	if (result.status != 0)
	{
		sendError(result.status, result.allowHeader);
		return ;
	}

	if (result.isCgi)
	{
		_cgi = new CgiProcess();
		if (!_cgi->start(_request, *_server, *result.location,
						 result.scriptPath, result.pathInfo))
		{
			int	status = _cgi->status();
			delete _cgi;
			_cgi = NULL;
			sendError(status);
			return ;
		}
		_state = RUNNING_CGI;
		return ;
	}

	sendResponse(response);
}

void	Connection::onCgiReadable(void)
{
	if (!_cgi)
		return ;
	touch();
	_cgi->readChunk();
}

void	Connection::onCgiWritable(void)
{
	if (!_cgi)
		return ;
	touch();
	_cgi->writeChunk();
}

void	Connection::finishCgi(void)
{
	if (!_cgi)
		return ;

	HttpResponse	response;

	if (_cgi->state() == CgiProcess::FAILED)
	{
		int	status = _cgi->status();
		delete _cgi;
		_cgi = NULL;
		sendError(status);
		return ;
	}

	if (!_cgi->buildResponse(response))
	{
		delete _cgi;
		_cgi = NULL;
		// The script ran but produced something that is not a CGI response,
		// which is exactly what 502 describes.
		sendError(502);
		return ;
	}

	delete _cgi;
	_cgi = NULL;
	sendResponse(response);
}

void	Connection::sendResponse(HttpResponse& response)
{
	response.setKeepAlive(_keepAlive);
	response.setHeadOnly(_headOnly);

	_outputBuffer = response.serialise();
	_outputOffset = 0;
	_state = WRITING_RESPONSE;

	Logger::info(_request.method() + " " + _request.target() + " -> "
		+ Utils::toString(static_cast<long>(response.status())));
}

void	Connection::sendError(int status, const std::string& allowHeader)
{
	HttpResponse	response(status);

	RequestHandler::buildError(status, *_server, response);

	// A 405 must state which methods the resource does accept.
	if (!allowHeader.empty())
		response.setHeader("Allow", allowHeader);

	// Any 4xx or 5xx that reflects a broken byte stream, rather than a
	// well-formed request the server declined, must close the connection.
	if (status == 400 || status == 408 || status == 413 || status == 414
		|| status == 431 || status == 500 || status == 501 || status == 505)
		_keepAlive = false;

	response.setKeepAlive(_keepAlive);
	response.setHeadOnly(_headOnly);

	_outputBuffer = response.serialise();
	_outputOffset = 0;
	_state = WRITING_RESPONSE;

	Logger::warn(_request.method().empty() ? ("-> " + Utils::toString(static_cast<long>(status)))
		: (_request.method() + " " + _request.target() + " -> "
			+ Utils::toString(static_cast<long>(status))));
}

bool	Connection::onWritable(void)
{
	if (_outputOffset >= _outputBuffer.size())
	{
		recycle();
		return _state != CLOSING;
	}

	size_t	remaining = _outputBuffer.size() - _outputOffset;
	ssize_t	written = send(_fd, _outputBuffer.data() + _outputOffset, remaining, 0);

	if (written <= 0)
	{
		// Either the peer went away or the socket failed. Both mean the
		// response cannot be delivered.
		_state = CLOSING;
		return false;
	}

	touch();
	_outputOffset += static_cast<size_t>(written);

	// A short write is normal once the socket buffer fills: the remainder stays
	// queued and the event loop will call back when there is room. Ignoring
	// this return value, as the original did, silently truncates large files.
	if (_outputOffset < _outputBuffer.size())
		return true;

	recycle();
	return _state != CLOSING;
}

void	Connection::recycle(void)
{
	if (!_keepAlive)
	{
		_state = CLOSING;
		return ;
	}

	_outputBuffer.clear();
	_outputOffset = 0;
	_headOnly = false;
	_interimBuffer.clear();
	_interimOffset = 0;
	_continueSent = false;
	// The next request on this connection may match a different location, so
	// the narrowed limit must not carry over.
	_bodyLimitApplied = false;

	// reset() preserves any bytes of the next pipelined request that already
	// arrived, so a client sending back-to-back requests is served correctly.
	_request.reset();
	_request.setMaxBodySize(_server->maxBodySize);
	_state = READING_REQUEST;

	// Those carried-over bytes may already form a whole request, in which case
	// no further readability event is coming and it must be handled now.
	_request.consume(NULL, 0);
	applyLocationBodyLimit();
	if (_request.hasFailed())
	{
		_keepAlive = false;
		sendError(_request.statusCode());
	}
	else if (_request.isComplete())
		dispatch();
}
