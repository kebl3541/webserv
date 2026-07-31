#include "Config.hpp"
#include "Logger.hpp"
#include "Utils.hpp"

#include <fstream>
#include <sstream>

// ---------------------------------------------------------------------------
// LocationConfig
// ---------------------------------------------------------------------------

LocationConfig::LocationConfig()
	: autoindex(false),
	  uploadEnabled(false),
	  redirectCode(0),
	  maxBodySize(0)
{
}

bool	LocationConfig::allowsMethod(const std::string& method) const
{
	// An empty set means the block never declared "allow_methods". Falling back
	// to the safe read-only trio keeps an under-specified config from silently
	// exposing writes.
	if (methods.empty())
		return method == "GET" || method == "HEAD";
	return methods.find(method) != methods.end();
}

// ---------------------------------------------------------------------------
// ServerConfig
// ---------------------------------------------------------------------------

ServerConfig::ServerConfig()
	: host("0.0.0.0"),
	  port("8080"),
	  root("./www"),
	  index("index.html"),
	  maxBodySize(1048576)
{
}

const LocationConfig*	ServerConfig::matchLocation(const std::string& uriPath) const
{
	const LocationConfig*	best = NULL;
	size_t					bestLength = 0;

	for (size_t i = 0; i < locations.size(); ++i)
	{
		const std::string&	prefix = locations[i].path;

		if (!Utils::startsWith(uriPath, prefix))
			continue ;
		// "/cgi" must not match "/cgi-bin/x": a prefix only counts when it ends
		// at a path boundary.
		if (prefix.size() > 1
			&& uriPath.size() > prefix.size()
			&& prefix[prefix.size() - 1] != '/'
			&& uriPath[prefix.size()] != '/')
			continue ;
		if (best == NULL || prefix.size() > bestLength)
		{
			best = &locations[i];
			bestLength = prefix.size();
		}
	}
	return best;
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

Config::Config()
{
}

Config::~Config()
{
}

const std::vector<ServerConfig>&	Config::servers(void) const
{
	return _servers;
}

const std::string&	Config::error(void) const
{
	return _error;
}

bool	Config::fail(const std::string& message, size_t line)
{
	std::ostringstream	stream;

	stream << "line " << line << ": " << message;
	_error = stream.str();
	return false;
}

bool	Config::loadFromFile(const std::string& path)
{
	std::ifstream	file(path.c_str());

	if (!file.is_open())
	{
		_error = "cannot open configuration file '" + path + "'";
		return false;
	}

	std::ostringstream	buffer;
	buffer << file.rdbuf();
	file.close();

	_tokens.clear();
	_servers.clear();
	_error.clear();

	if (!tokenise(buffer.str()))
		return false;

	size_t	index = 0;
	while (index < _tokens.size())
	{
		if (_tokens[index].text != "server")
			return fail("expected 'server', found '" + _tokens[index].text + "'",
				_tokens[index].line);
		++index;
		if (!parseServerBlock(index))
			return false;
	}

	if (_servers.empty())
	{
		_error = "configuration defines no server blocks";
		return false;
	}
	return validate();
}

// Splits the file into words, braces and semicolons. Handling this as a real
// token stream is what removes the original parser's dependence on tabs as the
// key/value separator.
bool	Config::tokenise(const std::string& source)
{
	size_t	line = 1;
	size_t	i = 0;

	while (i < source.size())
	{
		char	c = source[i];

		if (c == '\n')
		{
			++line;
			++i;
			continue ;
		}
		if (c == ' ' || c == '\t' || c == '\r')
		{
			++i;
			continue ;
		}
		if (c == '#')
		{
			while (i < source.size() && source[i] != '\n')
				++i;
			continue ;
		}
		if (c == '{' || c == '}' || c == ';')
		{
			Token	token;
			token.text = std::string(1, c);
			token.line = line;
			_tokens.push_back(token);
			++i;
			continue ;
		}
		if (c == '"' || c == '\'')
		{
			char		quote = c;
			size_t		start = ++i;
			while (i < source.size() && source[i] != quote)
			{
				if (source[i] == '\n')
					return fail("unterminated quoted string", line);
				++i;
			}
			if (i >= source.size())
				return fail("unterminated quoted string", line);
			Token	token;
			token.text = source.substr(start, i - start);
			token.line = line;
			_tokens.push_back(token);
			++i;
			continue ;
		}

		size_t	start = i;
		while (i < source.size()
			&& source[i] != ' ' && source[i] != '\t' && source[i] != '\r'
			&& source[i] != '\n' && source[i] != ';' && source[i] != '{'
			&& source[i] != '}' && source[i] != '#')
			++i;
		Token	token;
		token.text = source.substr(start, i - start);
		token.line = line;
		_tokens.push_back(token);
	}
	return true;
}

bool	Config::expect(size_t& index, const std::string& text)
{
	if (index >= _tokens.size())
	{
		_error = "unexpected end of file, expected '" + text + "'";
		return false;
	}
	if (_tokens[index].text != text)
		return fail("expected '" + text + "', found '" + _tokens[index].text + "'",
			_tokens[index].line);
	++index;
	return true;
}

bool	Config::parseServerBlock(size_t& index)
{
	if (!expect(index, "{"))
		return false;

	ServerConfig	server;

	while (index < _tokens.size() && _tokens[index].text != "}")
	{
		if (_tokens[index].text == "location")
		{
			++index;
			if (!parseLocationBlock(index, server))
				return false;
		}
		else if (!parseDirective(index, server, NULL))
			return false;
	}
	if (!expect(index, "}"))
		return false;

	_servers.push_back(server);
	return true;
}

bool	Config::parseLocationBlock(size_t& index, ServerConfig& server)
{
	if (index >= _tokens.size())
	{
		_error = "unexpected end of file inside location";
		return false;
	}

	LocationConfig	location;
	location.path = _tokens[index].text;
	size_t	line = _tokens[index].line;
	++index;

	if (location.path.empty() || location.path[0] != '/')
		return fail("location path must start with '/'", line);

	if (!expect(index, "{"))
		return false;

	while (index < _tokens.size() && _tokens[index].text != "}")
	{
		if (_tokens[index].text == "location")
			return fail("nested location blocks are not supported", _tokens[index].line);
		if (!parseDirective(index, server, &location))
			return false;
	}
	if (!expect(index, "}"))
		return false;

	server.locations.push_back(location);
	return true;
}

bool	Config::parseDirective(size_t& index, ServerConfig& server, LocationConfig* location)
{
	const std::string	name = _tokens[index].text;
	const size_t		line = _tokens[index].line;

	++index;

	std::vector<std::string>	arguments;
	while (index < _tokens.size()
		&& _tokens[index].text != ";"
		&& _tokens[index].text != "{"
		&& _tokens[index].text != "}")
	{
		arguments.push_back(_tokens[index].text);
		++index;
	}
	if (index >= _tokens.size() || _tokens[index].text != ";")
		return fail("directive '" + name + "' must end with ';'", line);
	++index;

	if (arguments.empty())
		return fail("directive '" + name + "' needs at least one value", line);

	// --- directives valid in both scopes -----------------------------------
	if (name == "root")
	{
		if (location)
			location->root = arguments[0];
		else
			server.root = arguments[0];
		return true;
	}
	if (name == "index")
	{
		if (location)
			location->index = arguments[0];
		else
			server.index = arguments[0];
		return true;
	}
	if (name == "client_max_body_size")
	{
		size_t	value = 0;
		if (!Utils::parseSizeT(arguments[0], value))
			return fail("client_max_body_size expects a byte count", line);
		if (location)
			location->maxBodySize = value;
		else
			server.maxBodySize = value;
		return true;
	}

	// --- server-only directives --------------------------------------------
	if (!location)
	{
		if (name == "listen")
		{
			// Accepts "8080" as well as "127.0.0.1:8080".
			const std::string&	value = arguments[0];
			size_t				colon = value.rfind(':');

			if (colon == std::string::npos)
				server.port = value;
			else
			{
				server.host = value.substr(0, colon);
				server.port = value.substr(colon + 1);
			}
			long	portNumber = 0;
			if (!Utils::parseLong(server.port, portNumber)
				|| portNumber <= 0 || portNumber > 65535)
				return fail("listen expects a port between 1 and 65535", line);
			return true;
		}
		if (name == "server_name")
		{
			server.serverNames = arguments;
			return true;
		}
		if (name == "error_page")
		{
			// "error_page 404 500 /errors/generic.html" assigns one page to
			// several codes, so every argument but the last is a status code.
			if (arguments.size() < 2)
				return fail("error_page expects one or more codes then a path", line);
			const std::string&	target = arguments[arguments.size() - 1];
			for (size_t i = 0; i + 1 < arguments.size(); ++i)
			{
				long	code = 0;
				if (!Utils::parseLong(arguments[i], code) || code < 300 || code > 599)
					return fail("error_page: '" + arguments[i] + "' is not a status code", line);
				server.errorPages[static_cast<int>(code)] = target;
			}
			return true;
		}
	}

	// --- location-only directives ------------------------------------------
	if (location)
	{
		if (name == "allow_methods")
		{
			for (size_t i = 0; i < arguments.size(); ++i)
			{
				const std::string	method = arguments[i];
				if (method != "GET" && method != "POST" && method != "DELETE"
					&& method != "HEAD" && method != "PUT")
					return fail("allow_methods: unsupported method '" + method + "'", line);
				location->methods.insert(method);
			}
			// HEAD is defined as GET without a body, so allowing GET implies it.
			if (location->methods.count("GET"))
				location->methods.insert("HEAD");
			return true;
		}
		if (name == "alias")
		{
			location->alias = arguments[0];
			return true;
		}
		if (name == "autoindex")
		{
			if (arguments[0] != "on" && arguments[0] != "off")
				return fail("autoindex expects 'on' or 'off'", line);
			location->autoindex = (arguments[0] == "on");
			return true;
		}
		if (name == "upload_store")
		{
			location->uploadStore = arguments[0];
			location->uploadEnabled = true;
			return true;
		}
		if (name == "cgi_extension")
		{
			location->cgiExtension = arguments[0];
			if (!location->cgiExtension.empty() && location->cgiExtension[0] != '.')
				location->cgiExtension = "." + location->cgiExtension;
			return true;
		}
		if (name == "cgi_interpreter")
		{
			location->cgiInterpreter = arguments[0];
			return true;
		}
		if (name == "return")
		{
			if (arguments.size() < 2)
				return fail("return expects a status code and a target", line);
			long	code = 0;
			if (!Utils::parseLong(arguments[0], code) || code < 300 || code > 399)
				return fail("return expects a 3xx status code", line);
			location->redirectCode = static_cast<int>(code);
			location->redirectTarget = arguments[1];
			return true;
		}
	}

	return fail("unknown directive '" + name + "' in this context", line);
}

bool	Config::validate(void)
{
	std::set<std::string>	endpoints;

	for (size_t i = 0; i < _servers.size(); ++i)
	{
		ServerConfig&		server = _servers[i];
		const std::string	endpoint = server.host + ":" + server.port;

		// Two servers on one endpoint would mean two binds of the same socket.
		// Name-based virtual hosting on a shared port is out of scope here.
		if (!endpoints.insert(endpoint).second)
		{
			_error = "two server blocks both listen on " + endpoint;
			return false;
		}

		// Guarantee a catch-all route so that every request resolves somewhere.
		if (server.matchLocation("/") == NULL)
		{
			LocationConfig	fallback;
			fallback.path = "/";
			fallback.methods.insert("GET");
			fallback.methods.insert("HEAD");
			fallback.index = server.index;
			server.locations.push_back(fallback);
			Logger::warn("server " + endpoint + " had no '/' location; a read-only one was added");
		}

		for (size_t j = 0; j < server.locations.size(); ++j)
		{
			LocationConfig&	location = server.locations[j];

			if (location.root.empty() && location.alias.empty())
				location.root = server.root;
			if (location.index.empty())
				location.index = server.index;
			if (location.maxBodySize == 0)
				location.maxBodySize = server.maxBodySize;
			if (!location.cgiExtension.empty() && location.cgiInterpreter.empty())
			{
				_error = "location " + location.path
					+ ": cgi_extension without cgi_interpreter";
				return false;
			}
		}
	}
	return true;
}
