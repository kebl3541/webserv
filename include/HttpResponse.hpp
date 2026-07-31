#ifndef HTTPRESPONSE_HPP
# define HTTPRESPONSE_HPP

# include <map>
# include <string>

// Builds a well-formed HTTP/1.1 response.
//
// The class owns header ordering and the Content-Length calculation so that no
// call site can produce a message whose framing disagrees with its body: that
// mismatch is what desynchronises a keep-alive connection.
class HttpResponse
{
	public:
		HttpResponse();
		explicit HttpResponse(int status);

		void	setStatus(int status);
		int		status(void) const;

		void	setHeader(const std::string& name, const std::string& value);
		void	setBody(const std::string& body, const std::string& contentType);
		void	setKeepAlive(bool keepAlive);

		// Suppresses the body while keeping Content-Length, which is what a
		// correct HEAD response looks like.
		void	setHeadOnly(bool headOnly);

		std::string	serialise(void) const;

		// Canonical reason phrase, used both here and by the error-page builder.
		static const char*	reasonPhrase(int status);

		// A ready-made HTML error page, used when the config supplies none.
		static std::string	defaultErrorBody(int status);

	private:
		int									_status;
		std::map<std::string, std::string>	_headers;
		std::string							_body;
		bool								_keepAlive;
		bool								_headOnly;
};

#endif
