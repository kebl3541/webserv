#ifndef EVENTLOOP_HPP
# define EVENTLOOP_HPP

# include <poll.h>
# include <ctime>
# include <map>
# include <string>
# include <vector>

class Config;
class Connection;
class ListenSocket;

// The single-threaded poll() loop that owns every descriptor.
//
// Two rules make the loop safe, and both were violated by the original:
//
//  1. The poll set is rebuilt from scratch at the top of each iteration rather
//     than mutated while it is being walked. The original pushed and erased
//     entries inside the loop that was iterating over them, which invalidates
//     the iterator. It happened to survive because reserve(1024) kept the
//     vector from reallocating below 1024 descriptors; that is luck, not a
//     guarantee, and it is undefined behaviour either way.
//
//  2. Descriptors are closed in one place, at the end of an iteration, so no
//     code path can act on an fd that another path already closed and that the
//     kernel may have handed to a new connection.
class EventLoop
{
	public:
		explicit EventLoop(const Config& config);
		~EventLoop();

		// Binds every configured endpoint. Returns false if none could be bound.
		bool	setup(void);

		// Runs until stop() is called. Returns the process exit status.
		int		run(void);

		// Signal-safe request to leave the loop after the current iteration.
		static void	requestStop(void);

	private:
		EventLoop(const EventLoop& other);
		EventLoop&	operator=(const EventLoop& other);

		// Ownership of a descriptor: what the loop should do when it fires.
		enum FdRole
		{
			ROLE_LISTEN,
			ROLE_CLIENT,
			ROLE_CGI_IN,
			ROLE_CGI_OUT
		};

		struct FdEntry
		{
			FdRole		role;
			int			ownerFd;	// the client fd a CGI pipe belongs to
			size_t		index;		// which listening socket, for ROLE_LISTEN

			FdEntry();
		};

		void	buildPollSet(std::vector<struct pollfd>& pollSet);
		void	handleEvent(const struct pollfd& entry);
		void	acceptClients(size_t listenerIndex);

		void	closeConnection(int clientFd);
		void	scheduleClose(int clientFd);
		void	reapClosed(void);

		void	pollCgi(void);
		void	enforceTimeouts(void);

		const Config&					_config;
		std::vector<ListenSocket*>		_listeners;
		std::map<int, Connection*>		_connections;
		std::map<int, FdEntry>			_registry;
		std::vector<int>				_pendingClose;

		long	_clientTimeout;		// seconds a connection may stay idle
		long	_cgiTimeout;		// seconds a script may run

		static volatile bool	_stopRequested;
};

#endif
