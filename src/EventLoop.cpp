#include "EventLoop.hpp"
#include "CgiProcess.hpp"
#include "Config.hpp"
#include "Connection.hpp"
#include "ListenSocket.hpp"
#include "Logger.hpp"
#include "Utils.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>

volatile bool	EventLoop::_stopRequested = false;

namespace
{
	const int	POLL_TIMEOUT_MS = 1000;

	// Bounds concurrent clients so the process cannot exhaust its descriptor
	// limit and start failing accept() for everyone.
	const size_t	MAX_CONNECTIONS = 512;

	// How many clients to take per readability event on a listening socket.
	// Draining in a bounded batch keeps one busy listener from starving others.
	const int	ACCEPT_BATCH = 32;
}

EventLoop::FdEntry::FdEntry()
	: role(ROLE_CLIENT),
	  ownerFd(-1),
	  index(0)
{
}

EventLoop::EventLoop(const Config& config)
	: _config(config),
	  _clientTimeout(65),
	  _cgiTimeout(10)
{
}

EventLoop::~EventLoop()
{
	for (std::map<int, Connection*>::iterator it = _connections.begin();
		 it != _connections.end(); ++it)
	{
		close(it->first);
		delete it->second;
	}
	_connections.clear();

	for (size_t i = 0; i < _listeners.size(); ++i)
		delete _listeners[i];
	_listeners.clear();
}

void	EventLoop::requestStop(void)
{
	// The only thing a signal handler does is set this flag; everything else
	// happens back on the main path where it is safe.
	_stopRequested = true;
}

bool	EventLoop::setup(void)
{
	const std::vector<ServerConfig>&	servers = _config.servers();

	for (size_t i = 0; i < servers.size(); ++i)
	{
		const size_t	before = _listeners.size();

		// A server block can yield several listeners, one per address its host
		// resolves to, so that a name covering both IPv6 and IPv4 is served on
		// both rather than only on whichever the resolver happened to return.
		if (!ListenSocket::openAll(servers[i], _listeners))
			// An endpoint failing to bind entirely is fatal: silently serving a
			// subset of the configuration would be worse than refusing to start.
			return false;

		for (size_t j = before; j < _listeners.size(); ++j)
		{
			FdEntry	entry;
			entry.role = ROLE_LISTEN;
			entry.index = j;
			_registry[_listeners[j]->fd()] = entry;
		}
	}

	return !_listeners.empty();
}

void	EventLoop::buildPollSet(std::vector<struct pollfd>& pollSet)
{
	pollSet.clear();
	pollSet.reserve(_registry.size());

	for (std::map<int, FdEntry>::const_iterator it = _registry.begin();
		 it != _registry.end(); ++it)
	{
		struct pollfd	entry;
		entry.fd = it->first;
		entry.revents = 0;

		switch (it->second.role)
		{
			case ROLE_LISTEN:
				entry.events = POLLIN;
				break ;

			case ROLE_CLIENT:
			{
				std::map<int, Connection*>::const_iterator	found
					= _connections.find(it->first);
				if (found == _connections.end())
					continue ;

				// Interest is derived from the connection's state, so the loop
				// never asks for writability on a socket with nothing to write.
				switch (found->second->state())
				{
					case Connection::READING_REQUEST:
						entry.events = POLLIN;
						// A queued 100 Continue has to go out while the
						// connection is still reading, so writability is added
						// on top of readability rather than replacing it.
						if (found->second->hasPendingInterim())
							entry.events |= POLLOUT;
						break ;
					case Connection::WRITING_RESPONSE:
						entry.events = POLLOUT;
						break ;
					case Connection::RUNNING_CGI:
						// The child owns the response; watching the socket for
						// readability here would spin on a half-closed peer.
						continue ;
					case Connection::CLOSING:
						continue ;
				}
				break ;
			}

			case ROLE_CGI_IN:
				entry.events = POLLOUT;
				break ;

			case ROLE_CGI_OUT:
				entry.events = POLLIN;
				break ;
		}
		pollSet.push_back(entry);
	}
}

int	EventLoop::run(void)
{
	std::vector<struct pollfd>	pollSet;

	while (!_stopRequested)
	{
		// Rebuilding here, before any handler runs, is what makes it safe for
		// handlers to add and remove descriptors: they mutate _registry, never
		// the vector being walked.
		buildPollSet(pollSet);

		if (pollSet.empty())
		{
			Logger::error("no descriptors left to poll");
			break ;
		}

		int	ready = poll(&pollSet[0], pollSet.size(), POLL_TIMEOUT_MS);

		if (ready == -1)
		{
			// A signal interrupting poll() is expected, not an error.
			if (_stopRequested)
				break ;
			Logger::error("poll(): " + std::string(strerror(errno)));
			continue ;
		}

		if (ready > 0)
		{
			// The snapshot is iterated by index and never resized, so handlers
			// adding descriptors cannot invalidate this walk.
			for (size_t i = 0; i < pollSet.size(); ++i)
			{
				if (pollSet[i].revents == 0)
					continue ;
				handleEvent(pollSet[i]);
			}
		}

		pollCgi();
		enforceTimeouts();
		reapClosed();
	}

	Logger::info("shutting down");
	return 0;
}

