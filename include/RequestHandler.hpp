#ifndef REQUESTHANDLER_HPP
# define REQUESTHANDLER_HPP

# include <string>

class HttpRequest;
class HttpResponse;
struct LocationConfig;
struct ServerConfig;

// Turns a parsed request into a response.
//
// Kept free of socket and event-loop concerns so that routing decisions can be
// reasoned about, and tested, without a live connection.
namespace RequestHandler
{
	// Outcome of routing a request.
	struct Result
	{
		bool					isCgi;			// caller must fork a CGI child
		const LocationConfig*	location;
		std::string				scriptPath;		// filesystem path of the script
		std::string				pathInfo;		// trailing path handed to the script
		int						status;			// non-zero means "answer with this"

		// Headers that must survive onto the error response. A 405 is required
		// to advertise what is permitted, and the error path builds a fresh
		// response, so the value has to travel with the result rather than
		// being written into a response object that is then discarded.
		std::string				allowHeader;

		Result();
	};

	// Decides what should happen. Fills `response` for anything served directly,
	// or reports isCgi so the connection can start a child process.
	Result	route(const HttpRequest& request,
				  const ServerConfig& server,
				  HttpResponse& response);

	// Resolves a URI path to a filesystem path inside the location's root.
	// Returns false when the result would escape that root.
	bool	resolvePath(const std::string& uriPath,
						const LocationConfig& location,
						std::string& out);

	// Fills `response` with an error page, using the server's configured page
	// when one exists and a built-in otherwise.
	void	buildError(int status, const ServerConfig& server, HttpResponse& response);

	// Renders a directory listing, used when autoindex is on.
	bool	buildAutoIndex(const std::string& directoryPath,
						   const std::string& uriPath,
						   HttpResponse& response);
}

#endif
