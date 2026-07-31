#include "HttpRequest.hpp"
#include "Utils.hpp"

#include <cctype>

namespace
{
	const std::string	EMPTY_STRING = "";

	// Caps that bound how much memory one unauthenticated peer can make the
	// server hold before the request is even understood.
	const size_t	MAX_REQUEST_LINE = 8192;
	const size_t	MAX_HEADER_BYTES = 32768;
	const size_t	MAX_HEADER_COUNT = 100;

	bool	isToken(const std::string& text)
	{
		if (text.empty())
			return false;
		for (size_t i = 0; i < text.size(); ++i)
		{
			unsigned char	c = static_cast<unsigned char>(text[i]);
			if (c <= 32 || c >= 127)
				return false;
			// Separators are not allowed inside an RFC 7230 token.
			if (std::string("()<>@,;:\\\"/[]?={}").find(static_cast<char>(c)) != std::string::npos)
				return false;
		}
		return true;
	}
}

HttpRequest::HttpRequest()
	: _state(PARSING_REQUEST_LINE),
	  _statusCode(0),
	  _cursor(0),
	  _contentLength(0),
	  _chunked(false),
	  _chunkRemaining(0),
	  _maxBodySize(1048576),
	  _headerBytes(0)
{
}

void	HttpRequest::reset(void)
{
	// Anything already received beyond the finished request belongs to the next
	// one on this keep-alive connection, so it is carried over rather than lost.
	std::string	leftover;
	if (_cursor < _buffer.size())
		leftover = _buffer.substr(_cursor);

	_state = PARSING_REQUEST_LINE;
	_statusCode = 0;
	_buffer = leftover;
	_cursor = 0;
	_method.clear();
	_target.clear();
	_path.clear();
	_query.clear();
	_version.clear();
	_headers.clear();
	_body.clear();
	_contentLength = 0;
	_chunked = false;
	_chunkRemaining = 0;
	_headerBytes = 0;
}

bool	HttpRequest::isComplete(void) const
{
	return _state == COMPLETE;
}

bool	HttpRequest::hasFailed(void) const
{
	return _state == FAILED;
}

int	HttpRequest::statusCode(void) const
{
	return _statusCode;
}

const std::string&	HttpRequest::method(void) const	{ return _method; }
const std::string&	HttpRequest::target(void) const	{ return _target; }
const std::string&	HttpRequest::path(void) const	{ return _path; }
const std::string&	HttpRequest::query(void) const	{ return _query; }
const std::string&	HttpRequest::version(void) const{ return _version; }
const std::string&	HttpRequest::body(void) const	{ return _body; }

const std::map<std::string, std::string>&	HttpRequest::headers(void) const
{
	return _headers;
}

bool	HttpRequest::hasHeader(const std::string& name) const
{
	return _headers.find(Utils::toLower(name)) != _headers.end();
}

const std::string&	HttpRequest::header(const std::string& name) const
{
	std::map<std::string, std::string>::const_iterator	found
		= _headers.find(Utils::toLower(name));

	if (found == _headers.end())
		return EMPTY_STRING;
	return found->second;
}

void	HttpRequest::setMaxBodySize(size_t limit)
{
	_maxBodySize = limit;

	if (_state == FAILED || _state == PARSING_REQUEST_LINE)
		return ;

	// The limit can be narrowed after the headers have been read, once the
	// matched location is known. Re-checking here is what makes a per-location
	// limit effective for a body that is already declared or partly buffered.
	if (_contentLength > limit || _body.size() > limit)
		failWith(413);
}

bool	HttpRequest::headersParsed(void) const
{
	return _state != PARSING_REQUEST_LINE && _state != PARSING_HEADERS;
}

bool	HttpRequest::failWith(int code)
{
	_state = FAILED;
	_statusCode = code;
	return false;
}

bool	HttpRequest::takeLine(std::string& line)
{
	size_t	position = _buffer.find("\r\n", _cursor);

	if (position == std::string::npos)
		return false;
	line = _buffer.substr(_cursor, position - _cursor);
	_cursor = position + 2;
	return true;
}

