#include "Utils.hpp"

#include <sys/stat.h>
#include <limits.h>
#include <stdlib.h>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace Utils
{

std::string	toString(long value)
{
	std::ostringstream	stream;

	stream << value;
	return stream.str();
}

std::string	toString(size_t value)
{
	std::ostringstream	stream;

	stream << value;
	return stream.str();
}

bool	parseLong(const std::string& text, long& out)
{
	if (text.empty())
		return false;

	size_t	index = 0;
	bool	negative = false;

	if (text[0] == '+' || text[0] == '-')
	{
		negative = (text[0] == '-');
		index = 1;
		if (text.size() == 1)
			return false;
	}

	long	result = 0;
	while (index < text.size())
	{
		if (!std::isdigit(static_cast<unsigned char>(text[index])))
			return false;
		int	digit = text[index] - '0';
		// Reject anything that would wrap around, rather than storing garbage.
		if (result > (2147483647L - digit) / 10)
			return false;
		result = result * 10 + digit;
		++index;
	}
	out = negative ? -result : result;
	return true;
}

bool	parseSizeT(const std::string& text, size_t& out)
{
	long	value = 0;

	if (!parseLong(text, value) || value < 0)
		return false;
	out = static_cast<size_t>(value);
	return true;
}

bool	parseHex(const std::string& text, size_t& out)
{
	if (text.empty() || text.size() > 16)
		return false;

	size_t	result = 0;
	for (size_t i = 0; i < text.size(); ++i)
	{
		char	c = text[i];
		int		digit;

		if (c >= '0' && c <= '9')
			digit = c - '0';
		else if (c >= 'a' && c <= 'f')
			digit = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			digit = c - 'A' + 10;
		else
			return false;
		result = result * 16 + static_cast<size_t>(digit);
	}
	out = result;
	return true;
}

std::string	toLower(const std::string& text)
{
	std::string	result(text);

	for (size_t i = 0; i < result.size(); ++i)
		result[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(result[i])));
	return result;
}

std::string	trim(const std::string& text)
{
	const std::string	whitespace = " \t\r\n";
	size_t				start = text.find_first_not_of(whitespace);

	if (start == std::string::npos)
		return "";
	size_t	end = text.find_last_not_of(whitespace);
	return text.substr(start, end - start + 1);
}

bool	startsWith(const std::string& text, const std::string& prefix)
{
	if (prefix.size() > text.size())
		return false;
	return text.compare(0, prefix.size(), prefix) == 0;
}

