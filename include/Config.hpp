#ifndef CONFIG_HPP
# define CONFIG_HPP

# include <map>
# include <set>
# include <string>
# include <vector>

// One "location" block: how a URI prefix maps onto the filesystem and what is
// permitted there.
struct LocationConfig
{
	std::string				path;			// URI prefix this block matches
	std::string				root;			// filesystem root for this prefix
	std::string				alias;			// replaces the matched prefix entirely
	std::set<std::string>	methods;		// empty means "inherit server default"
	std::string				index;			// file served for a directory request
	bool					autoindex;		// generate a listing when no index file
	std::string				uploadStore;	// where POST bodies are written
	bool					uploadEnabled;
	std::string				cgiExtension;	// e.g. ".py"
	std::string				cgiInterpreter;	// absolute path to the interpreter
	int						redirectCode;	// 0 when this block does not redirect
	std::string				redirectTarget;
	size_t					maxBodySize;	// 0 means "inherit from the server"

	LocationConfig();

	bool	allowsMethod(const std::string& method) const;
};

// One "server" block: a listening endpoint plus its routing table.
struct ServerConfig
{
	std::string					host;
	std::string					port;
	std::vector<std::string>	serverNames;
	std::string					root;
	std::string					index;
	size_t						maxBodySize;
	std::map<int, std::string>	errorPages;
	std::vector<LocationConfig>	locations;

	ServerConfig();

	// Longest-prefix match, the same rule nginx uses, so that "/cgi-bin/" wins
	// over "/" for a request to "/cgi-bin/script.py".
	const LocationConfig*	matchLocation(const std::string& uriPath) const;
};

// Parses the configuration file into a list of server blocks. Every failure is
// reported with a line number rather than being silently skipped.
class Config
{
	public:
		Config();
		~Config();

		bool	loadFromFile(const std::string& path);

		const std::vector<ServerConfig>&	servers(void) const;
		const std::string&					error(void) const;

	private:
		Config(const Config& other);
		Config&	operator=(const Config& other);

		struct Token
		{
			std::string	text;
			size_t		line;
		};

		bool	tokenise(const std::string& source);
		bool	parseServerBlock(size_t& index);
		bool	parseLocationBlock(size_t& index, ServerConfig& server);
		bool	parseDirective(size_t& index, ServerConfig& server, LocationConfig* location);
		bool	validate(void);

		bool	fail(const std::string& message, size_t line);
		bool	expect(size_t& index, const std::string& text);

		std::vector<Token>			_tokens;
		std::vector<ServerConfig>	_servers;
		std::string					_error;
};

#endif