bool	HttpRequest::consume(const char* data, size_t length)
{
	if (_state == FAILED)
		return false;
	if (data && length > 0)
		_buffer.append(data, length);

	// Each helper returns false when it needs more bytes, which ends the pass
	// without treating "incomplete" as "broken".
	while (true)
	{
		switch (_state)
		{
			case PARSING_REQUEST_LINE:
				if (!parseRequestLine())
					return _state != FAILED;
				break ;
			case PARSING_HEADERS:
				if (!parseHeaderBlock())
					return _state != FAILED;
				break ;
			case PARSING_BODY:
				if (!readFixedBody())
					return _state != FAILED;
				break ;
			case PARSING_CHUNK_SIZE:
				if (!readChunkSize())
					return _state != FAILED;
				break ;
			case PARSING_CHUNK_DATA:
				if (!readChunkData())
					return _state != FAILED;
				break ;
			case PARSING_CHUNK_TRAILER:
				if (!readChunkTrailer())
					return _state != FAILED;
				break ;
			case COMPLETE:
			case FAILED:
				return _state == COMPLETE;
		}
	}
}

bool	HttpRequest::parseRequestLine(void)
{
	// Tolerate leading empty lines: RFC 7230 lets a client send a stray CRLF
	// before the request line, and some proxies do.
	while (_buffer.size() >= _cursor + 2 && _buffer.compare(_cursor, 2, "\r\n") == 0)
		_cursor += 2;

	std::string	line;
	if (!takeLine(line))
	{
		if (_buffer.size() - _cursor > MAX_REQUEST_LINE)
			return failWith(414);
		return false;
	}
	if (line.size() > MAX_REQUEST_LINE)
		return failWith(414);

	size_t	firstSpace = line.find(' ');
	if (firstSpace == std::string::npos)
		return failWith(400);
	size_t	secondSpace = line.find(' ', firstSpace + 1);
	if (secondSpace == std::string::npos)
		return failWith(400);
	// A third space means the target contained an unescaped one.
	if (line.find(' ', secondSpace + 1) != std::string::npos)
		return failWith(400);

	_method = line.substr(0, firstSpace);
	_target = line.substr(firstSpace + 1, secondSpace - firstSpace - 1);
	_version = line.substr(secondSpace + 1);

	if (!isToken(_method))
		return failWith(400);
	if (_target.empty())
		return failWith(400);
	if (_version != "HTTP/1.1" && _version != "HTTP/1.0")
		return failWith(505);
	if (!splitTarget())
		return false;

	_state = PARSING_HEADERS;
	return true;
}

bool	HttpRequest::splitTarget(void)
{
	std::string	rawPath = _target;

	// An absolute-form target ("GET http://host/path") is legal for proxies;
	// strip the authority so routing only ever sees an origin-form path.
	if (Utils::startsWith(rawPath, "http://") || Utils::startsWith(rawPath, "https://"))
	{
		size_t	schemeEnd = rawPath.find("://");
		size_t	slash = rawPath.find('/', schemeEnd + 3);
		rawPath = (slash == std::string::npos) ? "/" : rawPath.substr(slash);
	}

	size_t	questionMark = rawPath.find('?');
	if (questionMark != std::string::npos)
	{
		_query = rawPath.substr(questionMark + 1);
		rawPath = rawPath.substr(0, questionMark);
	}
	if (rawPath.empty() || rawPath[0] != '/')
		return failWith(400);

	std::string	decoded;
	if (!Utils::percentDecode(rawPath, decoded))
		return failWith(400);
	// A decoded NUL would truncate every later C-string path operation.
	if (decoded.find('\0') != std::string::npos)
		return failWith(400);

	std::string	normalised;
	// Decoding first and normalising second is deliberate: "%2e%2e%2f" has to
	// be recognised as "../" before the traversal check can reject it.
	if (!Utils::normalisePath(decoded, normalised))
		return failWith(400);

	_path = normalised;
	return true;
}

bool	HttpRequest::parseHeaderBlock(void)
{
	std::string	line;

	while (takeLine(line))
	{
		if (line.empty())
		{
			// Every request must carry Host, which is how virtual hosting and
			// absolute URI reconstruction work.
			if (_version == "HTTP/1.1" && !hasHeader("host"))
				return failWith(400);
			return prepareBody();
		}

		_headerBytes += line.size();
		if (_headerBytes > MAX_HEADER_BYTES || _headers.size() > MAX_HEADER_COUNT)
			return failWith(431);

		// Obsolete line folding: a header continued onto an indented line.
		// It is deprecated and a well-known request-smuggling vector.
		if (line[0] == ' ' || line[0] == '\t')
			return failWith(400);

		size_t	colon = line.find(':');
		if (colon == std::string::npos || colon == 0)
			return failWith(400);

		std::string	name = line.substr(0, colon);
		if (!isToken(name))
			return failWith(400);

		// The space after the colon is optional whitespace, not a requirement.
		// Rejecting "Host:localhost" was a genuine bug in the original parser.
		std::string	value = Utils::trim(line.substr(colon + 1));
		name = Utils::toLower(name);

		std::map<std::string, std::string>::iterator	existing = _headers.find(name);
		if (existing == _headers.end())
			_headers[name] = value;
		else if (name == "host" || name == "content-length")
			// Duplicates of these two change how the message is framed, so a
			// conflicting repeat is a smuggling attempt, not a merge.
			return failWith(400);
		else
			existing->second += ", " + value;
	}

	if (_buffer.size() - _cursor > MAX_HEADER_BYTES)
		return failWith(431);
	return false;
}