void	EventLoop::handleEvent(const struct pollfd& entry)
{
	std::map<int, FdEntry>::const_iterator	found = _registry.find(entry.fd);

	// The descriptor may have been closed by an earlier event in this same
	// batch, which is why every handler re-checks the registry.
	if (found == _registry.end())
		return ;

	const FdEntry	info = found->second;

	switch (info.role)
	{
		case ROLE_LISTEN:
			if (entry.revents & POLLIN)
				acceptClients(info.index);
			return ;

		case ROLE_CLIENT:
		{
			std::map<int, Connection*>::iterator	client = _connections.find(entry.fd);
			if (client == _connections.end())
				return ;

			// POLLHUP on a client socket still allows draining whatever is
			// already buffered, but POLLERR and POLLNVAL never do.
			if (entry.revents & (POLLERR | POLLNVAL))
			{
				scheduleClose(entry.fd);
				return ;
			}

			bool	alive = true;
			// The interim reply is drained first: the client is waiting on it
			// before it will send the body this connection is reading for.
			if ((entry.revents & POLLOUT) && client->second->hasPendingInterim())
				alive = client->second->flushInterim();
			else if (entry.revents & POLLIN)
				alive = client->second->onReadable();
			else if (entry.revents & POLLOUT)
				alive = client->second->onWritable();
			else if (entry.revents & POLLHUP)
				alive = false;

			if (!alive || client->second->state() == Connection::CLOSING)
			{
				scheduleClose(entry.fd);
				return ;
			}

			// Starting a CGI child adds two more descriptors to watch.
			if (client->second->state() == Connection::RUNNING_CGI)
			{
				CgiProcess*	cgi = client->second->cgi();
				if (cgi)
				{
					if (cgi->inputFd() != -1 && _registry.find(cgi->inputFd()) == _registry.end())
					{
						FdEntry	pipeEntry;
						pipeEntry.role = ROLE_CGI_IN;
						pipeEntry.ownerFd = entry.fd;
						_registry[cgi->inputFd()] = pipeEntry;
					}
					if (cgi->outputFd() != -1 && _registry.find(cgi->outputFd()) == _registry.end())
					{
						FdEntry	pipeEntry;
						pipeEntry.role = ROLE_CGI_OUT;
						pipeEntry.ownerFd = entry.fd;
						_registry[cgi->outputFd()] = pipeEntry;
					}
				}
			}
			return ;
		}

		case ROLE_CGI_IN:
		{
			std::map<int, Connection*>::iterator	owner = _connections.find(info.ownerFd);
			if (owner == _connections.end())
			{
				_registry.erase(entry.fd);
				return ;
			}
			owner->second->onCgiWritable();

			CgiProcess*	cgi = owner->second->cgi();
			// The pipe closes once the body has been handed over.
			if (!cgi || cgi->inputFd() == -1)
				_registry.erase(entry.fd);
			return ;
		}

		case ROLE_CGI_OUT:
		{
			std::map<int, Connection*>::iterator	owner = _connections.find(info.ownerFd);
			if (owner == _connections.end())
			{
				_registry.erase(entry.fd);
				return ;
			}
			owner->second->onCgiReadable();

			CgiProcess*	cgi = owner->second->cgi();
			if (!cgi || cgi->outputFd() == -1)
			{
				// EOF on stdout: stop watching, then let pollCgi() collect the
				// child and turn its output into a response.
				_registry.erase(entry.fd);
			}
			return ;
		}
	}
}

void	EventLoop::acceptClients(size_t listenerIndex)
{
	if (listenerIndex >= _listeners.size())
		return ;

	ListenSocket*	listener = _listeners[listenerIndex];

	// A level-triggered poll() would report the listener ready once per pending
	// connection anyway, but draining in a batch cuts the number of syscalls.
	for (int taken = 0; taken < ACCEPT_BATCH; ++taken)
	{
		struct sockaddr_storage	address;
		socklen_t				addressSize = sizeof(address);

		int	clientFd = accept(listener->fd(),
			reinterpret_cast<struct sockaddr*>(&address), &addressSize);

		if (clientFd == -1)
			// Either the queue is empty or accept failed; in both cases the
			// right move is to stop and wait for the next event.
			return ;

		if (_connections.size() >= MAX_CONNECTIONS)
		{
			Logger::warn("connection limit reached; rejecting a client");
			close(clientFd);
			return ;
		}

		int	flags = fcntl(clientFd, F_GETFL, 0);
		if (flags == -1 || fcntl(clientFd, F_SETFL, flags | O_NONBLOCK) == -1)
		{
			Logger::error("cannot set client socket non-blocking");
			close(clientFd);
			continue ;
		}

		Connection*	connection = new Connection(clientFd, *listener->config());
		_connections[clientFd] = connection;

		FdEntry	entry;
		entry.role = ROLE_CLIENT;
		_registry[clientFd] = entry;

		Logger::debug("accepted client on fd " + Utils::toString(static_cast<long>(clientFd)));
	}
}

