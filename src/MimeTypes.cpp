#include "MimeTypes.hpp"
#include "Utils.hpp"

namespace
{
	const std::string	DEFAULT_TYPE = "application/octet-stream";
}

const std::map<std::string, std::string>&	MimeTypes::table(void)
{
	// Built once on first use and never mutated, so a function-local static is
	// safe here and avoids depending on static initialisation order.
	static std::map<std::string, std::string>	types;

	if (!types.empty())
		return types;

	types["html"] = "text/html; charset=utf-8";
	types["htm"]  = "text/html; charset=utf-8";
	types["css"]  = "text/css; charset=utf-8";
	types["js"]   = "application/javascript; charset=utf-8";
	types["json"] = "application/json";
	types["xml"]  = "application/xml";
	types["txt"]  = "text/plain; charset=utf-8";
	types["md"]   = "text/plain; charset=utf-8";
	types["csv"]  = "text/csv; charset=utf-8";

	types["png"]  = "image/png";
	types["jpg"]  = "image/jpeg";
	types["jpeg"] = "image/jpeg";
	types["gif"]  = "image/gif";
	types["svg"]  = "image/svg+xml";
	types["ico"]  = "image/x-icon";
	types["webp"] = "image/webp";
	types["bmp"]  = "image/bmp";

	types["pdf"]  = "application/pdf";
	types["zip"]  = "application/zip";
	types["gz"]   = "application/gzip";
	types["tar"]  = "application/x-tar";

	types["mp3"]  = "audio/mpeg";
	types["wav"]  = "audio/wav";
	types["mp4"]  = "video/mp4";
	types["webm"] = "video/webm";

	types["woff"]  = "font/woff";
	types["woff2"] = "font/woff2";
	types["ttf"]   = "font/ttf";

	return types;
}

const std::string&	MimeTypes::forPath(const std::string& path)
{
	size_t	slash = path.find_last_of('/');
	size_t	dot = path.find_last_of('.');

	// A dot that sits before the last slash belongs to a directory name, and a
	// leading dot marks a hidden file rather than an extension.
	if (dot == std::string::npos)
		return DEFAULT_TYPE;
	if (slash != std::string::npos && dot < slash)
		return DEFAULT_TYPE;
	if (slash != std::string::npos && dot == slash + 1)
		return DEFAULT_TYPE;
	if (slash == std::string::npos && dot == 0)
		return DEFAULT_TYPE;

	std::string	extension = Utils::toLower(path.substr(dot + 1));

	const std::map<std::string, std::string>&			types = table();
	std::map<std::string, std::string>::const_iterator	found = types.find(extension);

	if (found == types.end())
		return DEFAULT_TYPE;
	return found->second;
}
