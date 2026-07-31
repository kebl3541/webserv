#include "Upload.hpp"
#include "HttpRequest.hpp"
#include "Logger.hpp"
#include "Utils.hpp"

#include <fstream>
#include <sstream>

namespace Upload
{

bool	extractBoundary(const std::string& contentType, std::string& boundary)
{
	const std::string	needle = "boundary=";
	size_t				at = Utils::toLower(contentType).find(needle);

	// The original code assumed this search always succeeded and advanced the
	// returned pointer by nine bytes regardless, dereferencing NULL whenever a
	// client omitted the parameter.
	if (at == std::string::npos)
		return false;

	std::string	value = Utils::trim(contentType.substr(at + needle.size()));

	// The value may be quoted, and may be followed by further parameters.
	if (!value.empty() && value[0] == '"')
	{
		size_t	closing = value.find('"', 1);
		if (closing == std::string::npos)
			return false;
		value = value.substr(1, closing - 1);
	}
	else
	{
		size_t	semicolon = value.find(';');
		if (semicolon != std::string::npos)
			value = Utils::trim(value.substr(0, semicolon));
	}

	if (value.empty() || value.size() > 70)	// RFC 2046 caps boundaries at 70
		return false;

	boundary = value;
	return true;
}

std::string	sanitiseFilename(const std::string& name)
{
	// Keep only the final path component: a browser may legitimately send a
	// full path, and an attacker will deliberately send one.
	size_t		slash = name.find_last_of("/\\");
	std::string	base = (slash == std::string::npos) ? name : name.substr(slash + 1);

	base = Utils::trim(base);

	// "." and ".." are not usable filenames, and a leading dot would create a
	// hidden file the uploader did not ask for.
	if (base.empty() || base == "." || base == "..")
		return "";

	std::string	result;
	for (size_t i = 0; i < base.size(); ++i)
	{
		unsigned char	c = static_cast<unsigned char>(base[i]);
		// Control characters and shell metacharacters never belong in a name
		// that will be handed to the filesystem.
		if (c < 32 || c == 127)
			continue ;
		if (std::string("<>:\"|?*").find(static_cast<char>(c)) != std::string::npos)
			continue ;
		result += static_cast<char>(c);
	}

	if (result.empty() || result[0] == '.')
		return "";
	if (result.size() > 255)
		result = result.substr(0, 255);
	return result;
}

// Pulls the filename out of a Content-Disposition header.
static std::string	filenameFromDisposition(const std::string& disposition)
{
	size_t	at = Utils::toLower(disposition).find("filename=");

	if (at == std::string::npos)
		return "";

	std::string	value = Utils::trim(disposition.substr(at + 9));
	if (value.empty())
		return "";

	if (value[0] == '"')
	{
		size_t	closing = value.find('"', 1);
		if (closing == std::string::npos)
			return "";
		value = value.substr(1, closing - 1);
	}
	else
	{
		size_t	semicolon = value.find(';');
		if (semicolon != std::string::npos)
			value = value.substr(0, semicolon);
	}
	return Utils::trim(value);
}

static bool	writeToDisk(const std::string& directory,
						const std::string& filename,
						const std::string& content,
						std::string& finalName)
{
	std::string	candidate = filename;
	std::string	path = Utils::joinPath(directory, candidate);

	// Never silently overwrite: add a numeric suffix instead, the way a browser
	// download does.
	if (Utils::pathExists(path))
	{
		size_t		dot = filename.find_last_of('.');
		std::string	stem = (dot == std::string::npos) ? filename : filename.substr(0, dot);
		std::string	extension = (dot == std::string::npos) ? "" : filename.substr(dot);

		for (int counter = 1; counter < 1000; ++counter)
		{
			std::ostringstream	attempt;
			attempt << stem << "-" << counter << extension;
			candidate = attempt.str();
			path = Utils::joinPath(directory, candidate);
			if (!Utils::pathExists(path))
				break ;
		}
		if (Utils::pathExists(path))
			return false;
	}

	std::ofstream	file(path.c_str(), std::ios::binary | std::ios::trunc);
	if (!file.is_open())
	{
		Logger::error("cannot open upload target: " + path);
		return false;
	}
	file.write(content.data(), static_cast<std::streamsize>(content.size()));
	if (!file.good())
	{
		file.close();
		Logger::error("write failed for upload target: " + path);
		return false;
	}
	file.close();

	finalName = candidate;
	return true;
}

// Walks a multipart body, writing each part that carries a filename.
static int	storeMultipart(const std::string& body,
						   const std::string& boundary,
						   const std::string& directory,
						   std::vector<std::string>& stored)
{
	const std::string	delimiter = "--" + boundary;
	size_t				cursor = body.find(delimiter);

	// A body whose first delimiter is missing is not multipart at all.
	if (cursor == std::string::npos)
		return 400;

	while (cursor != std::string::npos)
	{
		cursor += delimiter.size();

		// "--" right after the delimiter marks the end of the whole body.
		if (cursor + 2 <= body.size() && body.compare(cursor, 2, "--") == 0)
			break ;

		// Skip the CRLF that closes the delimiter line.
		if (cursor + 2 <= body.size() && body.compare(cursor, 2, "\r\n") == 0)
			cursor += 2;
		else if (cursor + 1 <= body.size() && body[cursor] == '\n')
			cursor += 1;
		else
			return 400;

		size_t	headerEnd = body.find("\r\n\r\n", cursor);
		size_t	skip = 4;
		if (headerEnd == std::string::npos)
		{
			headerEnd = body.find("\n\n", cursor);
			skip = 2;
		}
		if (headerEnd == std::string::npos)
			return 400;

		const std::string	headerBlock = body.substr(cursor, headerEnd - cursor);
		const size_t		contentStart = headerEnd + skip;

		// The next delimiter bounds this part's content.
		size_t	nextDelimiter = body.find(delimiter, contentStart);
		if (nextDelimiter == std::string::npos)
			return 400;

		size_t	contentEnd = nextDelimiter;
		// The CRLF immediately before a delimiter belongs to the delimiter,
		// not to the content, so trim it before writing the file.
		if (contentEnd >= contentStart + 2 && body.compare(contentEnd - 2, 2, "\r\n") == 0)
			contentEnd -= 2;
		else if (contentEnd >= contentStart + 1 && body[contentEnd - 1] == '\n')
			contentEnd -= 1;

		std::string	disposition;
		std::vector<std::string>	lines = Utils::split(headerBlock, "\n");
		for (size_t i = 0; i < lines.size(); ++i)
		{
			std::string	line = Utils::trim(lines[i]);
			size_t		colon = line.find(':');
			if (colon == std::string::npos)
				continue ;
			if (Utils::toLower(Utils::trim(line.substr(0, colon))) == "content-disposition")
				disposition = Utils::trim(line.substr(colon + 1));
		}

		const std::string	rawName = filenameFromDisposition(disposition);
		if (!rawName.empty())
		{
			const std::string	safeName = sanitiseFilename(rawName);
			if (safeName.empty())
				return 400;

			const std::string	content = body.substr(contentStart, contentEnd - contentStart);
			std::string			finalName;
			if (!writeToDisk(directory, safeName, content, finalName))
				return 500;
			stored.push_back(finalName);
		}
		// Parts without a filename are ordinary form fields and are ignored.

		cursor = nextDelimiter;
	}

	if (stored.empty())
		return 400;
	return 0;
}

int	store(const HttpRequest& request,
		  const std::string& directory,
		  std::vector<std::string>& stored)
{
	const std::string&	body = request.body();

	if (body.empty())
		return 400;

	const std::string	contentType = request.header("content-type");

	if (Utils::toLower(contentType).find("multipart/form-data") != std::string::npos)
	{
		std::string	boundary;
		if (!extractBoundary(contentType, boundary))
		{
			Logger::warn("multipart upload without a boundary parameter");
			return 400;
		}
		return storeMultipart(body, boundary, directory, stored);
	}

	// A non-multipart POST is stored verbatim. The name comes from the final
	// path segment when the client supplied one.
	std::string	name = sanitiseFilename(
		request.path().substr(request.path().find_last_of('/') + 1));
	if (name.empty())
		name = "upload.bin";

	std::string	finalName;
	if (!writeToDisk(directory, name, body, finalName))
		return 500;
	stored.push_back(finalName);
	return 0;
}

}