bool	HttpRequest::prepareBody(void)
{
	const bool	hasLength = hasHeader("content-length");
	const bool	hasEncoding = hasHeader("transfer-encoding");

	// Accepting both at once lets a proxy and an origin disagree about where
	// the body ends, which is the classic request-smuggling primitive.
	if (hasLength && hasEncoding)
		return failWith(400);

	if (hasEncoding)
	{
		if (Utils::toLower(header("transfer-encoding")) != "chunked")
			return failWith(501);
		_chunked = true;
		_state = PARSING_CHUNK_SIZE;
		return true;
	}

	if (hasLength)
	{
		if (!Utils::parseSizeT(header("content-length"), _contentLength))
			return failWith(400);
		if (_contentLength > _maxBodySize)
			return failWith(413);
		if (_contentLength == 0)
		{
			_state = COMPLETE;
			return true;
		}
		_body.reserve(_contentLength);
		_state = PARSING_BODY;
		return true;
	}

	// No framing headers means no body. A POST without either is ambiguous.
	if (_method == "POST" || _method == "PUT")
		return failWith(411);

	_state = COMPLETE;
	return true;
}

bool	HttpRequest::readFixedBody(void)
{
	size_t	available = _buffer.size() - _cursor;
	size_t	needed = _contentLength - _body.size();
	size_t	take = (available < needed) ? available : needed;

	if (take > 0)
	{
		_body.append(_buffer, _cursor, take);
		_cursor += take;
	}
	if (_body.size() < _contentLength)
		return false;

	_state = COMPLETE;
	return true;
}

bool	HttpRequest::readChunkSize(void)
{
	std::string	line;

	if (!takeLine(line))
	{
		if (_buffer.size() - _cursor > 1024)
			return failWith(400);
		return false;
	}

	// A chunk-size line may carry extensions after a semicolon: "1a;name=value".
	size_t	semicolon = line.find(';');
	if (semicolon != std::string::npos)
		line = line.substr(0, semicolon);

	line = Utils::trim(line);

	size_t	size = 0;
	if (!Utils::parseHex(line, size))
		return failWith(400);

	if (size == 0)
	{
		_state = PARSING_CHUNK_TRAILER;
		return true;
	}
	// Checked per chunk so a hostile client cannot stream past the limit.
	if (_body.size() + size > _maxBodySize)
		return failWith(413);

	_chunkRemaining = size;
	_state = PARSING_CHUNK_DATA;
	return true;
}

bool	HttpRequest::readChunkData(void)
{
	size_t	available = _buffer.size() - _cursor;
	size_t	take = (available < _chunkRemaining) ? available : _chunkRemaining;

	if (take > 0)
	{
		_body.append(_buffer, _cursor, take);
		_cursor += take;
		_chunkRemaining -= take;
	}
	if (_chunkRemaining > 0)
		return false;

	// Each chunk's data is followed by its own CRLF.
	if (_buffer.size() < _cursor + 2)
		return false;
	if (_buffer.compare(_cursor, 2, "\r\n") != 0)
		return failWith(400);
	_cursor += 2;

	_state = PARSING_CHUNK_SIZE;
	return true;
}

bool	HttpRequest::readChunkTrailer(void)
{
	std::string	line;

	// Trailers are optional headers after the last chunk; read until the blank
	// line that closes the message.
	while (takeLine(line))
	{
		if (line.empty())
		{
			_state = COMPLETE;
			return true;
		}
		_headerBytes += line.size();
		if (_headerBytes > MAX_HEADER_BYTES)
			return failWith(431);
	}
	return false;
}

bool	HttpRequest::wantsKeepAlive(void) const
{
	const std::string	connection = Utils::toLower(header("connection"));

	if (connection.find("close") != std::string::npos)
		return false;
	if (_version == "HTTP/1.0")
		// 1.0 defaults to closing unless the client opts in.
		return connection.find("keep-alive") != std::string::npos;
	return true;
}

bool	HttpRequest::expectsContinue(void) const
{
	return Utils::toLower(header("expect")) == "100-continue";
}
