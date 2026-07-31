#ifndef LISTENSOCKET_HPP
# define LISTENSOCKET_HPP

# include <string>

struct ServerConfig;

// A bound, listening TCP socket for one server block.
class ListenSocket
{
	public:
		ListenSocket();
		~ListenSocket();

		// Creates, binds and listens. Returns false with a logged reason.
		bool	open(const ServerConfig& server);

		void	closeSocket(void);

		int						fd(void) const;
		const ServerConfig*		config(void) const;

	private:
		ListenSocket(const ListenSocket& other);
		ListenSocket&	operator=(const ListenSocket& other);

		int					_fd;
		const ServerConfig*	_config;
};

#endif
