#ifndef UTILS_HPP
# define UTILS_HPP

# include <string>
# include <vector>
# include <cstddef>

namespace Utils
{
	std::string	toString(long value);
	std::string	toString(size_t value);

	// Strict conversions: they report failure instead of silently yielding 0,
	// which matters when a config value or a chunk size is malformed.
	bool	parseLong(const std::string& text, long& out);
	bool	parseSizeT(const std::string& text, size_t& out);
	bool	parseHex(const std::string& text, size_t& out);

	std::string	toLower(const std::string& text);
	std::string	trim(const std::string& text);

	bool	startsWith(const std::string& text, const std::string& prefix);
	bool	endsWith(const std::string& text, const std::string& suffix);

	std::vector<std::string>	split(const std::string& text, const std::string& separators);

	// Percent-decoding for request targets and query strings. Returns false on
	// a malformed escape so the caller can answer 400 rather than guess.
	bool	percentDecode(const std::string& text, std::string& out);

	// Resolves "." and ".." inside a URI path without touching the filesystem.
	// Returns false when the path escapes above its own root, which is how
	// directory traversal is rejected before any file is opened.
	bool	normalisePath(const std::string& path, std::string& out);

	// RFC 7231 IMF-fixdate, always in GMT: "Sun, 06 Nov 1994 08:49:37 GMT".
	std::string	httpDate(void);

	bool	isDirectory(const std::string& path);
	bool	isRegularFile(const std::string& path);
	bool	pathExists(const std::string& path);

	// Joins two path fragments with exactly one separator between them.
	std::string	joinPath(const std::string& left, const std::string& right);
}

#endif
