#ifndef UPLOAD_HPP
# define UPLOAD_HPP

# include <string>
# include <vector>

class HttpRequest;

// Writes request bodies to disk.
//
// The multipart reader is the part of the original server that could be
// crashed remotely: it called strstr() for the boundary without checking for
// NULL and then indexed a vector of boundary offsets without checking its
// size, so a Content-Type of "multipart/form-data" with no boundary parameter
// killed the process. Every step here validates before it indexes.
namespace Upload
{
	// Returns 0 on success, or the HTTP status to answer with.
	// `stored` receives the names of the files that were written.
	int	store(const HttpRequest& request,
			  const std::string& directory,
			  std::vector<std::string>& stored);

	// Extracts the boundary parameter from a Content-Type header.
	// Returns false when the header carries none.
	bool	extractBoundary(const std::string& contentType, std::string& boundary);

	// Strips any directory component from a client-supplied filename, so an
	// upload named "../../etc/passwd" cannot escape the upload directory.
	std::string	sanitiseFilename(const std::string& name);
}

#endif
