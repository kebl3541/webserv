#ifndef CONNECTION_HPP
# define CONNECTION_HPP

# include "CgiProcess.hpp"
# include "HttpRequest.hpp"
# include "HttpResponse.hpp"

# include <ctime>
# include <string>

struct ServerConfig;

// One client socket and everything attached to it.
//
// The lifetime of a request is modelled as an explicit state machine. The
// original code signalled progress with bare integers returned up through the
// event loop (0, 2, 3, DISCONNECTED), which made it impossible to tell from a
// call site which transitions were legal. Naming the states makes the illegal
// ones unrepresentable.
class Connection
{
	public:
		enum State
		{
			READING_REQUEST,	// accumulating bytes until the parser is satisfied
			RUNNING_CGI,		// a child process owns the response
			WRITING_RESPONSE,	// draining the serialised response to the socket
			CLOSING				// scheduled for removal by the event loop
		};

		Connection(int fd, const ServerConfig& server);
		~Connection();

		int						fd(void) const;
		State					state(void) const;
		const ServerConfig&		server(void) const;
		CgiProcess*				cgi(void);

		// True once the response has been fully written and the connection is
		// waiting for another request on the same socket.
		bool	isIdle(void) const;

		// Reads from the socket and advances the parser. Returns false when the
		// connection must be closed (peer hung up, or a fatal socket error).
		bool	onReadable(void);

		// Writes as much of the pending response as the socket accepts.
		// Returns false when the connection must be closed.
		bool	onWritable(void);

		// Drives the CGI exchange; called when either CGI pipe is ready.
		void	onCgiReadable(void);
		void	onCgiWritable(void);

		// Called once the CGI child has finished so its output becomes a response.
		void	finishCgi(void);

		// Queues a response and moves to WRITING_RESPONSE.
		void	sendResponse(HttpResponse& response);

		// Builds an error response, honouring the server's error_page settings.
		// `allowHeader`, when set, is emitted as Allow: for a 405.
		void	sendError(int status, const std::string& allowHeader = "");

		time_t	lastActivity(void) const;
		void	touch(void);

		bool	shouldKeepAlive(void) const;

	private:
		Connection(const Connection& other);
		Connection&	operator=(const Connection& other);

		// Routes a fully parsed request and produces a response, or starts CGI.
		void	dispatch(void);

		// Prepares the parser for the next request on a keep-alive connection.
		void	recycle(void);

		// Narrows the body limit from the server default to the one configured
		// for the matched location. This can only happen once the headers name
		// a path, which is why the limit is applied here rather than being
		// fixed when the connection was created.
		void	applyLocationBodyLimit(void);

		int					_fd;
		const ServerConfig*	_server;
		State				_state;

		HttpRequest			_request;

		std::string			_outputBuffer;
		size_t				_outputOffset;	// how much has reached the socket

		CgiProcess*			_cgi;

		bool				_keepAlive;
		bool				_headOnly;
		bool				_bodyLimitApplied;
		time_t				_lastActivity;
		size_t				_requestsServed;
};

#endif
