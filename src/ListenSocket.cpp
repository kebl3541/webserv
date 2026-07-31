#include "ListenSocket.hpp"
#include "Config.hpp"
#include "Logger.hpp"

#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>

ListenSocket::ListenSocket()
	: _fd(-1),
	  _config(NULL)
{
}

ListenSocket::~ListenSocket()
{
	closeSocket();
}

int	ListenSocket::fd(void) const
{
	return _fd;
}

const ServerConfig*	ListenSocket::config(void) const
{
	return _config;
}

void	ListenSocket::closeSocket(void)
{
	if (_fd != -1)
	{
		close(_fd);
		_fd = -1;
	}
}

bool	ListenSocket::open(const ServerConfig& server)
{
	struct addrinfo	hints;
	struct addrinfo*	results = NULL;

	std::memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	const char*	host = server.host.empty() ? NULL : server.host.c_str();
	int			error = getaddrinfo(host, server.port.c_str(), &hints, &results);

	// getaddrinfo reports failure through its own return code, not errno, so
	// gai_strerror is the only correct way to describe it.
	if (error != 0)
	{
		Logger::error("getaddrinfo(" + server.host + ":" + server.port + "): "
			+ gai_strerror(error));
		return false;
	}

	int	fd = socket(results->ai_family, results->ai_socktype, results->ai_protocol);
	if (fd == -1)
	{
		Logger::error("socket(): " + std::string(strerror(errno)));
		freeaddrinfo(results);
		return false;
	}

	// SO_REUSEADDR lets the server restart immediately while old connections
	// are still in TIME_WAIT, instead of failing to bind for a couple of minutes.
	int	enable = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) == -1)
	{
		Logger::error("setsockopt(SO_REUSEADDR): " + std::string(strerror(errno)));
		close(fd);
		freeaddrinfo(results);
		return false;
	}

	// SOCK_NONBLOCK cannot be OR-ed into the socket type here: it is a Linux
	// extension and does not exist on macOS or the BSDs. fcntl is portable.
	int	flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
	{
		Logger::error("fcntl(O_NONBLOCK): " + std::string(strerror(errno)));
		close(fd);
		freeaddrinfo(results);
		return false;
	}

	if (bind(fd, results->ai_addr, results->ai_addrlen) == -1)
	{
		Logger::error("bind(" + server.host + ":" + server.port + "): "
			+ std::string(strerror(errno)));
		close(fd);
		freeaddrinfo(results);
		return false;
	}
	freeaddrinfo(results);

	// The backlog is the queue of completed handshakes waiting to be accepted;
	// SOMAXCONN is whatever the kernel considers its maximum.
	if (listen(fd, SOMAXCONN) == -1)
	{
		Logger::error("listen(): " + std::string(strerror(errno)));
		close(fd);
		return false;
	}

	_fd = fd;
	_config = &server;
	Logger::info("listening on " + server.host + ":" + server.port);
	return true;
}
