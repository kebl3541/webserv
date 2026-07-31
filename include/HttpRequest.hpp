#ifndef HTTPREQUEST_HPP
# define HTTPREQUEST_HPP

# include <map>
# include <string>

// Incremental HTTP/1.1 request parser.
//
// The parser is fed whatever bytes recv() happened to return and keeps its own
// state between calls, so a request split across many TCP segments costs the
// same as one delivered in a single read. The original implementation instead
// re-scanned the entire accumulated buffer on every wake-up, which is O(n^2)
// in the number of segments and is exactly what a slow-drip client exploits.
class HttpRequest
{
	public:
		enum State
		{
			PARSING_REQUEST_LINE,
			PARSING_HEADERS,
			PARSING_BODY,
			PARSING_CHUNK_SIZE,
			PARSING_CHUNK_DATA,
			PARSING_CHUNK_TRAILER,
			COMPLETE,
			FAILED
		};

		HttpRequest();

		// Appends raw bytes and advances the state machine as far as it can.
		// Returns false once the request is malformed; statusCode() then holds
		// the response the caller should send.
		bool	consume(const char* data, size_t length);

		void	reset(void);

		bool	isComplete(void) const;
		bool	hasFailed(void) const;
		int		statusCode(void) const;

		const std::string&	method(void) const;
		const std::string&	target(void) const;		// raw request target
		const std::string&	path(void) const;		// decoded, normalised
		const std::string&	query(void) const;
		const std::string&	version(void) const;
		const std::string&	body(void) const;

		// Header lookup is case-insensitive: names are lowercased on the way in.
		bool				hasHeader(const std::string& name) const;
		const std::string&	header(const std::string& name) const;

		const std::map<std::string, std::string>&	headers(void) const;

		bool	wantsKeepAlive(void) const;
		bool	expectsContinue(void) const;

		// Enforced while the body streams in, so an oversized upload is cut off
		// early instead of after it has all been buffered. Lowering the limit
		// re-checks what has already been read, so a stricter per-location
		// limit applied mid-request still rejects an oversized body.
		void	setMaxBodySize(size_t limit);

		// True once the header block has been read, which is the earliest point
		// at which the request's path is known.
		bool	headersParsed(void) const;

	private:
		bool	parseRequestLine(void);
		bool	parseHeaderBlock(void);
		bool	prepareBody(void);
		bool	readFixedBody(void);
		bool	readChunkSize(void);
		bool	readChunkData(void);
		bool	readChunkTrailer(void);

		bool	failWith(int code);
		bool	splitTarget(void);

		// Locates a CRLF-terminated line in the pending buffer.
		bool	takeLine(std::string& line);

		State		_state;
		int			_statusCode;

		std::string	_buffer;		// bytes received but not yet consumed
		size_t		_cursor;		// how far into _buffer the parser has read

		std::string	_method;
		std::string	_target;
		std::string	_path;
		std::string	_query;
		std::string	_version;

		std::map<std::string, std::string>	_headers;

		std::string	_body;
		size_t		_contentLength;
		bool		_chunked;
		size_t		_chunkRemaining;
		size_t		_maxBodySize;
		size_t		_headerBytes;
};

#endif
