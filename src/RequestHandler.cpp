#include "RequestHandler.hpp"
#include "Config.hpp"
#include "HttpRequest.hpp"
#include "HttpResponse.hpp"
#include "Logger.hpp"
#include "MimeTypes.hpp"
#include "Upload.hpp"
#include "Utils.hpp"

#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace RequestHandler
{

Result::Result()
	: isCgi(false),
	  location(NULL),
	  status(0)
{
}

// Reads a file into memory. Fine for the file sizes this server is meant to
// hold; a production server would stream it with sendfile() instead.
static bool	readFile(const std::string& path, std::string& out)
{
	std::ifstream	file(path.c_str(), std::ios::binary);

	if (!file.is_open())
		return false;

	std::ostringstream	buffer;
	buffer << file.rdbuf();
	out = buffer.str();
	file.close();
	return true;
}

bool	resolvePath(const std::string& uriPath,
					const LocationConfig& location,
					std::string& out)
{
	std::string	base;
	std::string	relative;

	if (!location.alias.empty())
	{
		// alias replaces the matched prefix: /docs/ -> /var/html/ means
		// /docs/a.html resolves to /var/html/a.html.
		base = location.alias;
		relative = uriPath.substr(location.path.size());
	}
	else
	{
		// root appends the whole path: /docs/a.html under root /var/www
		// resolves to /var/www/docs/a.html.
		base = location.root;
		relative = uriPath;
	}

	std::string	combined = Utils::joinPath(base, relative);

	// The path was already normalised during parsing, so ".." cannot appear
	// here. This second check defends against a symlink or an alias whose own
	// text walks upwards.
	std::string	normalised;
	if (!Utils::normalisePath(combined, normalised))
		return false;

	std::string	normalisedBase;
	if (!Utils::normalisePath(base, normalisedBase))
		return false;

	// Compare against the base with a trailing slash so that "/var/www-secret"
	// is not accepted as living inside "/var/www".
	std::string	guard = normalisedBase;
	if (guard.empty() || guard[guard.size() - 1] != '/')
		guard += "/";

	std::string	candidate = normalised;
	if (candidate.size() + 1 == guard.size() && guard.compare(0, candidate.size(), candidate) == 0)
		; // the path is the base directory itself
	else if (!Utils::startsWith(candidate, guard))
	{
		Logger::warn("rejected path outside its root: " + combined);
		return false;
	}

	out = normalised;
	return true;
}

void	buildError(int status, const ServerConfig& server, HttpResponse& response)
{
	response.setStatus(status);

	std::map<int, std::string>::const_iterator	page = server.errorPages.find(status);
	if (page != server.errorPages.end())
	{
		std::string	path = Utils::joinPath(server.root, page->second);
		std::string	body;

		if (readFile(path, body))
		{
			response.setBody(body, MimeTypes::forPath(path));
			return ;
		}
		// A missing custom page must not turn a 404 into a 500, so fall through
		// to the built-in page instead of failing.
		Logger::warn("configured error page is unreadable: " + path);
	}
	response.setBody(HttpResponse::defaultErrorBody(status), "text/html; charset=utf-8");
}

bool	buildAutoIndex(const std::string& directoryPath,
					   const std::string& uriPath,
					   HttpResponse& response)
{
	DIR*	directory = opendir(directoryPath.c_str());

	if (!directory)
		return false;

	std::vector<std::string>	directories;
	std::vector<std::string>	files;
	struct dirent*				entry;

	while ((entry = readdir(directory)) != NULL)
	{
		std::string	name = entry->d_name;

		if (name == ".")
			continue ;
		if (Utils::isDirectory(Utils::joinPath(directoryPath, name)))
			directories.push_back(name + "/");
		else
			files.push_back(name);
	}
	closedir(directory);

	std::sort(directories.begin(), directories.end());
	std::sort(files.begin(), files.end());

	std::ostringstream	html;
	html << "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n"
		 << "<meta charset=\"utf-8\">\n"
		 << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
		 << "<title>Index of " << Utils::htmlEscape(uriPath) << "</title>\n"
		 << "<style>\n"
		 << "  html { font-size: 15px; }\n"
		 << "  body { max-width: 40em; margin: 3em auto; padding: 0 1.5em;\n"
		 << "         font-family: Menlo, Consolas, monospace;\n"
		 << "         line-height: 1.6; color: #1a1a1a; background: #fff; }\n"
		 << "  h1 { font-size: 1em; font-weight: bold; margin: 0 0 .8em;\n"
		 << "       padding-bottom: .5em; border-bottom: 1px solid #ddd; }\n"
		 << "  ul { list-style: none; padding: 0; margin: 0; }\n"
		 << "  li { padding: .15em 0; }\n"
		 << "  a { color: #0645ad; text-decoration: none; }\n"
		 << "  a:hover { text-decoration: underline; }\n"
		 << "</style>\n</head>\n<body>\n"
		 << "<h1>Index of " << Utils::htmlEscape(uriPath) << "</h1>\n<ul>\n";

	for (size_t i = 0; i < directories.size(); ++i)
		html << "<li><a href=\"" << Utils::uriEncode(directories[i])
			 << "\">" << Utils::htmlEscape(directories[i]) << "</a></li>\n";
	for (size_t i = 0; i < files.size(); ++i)
		html << "<li><a href=\"" << Utils::uriEncode(files[i]) << "\">"
			 << Utils::htmlEscape(files[i]) << "</a></li>\n";

	html << "</ul>\n</body>\n</html>\n";

	response.setStatus(200);
	response.setBody(html.str(), "text/html; charset=utf-8");
	return true;
}

// Serves a file, or a directory's index file, or a listing.
static int	serveStatic(const std::string& filesystemPath,
						const std::string& uriPath,
						const std::string& query,
						const LocationConfig& location,
						HttpResponse& response)
{
	if (Utils::isDirectory(filesystemPath))
	{
		// Without the trailing slash a browser resolves relative links against
		// the parent directory, so redirect before serving anything.
		if (uriPath.empty() || uriPath[uriPath.size() - 1] != '/')
		{
			response.setStatus(301);
			// The query string belongs to the request, not the path, so it has
			// to survive the redirect or the client silently loses it.
			std::string	target = Utils::uriEncode(uriPath) + "/";
			if (!query.empty())
				target += "?" + query;
			response.setHeader("Location", target);
			response.setBody("", "");
			return 0;
		}

		if (!location.index.empty())
		{
			std::string	indexPath = Utils::joinPath(filesystemPath, location.index);
			if (Utils::isRegularFile(indexPath))
			{
				std::string	body;
				if (!readFile(indexPath, body))
					return 403;
				response.setStatus(200);
				response.setBody(body, MimeTypes::forPath(indexPath));
				return 0;
			}
		}
		if (location.autoindex)
		{
			if (!buildAutoIndex(filesystemPath, uriPath, response))
				return 403;
			return 0;
		}
		// A directory with no index file and no listing is deliberately opaque.
		return 403;
	}

	if (!Utils::pathExists(filesystemPath))
		return 404;
	if (!Utils::isRegularFile(filesystemPath))
		// Sockets, devices and FIFOs would block or leak host state if served.
		return 403;
	if (access(filesystemPath.c_str(), R_OK) != 0)
		return 403;

	std::string	body;
	if (!readFile(filesystemPath, body))
		return 403;

	response.setStatus(200);
	response.setBody(body, MimeTypes::forPath(filesystemPath));
	return 0;
}

static int	handleDelete(const std::string& filesystemPath)
{
	if (!Utils::pathExists(filesystemPath))
		return 404;
	if (Utils::isDirectory(filesystemPath))
		return 403;
	if (std::remove(filesystemPath.c_str()) != 0)
	{
		Logger::error("DELETE failed for " + filesystemPath + ": " + strerror(errno));
		return (errno == EACCES || errno == EPERM) ? 403 : 500;
	}
	return 0;
}

Result	route(const HttpRequest& request,
			  const ServerConfig& server,
			  HttpResponse& response)
{
	Result	result;

	const LocationConfig*	location = server.matchLocation(request.path());
	if (!location)
	{
		result.status = 404;
		return result;
	}
	result.location = location;

	// A redirect short-circuits everything else about the block.
	if (location->redirectCode != 0)
	{
		response.setStatus(location->redirectCode);
		response.setHeader("Location", location->redirectTarget);
		response.setBody(HttpResponse::defaultErrorBody(location->redirectCode),
			"text/html; charset=utf-8");
		return result;
	}

	const std::string&	method = request.method();

	// A method the server does not implement at all is 501; one that is simply
	// not permitted here is 405, and that answer must advertise what is.
	if (method != "GET" && method != "HEAD" && method != "POST" && method != "DELETE")
	{
		result.status = 501;
		return result;
	}
	if (!location->allowsMethod(method))
	{
		std::string	allowed;
		for (std::set<std::string>::const_iterator it = location->methods.begin();
			 it != location->methods.end(); ++it)
		{
			if (!allowed.empty())
				allowed += ", ";
			allowed += *it;
		}
		if (allowed.empty())
			allowed = "GET, HEAD";
		result.allowHeader = allowed;
		result.status = 405;
		return result;
	}

	std::string	filesystemPath;
	if (!resolvePath(request.path(), *location, filesystemPath))
	{
		result.status = 403;
		return result;
	}

	// --- CGI ---------------------------------------------------------------
	if (!location->cgiExtension.empty())
	{
		// The script is the longest leading portion of the path that ends with
		// the CGI extension; whatever follows becomes PATH_INFO.
		std::string	uriPath = request.path();
		size_t		extensionAt = uriPath.find(location->cgiExtension);

		if (extensionAt != std::string::npos)
		{
			size_t	afterExtension = extensionAt + location->cgiExtension.size();
			// The extension must end at a path boundary, so that "script.python"
			// is not mistaken for "script.py".
			if (afterExtension == uriPath.size() || uriPath[afterExtension] == '/')
			{
				std::string	scriptUri = uriPath.substr(0, afterExtension);
				result.pathInfo = uriPath.substr(afterExtension);

				if (!resolvePath(scriptUri, *location, result.scriptPath))
				{
					result.status = 403;
					return result;
				}
				if (!Utils::isRegularFile(result.scriptPath))
				{
					result.status = 404;
					return result;
				}
				result.isCgi = true;
				return result;
			}
		}
	}

	// --- POST --------------------------------------------------------------
	if (method == "POST")
	{
		if (!location->uploadEnabled)
		{
			// POST to a location with nowhere to put the body is a config
			// mismatch, not a client error the body could fix. The Allow header
			// still has to name what this location really permits, which is not
			// necessarily the read-only pair.
			std::string	allowed;
			for (std::set<std::string>::const_iterator it = location->methods.begin();
				 it != location->methods.end(); ++it)
			{
				if (*it == "POST")
					continue ;
				if (!allowed.empty())
					allowed += ", ";
				allowed += *it;
			}
			result.status = 405;
			result.allowHeader = allowed.empty() ? "GET, HEAD" : allowed;
			return result;
		}

		std::string	storeDirectory = location->uploadStore;
		if (!Utils::isDirectory(storeDirectory))
		{
			Logger::error("upload_store is not a directory: " + storeDirectory);
			result.status = 500;
			return result;
		}

		std::vector<std::string>	stored;
		int	status = Upload::store(request, storeDirectory, stored);
		if (status != 0)
		{
			result.status = status;
			return result;
		}

		std::ostringstream	html;
		html << "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
			 << "<title>Upload complete</title></head><body>\n"
			 << "<h1>Upload complete</h1>\n<ul>\n";
		for (size_t i = 0; i < stored.size(); ++i)
			html << "  <li>" << stored[i] << "</li>\n";
		html << "</ul>\n</body></html>\n";

		response.setStatus(201);
		response.setBody(html.str(), "text/html; charset=utf-8");
		return result;
	}

	// --- DELETE ------------------------------------------------------------
	if (method == "DELETE")
	{
		int	status = handleDelete(filesystemPath);
		if (status != 0)
		{
			result.status = status;
			return result;
		}
		response.setStatus(204);
		response.setBody("", "");
		return result;
	}

	// --- GET and HEAD ------------------------------------------------------
	int	status = serveStatic(filesystemPath, request.path(), request.query(),
		*location, response);
	if (status != 0)
		result.status = status;
	return result;
}

}
