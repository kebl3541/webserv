#include "HttpResponse.hpp"
#include "Utils.hpp"

#include <sstream>

HttpResponse::HttpResponse()
	: _status(200),
	  _keepAlive(true),
	  _headOnly(false)
{
}

HttpResponse::HttpResponse(int status)
	: _status(status),
	  _keepAlive(true),
	  _headOnly(false)
{
}

void	HttpResponse::setStatus(int status)
{
	_status = status;
}

int	HttpResponse::status(void) const
{
	return _status;
}

void	HttpResponse::setHeader(const std::string& name, const std::string& value)
{
	_headers[name] = value;
}

void	HttpResponse::addHeader(const std::string& name, const std::string& value)
{
	_repeatedHeaders.push_back(std::make_pair(name, value));
}

void	HttpResponse::setBody(const std::string& body, const std::string& contentType)
{
	_body = body;
	if (!contentType.empty())
		_headers["Content-Type"] = contentType;
}

void	HttpResponse::setKeepAlive(bool keepAlive)
{
	_keepAlive = keepAlive;
}

void	HttpResponse::setHeadOnly(bool headOnly)
{
	_headOnly = headOnly;
}

const char*	HttpResponse::reasonPhrase(int status)
{
	switch (status)
	{
		case 100: return "Continue";
		case 200: return "OK";
		case 201: return "Created";
		case 204: return "No Content";
		case 206: return "Partial Content";
		case 301: return "Moved Permanently";
		case 302: return "Found";
		case 303: return "See Other";
		case 304: return "Not Modified";
		case 307: return "Temporary Redirect";
		case 308: return "Permanent Redirect";
		case 400: return "Bad Request";
		case 403: return "Forbidden";
		case 404: return "Not Found";
		case 405: return "Method Not Allowed";
		case 408: return "Request Timeout";
		case 409: return "Conflict";
		case 411: return "Length Required";
		case 413: return "Content Too Large";
		case 414: return "URI Too Long";
		case 415: return "Unsupported Media Type";
		case 431: return "Request Header Fields Too Large";
		case 500: return "Internal Server Error";
		case 501: return "Not Implemented";
		case 502: return "Bad Gateway";
		case 503: return "Service Unavailable";
		case 504: return "Gateway Timeout";
		case 505: return "HTTP Version Not Supported";
		default:  return "Unknown";
	}
}

std::string	HttpResponse::defaultErrorBody(int status)
{
	std::ostringstream	html;
	const char*			reason = reasonPhrase(status);

	html << "<!DOCTYPE html>\n"
		 << "<html lang=\"en\">\n"
		 << "<head>\n"
		 << "  <meta charset=\"utf-8\">\n"
		 << "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
		 << "  <title>" << status << " " << reason << "</title>\n"
		 << "  <style>\n"
		 << "    body { margin: 0; min-height: 100vh; display: flex;\n"
		 << "           align-items: center; justify-content: center;\n"
		 << "           font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI',\n"
		 << "                        Roboto, Helvetica, Arial, sans-serif;\n"
		 << "           background: #0f1115; color: #e6e8eb; }\n"
		 << "    main { text-align: center; padding: 2rem; }\n"
		 << "    h1 { font-size: 5rem; margin: 0; font-weight: 600;\n"
		 << "         letter-spacing: -0.04em; }\n"
		 << "    p { font-size: 1.1rem; color: #9aa3ad; margin: 0.5rem 0 0; }\n"
		 << "    hr { width: 3rem; border: 0; border-top: 2px solid #2a2f36;\n"
		 << "         margin: 1.5rem auto; }\n"
		 << "    small { color: #6b7280; }\n"
		 << "  </style>\n"
		 << "</head>\n"
		 << "<body>\n"
		 << "  <main>\n"
		 << "    <h1>" << status << "</h1>\n"
		 << "    <p>" << reason << "</p>\n"
		 << "    <hr>\n"
		 << "    <small>webserv</small>\n"
		 << "  </main>\n"
		 << "</body>\n"
		 << "</html>\n";
	return html.str();
}

std::string	HttpResponse::serialise(void) const
{
	std::ostringstream	message;

	message << "HTTP/1.1 " << _status << " " << reasonPhrase(_status) << "\r\n";

	// Headers every response carries. Date and Server are set here rather than
	// by callers so they can never be forgotten.
	message << "Date: " << Utils::httpDate() << "\r\n";
	message << "Server: webserv/1.0\r\n";

	// 204 and 1xx are defined as having no body at all, so emitting
	// Content-Length for them would be a framing error.
	const bool	bodyAllowed = (_status != 204 && (_status < 100 || _status > 199));
	if (bodyAllowed)
		message << "Content-Length: " << _body.size() << "\r\n";

	message << "Connection: " << (_keepAlive ? "keep-alive" : "close") << "\r\n";

	for (std::map<std::string, std::string>::const_iterator it = _headers.begin();
		 it != _headers.end(); ++it)
	{
		// Skip anything already emitted above so no header appears twice.
		if (it->first == "Date" || it->first == "Server"
			|| it->first == "Content-Length" || it->first == "Connection")
			continue ;
		message << it->first << ": " << it->second << "\r\n";
	}

	for (size_t i = 0; i < _repeatedHeaders.size(); ++i)
		message << _repeatedHeaders[i].first << ": "
				<< _repeatedHeaders[i].second << "\r\n";

	message << "\r\n";

	// HEAD keeps the headers a GET would produce but sends no body, which is
	// why the length is computed before this point.
	if (bodyAllowed && !_headOnly)
		message << _body;

	return message.str();
}
