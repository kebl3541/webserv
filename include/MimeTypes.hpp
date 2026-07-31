#ifndef MIMETYPES_HPP
# define MIMETYPES_HPP

# include <map>
# include <string>

// Maps a filename extension to a media type. The original implementation
// sniffed magic bytes instead, which cannot distinguish CSS from plain text
// and so made browsers refuse stylesheets under strict MIME checking.
class MimeTypes
{
	public:
		static const std::string&	forPath(const std::string& path);

	private:
		MimeTypes();
		MimeTypes(const MimeTypes& other);
		MimeTypes&	operator=(const MimeTypes& other);

		static const std::map<std::string, std::string>&	table(void);
};

#endif