bool	endsWith(const std::string& text, const std::string& suffix)
{
	if (suffix.size() > text.size())
		return false;
	return text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string>	split(const std::string& text, const std::string& separators)
{
	std::vector<std::string>	tokens;
	size_t						start = 0;

	while (start < text.size())
	{
		size_t	position = text.find_first_of(separators, start);
		if (position == std::string::npos)
		{
			tokens.push_back(text.substr(start));
			break ;
		}
		if (position > start)
			tokens.push_back(text.substr(start, position - start));
		start = position + 1;
	}
	return tokens;
}

bool	percentDecode(const std::string& text, std::string& out)
{
	std::string	result;

	result.reserve(text.size());
	for (size_t i = 0; i < text.size(); ++i)
	{
		if (text[i] != '%')
		{
			result += text[i];
			continue ;
		}
		// A '%' must be followed by exactly two hex digits.
		if (i + 2 >= text.size())
			return false;
		size_t	value = 0;
		if (!parseHex(text.substr(i + 1, 2), value))
			return false;
		result += static_cast<char>(value);
		i += 2;
	}
	out = result;
	return true;
}

bool	normalisePath(const std::string& path, std::string& out)
{
	std::vector<std::string>	segments;
	size_t						index = 0;
	// Whether the result is anchored at the filesystem root has to be recorded
	// up front: a URI path is always absolute, but a configured root such as
	// "./www" is relative, and turning it into "/www" would point at an
	// entirely different directory.
	const bool					absolute = (!path.empty() && path[0] == '/');

	while (index < path.size())
	{
		while (index < path.size() && path[index] == '/')
			++index;
		if (index >= path.size())
			break ;

		size_t		end = path.find('/', index);
		std::string	segment = (end == std::string::npos)
			? path.substr(index)
			: path.substr(index, end - index);

		if (segment == ".")
			; // A single dot refers to the current directory: drop it.
		else if (segment == "..")
		{
			// Refusing to pop past the root is what stops "/../../etc/passwd".
			if (segments.empty())
				return false;
			segments.pop_back();
		}
		else
			segments.push_back(segment);

		if (end == std::string::npos)
			break ;
		index = end + 1;
	}

	std::string	result = absolute ? "/" : "";
	for (size_t i = 0; i < segments.size(); ++i)
	{
		result += segments[i];
		if (i + 1 < segments.size())
			result += "/";
	}
	// A relative path that reduced to nothing refers to the current directory.
	if (!absolute && result.empty())
		result = ".";
	// A trailing slash in the input is meaningful: it distinguishes a request
	// for a directory from a request for a file of the same name.
	if (!segments.empty() && !path.empty() && path[path.size() - 1] == '/')
		result += "/";
	out = result;
	return true;
}

std::string	httpDate(void)
{
	static const char*	days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
	static const char*	months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
									"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

	std::time_t	now = std::time(NULL);
	std::tm*	gmt = std::gmtime(&now);

	if (!gmt)
		return "Thu, 01 Jan 1970 00:00:00 GMT";

	// Built by hand rather than with strftime because strftime's day and month
	// names follow the current locale, while HTTP requires the English ones
	// regardless of where the server runs.
	std::ostringstream	out;

	out << days[gmt->tm_wday] << ", "
		<< std::setw(2) << std::setfill('0') << gmt->tm_mday << " "
		<< months[gmt->tm_mon] << " "
		<< std::setw(4) << std::setfill('0') << (gmt->tm_year + 1900) << " "
		<< std::setw(2) << std::setfill('0') << gmt->tm_hour << ":"
		<< std::setw(2) << std::setfill('0') << gmt->tm_min << ":"
		<< std::setw(2) << std::setfill('0') << gmt->tm_sec << " GMT";
	return out.str();
}

bool	isDirectory(const std::string& path)
{
	struct stat	info;

	if (stat(path.c_str(), &info) != 0)
		return false;
	return S_ISDIR(info.st_mode);
}

bool	isRegularFile(const std::string& path)
{
	struct stat	info;

	if (stat(path.c_str(), &info) != 0)
		return false;
	return S_ISREG(info.st_mode);
}

bool	pathExists(const std::string& path)
{
	struct stat	info;

	return stat(path.c_str(), &info) == 0;
}

bool	resolveReal(const std::string& path, std::string& out)
{
	char	buffer[PATH_MAX];

	if (realpath(path.c_str(), buffer) != NULL)
	{
		out = buffer;
		return true;
	}

	// realpath fails on anything that does not exist, which includes the target
	// of an upload. Resolving the parent and re-attaching the final component
	// still pins the result to a real directory, which is what the containment
	// check needs.
	size_t	slash = path.find_last_of('/');
	if (slash == std::string::npos)
		return false;

	const std::string	parent = (slash == 0) ? "/" : path.substr(0, slash);
	const std::string	leaf = path.substr(slash + 1);

	if (realpath(parent.c_str(), buffer) == NULL)
		return false;

	out = joinPath(std::string(buffer), leaf);
	return true;
}

std::string	htmlEscape(const std::string& text)
{
	std::string	result;

	result.reserve(text.size());
	for (size_t i = 0; i < text.size(); ++i)
	{
		switch (text[i])
		{
			case '&':  result += "&amp;";  break ;
			case '<':  result += "&lt;";   break ;
			case '>':  result += "&gt;";   break ;
			case '"':  result += "&quot;"; break ;
			case '\'': result += "&#39;";  break ;
			default:   result += text[i];  break ;
		}
	}
	return result;
}

std::string	uriEncode(const std::string& text)
{
	static const char*	hex = "0123456789ABCDEF";
	std::string			result;

	result.reserve(text.size());
	for (size_t i = 0; i < text.size(); ++i)
	{
		unsigned char	c = static_cast<unsigned char>(text[i]);

		// The unreserved set from RFC 3986, plus '/' so that path separators
		// inside a link survive intact.
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
			|| (c >= '0' && c <= '9')
			|| c == '-' || c == '_' || c == '.' || c == '~' || c == '/')
			result += static_cast<char>(c);
		else
		{
			result += '%';
			result += hex[(c >> 4) & 0x0F];
			result += hex[c & 0x0F];
		}
	}
	return result;
}

std::string	joinPath(const std::string& left, const std::string& right)
{
	if (left.empty())
		return right;
	if (right.empty())
		return left;

	bool	leftSlash = (left[left.size() - 1] == '/');
	bool	rightSlash = (right[0] == '/');

	if (leftSlash && rightSlash)
		return left + right.substr(1);
	if (!leftSlash && !rightSlash)
		return left + "/" + right;
	return left + right;
}

}
