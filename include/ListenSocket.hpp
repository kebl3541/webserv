#ifndef LISTENSOCKET_HPP
# define LISTENSOCKET_HPP

# include <string>
# include <vector>

struct ServerConfig;
struct addrinfo;

// A bound, listening TCP socket for one server block.
//
// One server block can need more than one of these. A name like "localhost"
// resolves to both ::1 and 127.0.0.1, and a socket can only be bound to one
// address, so serving both means one listener per address. Binding only the
// IPv4 address is why "http://localhost:8080" was refused on its first attempt:
// clients resolve to the IPv6 address first, and only some of them retry.
class ListenSocket
{
	public:
		ListenSocket();
		~ListenSocket();

		// Binds every address the server block resolves to, appending one
		// listener per address. Returns false only when none could be bound.
		static bool	openAll(const ServerConfig& server,
							std::vector<ListenSocket*>& out);

		void	closeSocket(void);

		int						fd(void) const;
		const ServerConfig*		config(void) const;
		const std::string&		describe(void) const;

	private:
		ListenSocket(const ListenSocket& other);
		ListenSocket&	operator=(const ListenSocket& other);

		// Binds one resolved address. Returns false with a logged reason.
		bool	openOne(const ServerConfig& server, const struct addrinfo* address);

		int					_fd;
		const ServerConfig*	_config;
		std::string			_description;	// "127.0.0.1:8080", for logging
};

#endif