void	EventLoop::pollCgi(void)
{
	const time_t	now = std::time(NULL);

	for (std::map<int, Connection*>::iterator it = _connections.begin();
		 it != _connections.end(); ++it)
	{
		Connection*	connection = it->second;

		if (connection->state() != Connection::RUNNING_CGI)
			continue ;

		CgiProcess*	cgi = connection->cgi();
		if (!cgi)
			continue ;

		if (cgi->hasTimedOut(now, _cgiTimeout))
		{
			Logger::warn("CGI script timed out; terminating it");
			if (cgi->inputFd() != -1)
				_registry.erase(cgi->inputFd());
			if (cgi->outputFd() != -1)
				_registry.erase(cgi->outputFd());
			cgi->terminate();
			connection->sendError(504);
			continue ;
		}

		// A script that has closed stdout may still be running. Waiting for the
		// child to be reaped before responding is what keeps zombies from
		// accumulating and guarantees the exit status is known.
		if (cgi->state() != CgiProcess::RUNNING || cgi->outputFd() == -1)
		{
			if (!cgi->reap())
				continue ;
			if (cgi->inputFd() != -1)
				_registry.erase(cgi->inputFd());
			if (cgi->outputFd() != -1)
				_registry.erase(cgi->outputFd());
			connection->finishCgi();
		}
	}
}

void	EventLoop::enforceTimeouts(void)
{
	const time_t	now = std::time(NULL);

	for (std::map<int, Connection*>::iterator it = _connections.begin();
		 it != _connections.end(); ++it)
	{
		Connection*	connection = it->second;

		// A CGI request has its own, shorter budget, handled in pollCgi().
		if (connection->state() == Connection::RUNNING_CGI)
			continue ;

		if (now - connection->lastActivity() < _clientTimeout)
			continue ;

		// A connection that is still writing when it times out has a peer that
		// has stopped reading, so there is nowhere to put a diagnostic: the
		// only useful move is to close. This branch also catches a connection
		// that was already sent a 408 below, because sending it moved the
		// connection into the writing state and did not refresh its timer.
		//
		// Refreshing the timer here instead would mean a stalled peer never
		// times out at all: it would be handed a fresh 408 and a fresh deadline
		// on every expiry, holding its slot forever. That is a connection leak
		// an attacker can open at will, and it is what this branch prevents.
		if (connection->state() == Connection::WRITING_RESPONSE)
			scheduleClose(it->first);
		// An idle keep-alive connection is closed without ceremony: nothing was
		// in flight, so there is nothing to report.
		else if (connection->isIdle())
			scheduleClose(it->first);
		// A connection stalled part way through a request gets a 408 so the
		// client learns why, and deliberately keeps its expired timer, so the
		// next pass closes it once the response has had a moment to drain.
		// This is the defence against a Slowloris client that opens sockets and
		// dribbles bytes to hold them open.
		else
			connection->sendError(408);
	}
}

void	EventLoop::scheduleClose(int clientFd)
{
	for (size_t i = 0; i < _pendingClose.size(); ++i)
	{
		if (_pendingClose[i] == clientFd)
			return ;
	}
	_pendingClose.push_back(clientFd);
}

void	EventLoop::reapClosed(void)
{
	for (size_t i = 0; i < _pendingClose.size(); ++i)
		closeConnection(_pendingClose[i]);
	_pendingClose.clear();
}

void	EventLoop::closeConnection(int clientFd)
{
	std::map<int, Connection*>::iterator	found = _connections.find(clientFd);

	if (found == _connections.end())
		return ;

	// Any CGI descriptors this connection registered must leave the registry
	// before the Connection is destroyed, since destroying it closes them.
	CgiProcess*	cgi = found->second->cgi();
	if (cgi)
	{
		if (cgi->inputFd() != -1)
			_registry.erase(cgi->inputFd());
		if (cgi->outputFd() != -1)
			_registry.erase(cgi->outputFd());
	}

	_registry.erase(clientFd);
	delete found->second;
	_connections.erase(found);
	close(clientFd);

	Logger::debug("closed client on fd " + Utils::toString(static_cast<long>(clientFd)));
}
