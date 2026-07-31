#include "ListenSocket.hpp"
#include "Config.hpp"
#include "Logger.hpp"

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>

namespace
{
	// Renders a resolved address as "host:port" for logging, so the log says
	// which of a name's several addresses was actually bound.
	std::string	describeAddress(const struct addrinfo* address, const std::string& port)
	{
		char	text[INET6_ADDRSTRLEN];

		std::memset(text, 0, sizeof(text));

		if (address->ai_family == AF_INET)
		{
			const struct sockaddr_in*	v4
				= reinterpret_cast<const struct sockaddr_in*>(address->ai_addr);
			if (!inet_ntop(AF_INET, &v4->sin_addr, text, sizeof(text)))
				return "?:" + port;
			return std::string(text) + ":" + port;
		}
		if (address->ai_family == AF_INET6)
		{
			const struct sockaddr_in6*	v6
				= reinterpret_cast<const struct sockaddr_in6*>(address->ai_addr);
			if (!inet_ntop(AF_INET6, &v6->sin6_addr, text, sizeof(text)))
				return "?:" + port;
			// Bracket notation, because a bare IPv6 address next to a colon and
			// a port is ambiguous.
			return "[" + std::string(text) + "]:" + port;
		}
		return "?:" + port;
	}
}

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

const std::string&	ListenSocket::describe(void) const
{
	return _description;
}

void	ListenSocket::closeSocket(void)
{
	if (_fd != -1)
	{
		close(_fd);
		_fd = -1;
	}
}

bool	ListenSocket::openOne(const ServerConfig& server, const struct addrinfo* address)
{
	int	fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);

	if (fd == -1)
	{
		Logger::warn("socket(): " + std::string(strerror(errno)));
		return false;
	}

	// SO_REUSEADDR lets the server restart immediately while old connections
	// are still in TIME_WAIT, instead of failing to bind for a couple of minutes.
	int	enable = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) == -1)
	{
		Logger::warn("setsockopt(SO_REUSEADDR): " + std::string(strerror(errno)));
		close(fd);
		return false;
	}

	// An IPv6 socket accepts IPv4 traffic by default on some systems. That
	// would make the separate IPv4 bind for the same port fail as already in
	// use, so each socket is confined to its own family and the two coexist.
	if (address->ai_family == AF_INET6)
	{
		int	v6only = 1;
		if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) == -1)
		{
			Logger::warn("setsockopt(IPV6_V6ONLY): " + std::string(strerror(errno)));
			close(fd);
			return false;
		}
	}

	// SOCK_NONBLOCK cannot be OR-ed into the socket type here: it is a Linux
	// extension and does not exist on macOS or the BSDs. fcntl is portable.
	int	flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
	{
		Logger::warn("fcntl(O_NONBLOCK): " + std::string(strerror(errno)));
		close(fd);
		return false;
	}

	const std::string	description = describeAddress(address, server.port);

	if (bind(fd, address->ai_addr, address->ai_addrlen) == -1)
	{
		Logger::warn("bind(" + description + "): " + std::string(strerror(errno)));
		close(fd);
		return false;
	}

	// The backlog is the queue of completed handshakes waiting to be accepted;
	// SOMAXCONN is whatever the kernel considers its maximum.
	if (listen(fd, SOMAXCONN) == -1)
	{
		Logger::warn("listen(" + description + "): " + std::string(strerror(errno)));
		close(fd);
		return false;
	}

	_fd = fd;
	_config = &server;
	_description = description;
	return true;
}

bool	ListenSocket::openAll(const ServerConfig& server,
							  std::vector<ListenSocket*>& out)
{
	struct addrinfo		hints;
	struct addrinfo*	results = NULL;

	std::memset(&hints, 0, sizeof(hints));
	// AF_UNSPEC rather than AF_INET, so that a name resolving to both an IPv6
	// and an IPv4 address yields both and each one gets its own listener.
	hints.ai_family = AF_UNSPEC;
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

	size_t	bound = 0;
	for (struct addrinfo* candidate = results;
		 candidate != NULL;
		 candidate = candidate->ai_next)
	{
		ListenSocket*	listener = new ListenSocket();

		if (!listener->openOne(server, candidate))
		{
			// One address of several failing is normal: a host may advertise an
			// IPv6 address on a machine with IPv6 disabled. It is only fatal
			// when nothing at all could be bound.
			delete listener;
			continue ;
		}
		Logger::info("listening on " + listener->describe());
		out.push_back(listener);
		++bound;
	}
	freeaddrinfo(results);

	if (bound == 0)
	{
		Logger::error("could not bind any address for "
			+ server.host + ":" + server.port);
		return false;
	}
	return true;
}
