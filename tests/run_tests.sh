#!/usr/bin/env bash
#
# Integration tests for webserv.
#
# Every case drives the real binary over a real socket. Several of them are
# regression tests for defects that existed in the first version of this
# project and are named accordingly, so a reintroduced bug fails loudly.

set -u

readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly HOST="127.0.0.1"
readonly PORT="8080"
readonly PORT2="8081"
readonly BASE="http://${HOST}:${PORT}"
readonly BINARY="${ROOT}/webserv"
readonly CONFIG="${ROOT}/conf/default.conf"
readonly LOG="$(mktemp -t webserv-test.XXXXXX)"

PASSED=0
FAILED=0
SERVER_PID=""

readonly GREEN=$'\033[32m'
readonly RED=$'\033[31m'
readonly GREY=$'\033[90m'
readonly BOLD=$'\033[1m'
readonly RESET=$'\033[0m'

cleanup() {
	if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
		kill "${SERVER_PID}" 2>/dev/null
		wait "${SERVER_PID}" 2>/dev/null
	fi
	rm -f "${LOG}"
}
trap cleanup EXIT

pass() {
	PASSED=$((PASSED + 1))
	printf '  %s✓%s %s\n' "${GREEN}" "${RESET}" "$1"
}

fail() {
	FAILED=$((FAILED + 1))
	printf '  %s✗%s %s\n' "${RED}" "${RESET}" "$1"
	printf '      expected: %s\n' "$2"
	printf '      actual:   %s\n' "$3"
}

check() {
	local name="$1" expected="$2" actual="$3"
	if [[ "${expected}" == "${actual}" ]]; then
		pass "${name}"
	else
		fail "${name}" "${expected}" "${actual}"
	fi
}

check_contains() {
	local name="$1" needle="$2" haystack="$3"
	if [[ "${haystack}" == *"${needle}"* ]]; then
		pass "${name}"
	else
		fail "${name}" "output containing '${needle}'" "$(printf '%s' "${haystack}" | head -c 200)"
	fi
}

section() {
	printf '\n%s%s%s\n' "${BOLD}" "$1" "${RESET}"
}

# Status code only.
status() {
	curl -s -o /dev/null -w '%{http_code}' --max-time 10 "$@"
}

# Sends a raw request over a socket and returns whatever comes back. Used where
# curl would refuse to build a deliberately malformed message. The helper
# applies its own socket timeout, so a keep-alive response that leaves the
# connection open cannot stall the suite.
raw() {
	printf '%b' "$1" | python3 "${ROOT}/tests/rawsend.py" "${HOST}" "${PORT}" --timeout 3 2>/dev/null
}

start_server() {
	if [[ ! -x "${BINARY}" ]]; then
		printf '%sbinary not found at %s; run make first%s\n' "${RED}" "${BINARY}" "${RESET}"
		exit 1
	fi

	cd "${ROOT}" || exit 1
	WEBSERV_LOG=warn "${BINARY}" "${CONFIG}" >"${LOG}" 2>&1 &
	SERVER_PID=$!

	for _ in $(seq 1 40); do
		if curl -s -o /dev/null --max-time 1 "${BASE}/" 2>/dev/null; then
			return 0
		fi
		if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
			printf '%sserver exited during startup%s\n' "${RED}" "${RESET}"
			cat "${LOG}"
			exit 1
		fi
		sleep 0.25
	done

	printf '%sserver did not become ready%s\n' "${RED}" "${RESET}"
	cat "${LOG}"
	exit 1
}

alive() {
	kill -0 "${SERVER_PID}" 2>/dev/null
}

printf '%swebserv integration tests%s\n' "${BOLD}" "${RESET}"
printf '%s%s%s\n' "${GREY}" "${BASE}" "${RESET}"

start_server

# ---------------------------------------------------------------------------
section 'Static files'

check 'GET / returns 200'                200 "$(status "${BASE}/")"
check 'GET a missing file returns 404'   404 "$(status "${BASE}/no-such-file")"
check 'GET /files/ lists the directory'  200 "$(status "${BASE}/files/")"
check 'GET a file in that listing'       200 "$(status "${BASE}/files/notes.txt")"
check 'alias routing resolves'           200 "$(status "${BASE}/docs/readme.html")"
check 'configured redirect returns 301'  301 "$(status "${BASE}/old-page")"

check_contains 'directory listing names its entries' 'notes.txt' \
	"$(curl -s --max-time 10 "${BASE}/files/")"

# ---------------------------------------------------------------------------
section 'MIME types (regression: content was sniffed, not mapped)'

check_contains 'html is served as text/html' 'text/html' \
	"$(curl -s -D- -o /dev/null --max-time 10 "${BASE}/" | tr -d '\r')"
check_contains 'txt is served as text/plain' 'text/plain' \
	"$(curl -s -D- -o /dev/null --max-time 10 "${BASE}/files/notes.txt" | tr -d '\r')"

# ---------------------------------------------------------------------------
section 'Response headers'

headers="$(curl -s -D- -o /dev/null --max-time 10 "${BASE}/" | tr -d '\r')"

# The original emitted asctime() in local time, which no HTTP client may parse.
if [[ "${headers}" =~ Date:\ [A-Z][a-z]{2},\ [0-9]{2}\ [A-Z][a-z]{2}\ [0-9]{4}\ [0-9]{2}:[0-9]{2}:[0-9]{2}\ GMT ]]; then
	pass 'Date uses RFC 7231 IMF-fixdate in GMT'
else
	fail 'Date uses RFC 7231 IMF-fixdate in GMT' 'Date: Sun, 06 Nov 1994 08:49:37 GMT' \
		"$(printf '%s' "${headers}" | grep -i '^date' || echo 'no Date header')"
fi

check_contains 'Content-Length is present' 'Content-Length:' "${headers}"
check_contains 'Server is identified'      'Server:'         "${headers}"

# ---------------------------------------------------------------------------
section 'Methods'

check 'HEAD is supported (regression: answered 405)' 200 "$(status -I "${BASE}/")"

head_body="$(curl -s --max-time 10 -I "${BASE}/" | tail -c 20)"
if [[ -z "$(curl -s --max-time 10 --head "${BASE}/" | sed -n '/^\r*$/,$p' | tail -n +2)" ]]; then
	pass 'HEAD sends headers but no body'
else
	fail 'HEAD sends headers but no body' 'empty body' "${head_body}"
fi

check 'an unimplemented method returns 501' 501 \
	"$(status -X BREW "${BASE}/")"
check 'a disallowed method returns 405' 405 \
	"$(status -X DELETE "${BASE}/index.html")"

check_contains '405 advertises the Allow header' 'Allow:' \
	"$(curl -s -D- -o /dev/null --max-time 10 -X DELETE "${BASE}/index.html" | tr -d '\r')"

# ---------------------------------------------------------------------------
section 'Request parsing'

# RFC 7230 makes the space after the colon optional; rejecting it was a bug.
check_contains 'a header with no space after the colon is accepted' '200 OK' \
	"$(raw 'GET / HTTP/1.1\r\nHost:localhost\r\nConnection: close\r\n\r\n')"

check_contains 'a missing Host header is rejected' '400' \
	"$(raw 'GET / HTTP/1.1\r\nConnection: close\r\n\r\n')"

check_contains 'an unsupported HTTP version is rejected' '505' \
	"$(raw 'GET / HTTP/9.9\r\nHost: localhost\r\n\r\n')"

# Content-Length and Transfer-Encoding together is the request-smuggling setup.
check_contains 'Content-Length with Transfer-Encoding is rejected' '400' \
	"$(raw 'POST /uploads/ HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\nhello')"

# Obsolete line folding, deprecated and a smuggling vector.
check_contains 'an obs-fold header is rejected' '400' \
	"$(raw 'GET / HTTP/1.1\r\nHost: localhost\r\nX-Test: a\r\n b\r\n\r\n')"

# ---------------------------------------------------------------------------
section 'Security'

check 'a traversal escape returns 400 or 404' 'yes' \
	"$(code=$(status "${BASE}/../../../../etc/passwd"); [[ "${code}" == 400 || "${code}" == 404 || "${code}" == 403 ]] && echo yes || echo "no (${code})")"

# The traversal must also be caught when it is percent-encoded, which is why
# decoding happens before normalisation rather than after.
check 'an encoded traversal escape is blocked' 'yes' \
	"$(code=$(status "${BASE}/%2e%2e%2f%2e%2e%2fetc/passwd"); [[ "${code}" == 400 || "${code}" == 404 || "${code}" == 403 ]] && echo yes || echo "no (${code})")"

check_contains 'the passwd file is never served' '' \
	"$(curl -s --max-time 10 "${BASE}/../../../../etc/passwd" | grep -c 'root:' || true)"

# ---------------------------------------------------------------------------
section 'Crash regressions (each of these killed the original server)'

# A multipart Content-Type with no boundary parameter dereferenced NULL.
raw 'POST /uploads/ HTTP/1.1\r\nHost: localhost\r\nContent-Type: multipart/form-data\r\nContent-Length: 5\r\n\r\nhello' >/dev/null
if alive; then
	pass 'multipart without a boundary does not crash the server'
else
	fail 'multipart without a boundary does not crash the server' 'server alive' 'server died'
	exit 1
fi

raw 'POST /uploads/ HTTP/1.1\r\nHost: localhost\r\nContent-Type: multipart/form-data; boundary=\r\nContent-Length: 5\r\n\r\nhello' >/dev/null
if alive; then
	pass 'an empty boundary does not crash the server'
else
	fail 'an empty boundary does not crash the server' 'server alive' 'server died'
	exit 1
fi

raw 'POST /uploads/ HTTP/1.1\r\nHost: localhost\r\nContent-Type: multipart/form-data; boundary=xyz\r\nContent-Length: 9\r\n\r\n--xyz\r\nab' >/dev/null
if alive; then
	pass 'a truncated multipart body does not crash the server'
else
	fail 'a truncated multipart body does not crash the server' 'server alive' 'server died'
	exit 1
fi

check 'the server still answers after the malformed requests' 200 "$(status "${BASE}/")"

# ---------------------------------------------------------------------------
section 'Uploads'

upload_source="$(mktemp -t webserv-upload.XXXXXX)"
printf 'uploaded by the test suite\n' > "${upload_source}"

upload_status="$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 \
	-F "file=@${upload_source};filename=test-upload.txt" "${BASE}/uploads/")"
check 'a multipart upload returns 201' 201 "${upload_status}"

check 'the uploaded file can be fetched' 200 "$(status "${BASE}/uploads/test-upload.txt")"
check_contains 'the uploaded content round-trips' 'uploaded by the test suite' \
	"$(curl -s --max-time 10 "${BASE}/uploads/test-upload.txt")"

check 'DELETE removes it' 204 "$(status -X DELETE "${BASE}/uploads/test-upload.txt")"
check 'it is gone afterwards' 404 "$(status "${BASE}/uploads/test-upload.txt")"
check 'DELETE on a missing file returns 404' 404 \
	"$(status -X DELETE "${BASE}/uploads/never-existed.txt")"

# A filename that tries to climb out of the upload directory.
curl -s -o /dev/null --max-time 10 \
	-F "file=@${upload_source};filename=../escaped.txt" "${BASE}/uploads/" >/dev/null 2>&1
if [[ -f "${ROOT}/escaped.txt" || -f "${ROOT}/www/escaped.txt" ]]; then
	fail 'an upload cannot escape its directory' 'no file outside uploads/' 'file was written outside'
	rm -f "${ROOT}/escaped.txt" "${ROOT}/www/escaped.txt"
else
	pass 'an upload cannot escape its directory'
fi
rm -f "${upload_source}" "${ROOT}/www/uploads/escaped.txt"

# ---------------------------------------------------------------------------
section 'Body limits'

# Larger than client_max_body_size for /uploads/ (5 MiB).
big="$(mktemp -t webserv-big.XXXXXX)"
dd if=/dev/zero of="${big}" bs=1024 count=6000 2>/dev/null
check 'an oversized body returns 413' 413 \
	"$(status -X POST --data-binary "@${big}" -H 'Content-Type: application/octet-stream' "${BASE}/uploads/")"
rm -f "${big}"

check 'a POST with no Content-Length returns 411' 411 \
	"$(raw 'POST /uploads/ HTTP/1.1\r\nHost: localhost\r\n\r\n' | head -1 | grep -o '411' || echo 'none')"

# ---------------------------------------------------------------------------
section 'CGI'

check 'a CGI script runs'              200 "$(status "${BASE}/cgi-bin/hello.py")"
check_contains 'its output is returned' 'Hello from a CGI script' \
	"$(curl -s --max-time 10 "${BASE}/cgi-bin/hello.py")"

check_contains 'QUERY_STRING reaches the script' 'name=world' \
	"$(curl -s --max-time 10 "${BASE}/cgi-bin/env.py?name=world")"

check_contains 'a POST body reaches the script' 'hello cgi' \
	"$(curl -s --max-time 10 -X POST -d 'hello cgi' "${BASE}/cgi-bin/echo.py")"

# A chunked body must be reassembled before the script sees it, since CGI has
# no way to express chunked input.
check_contains 'a chunked body is reassembled for the script' 'chunked payload' \
	"$(curl -s --max-time 10 -X POST -H 'Transfer-Encoding: chunked' \
		-d 'chunked payload' "${BASE}/cgi-bin/echo.py")"

check 'a script Status header is honoured' 418 \
	"$(status "${BASE}/cgi-bin/status.py?code=418")"

check 'a missing script returns 404' 404 "$(status "${BASE}/cgi-bin/absent.py")"

# The loop must stay responsive while a script is hanging.
printf '  %s…%s running the CGI timeout test (about 12s)\n' "${GREY}" "${RESET}"
slow_start=$(date +%s)
slow_code="$(status --max-time 30 "${BASE}/cgi-bin/slow.py")"
slow_elapsed=$(( $(date +%s) - slow_start ))
check 'a hanging script is killed and returns 504' 504 "${slow_code}"

if (( slow_elapsed < 25 )); then
	pass "the timeout fired promptly (${slow_elapsed}s)"
else
	fail 'the timeout fired promptly' 'under 25s' "${slow_elapsed}s"
fi

check 'the server is responsive during that time' 200 "$(status "${BASE}/")"

# A killed script must be reaped, not left as a zombie.
sleep 1
zombies="$(ps -o stat= -p "${SERVER_PID}" 2>/dev/null; ps -ax -o stat=,ppid= 2>/dev/null | awk -v p="${SERVER_PID}" '$2==p && $1 ~ /Z/' | wc -l | tr -d ' ')"
zombie_count="$(ps -ax -o stat=,ppid= 2>/dev/null | awk -v p="${SERVER_PID}" '$2==p && $1 ~ /Z/' | wc -l | tr -d ' ')"
check 'no zombie children are left behind' 0 "${zombie_count}"

# ---------------------------------------------------------------------------
section 'Connection handling'

# Two requests over one connection: the parser must carry leftover bytes across.
pipelined="$(raw 'GET / HTTP/1.1\r\nHost: localhost\r\n\r\nGET /files/notes.txt HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n')"
responses="$(printf '%s' "${pipelined}" | grep -c '^HTTP/1.1' || echo 0)"
if (( responses >= 2 )); then
	pass 'two pipelined requests receive two responses'
else
	fail 'two pipelined requests receive two responses' '2 responses' "${responses}"
fi

check_contains 'keep-alive is advertised' 'keep-alive' \
	"$(curl -s -D- -o /dev/null --max-time 10 "${BASE}/" | tr -d '\r' | grep -i '^connection' || echo none)"

check_contains 'Connection: close is honoured' 'close' \
	"$(curl -s -D- -o /dev/null --max-time 10 -H 'Connection: close' "${BASE}/" | tr -d '\r' | grep -i '^connection' || echo none)"

# ---------------------------------------------------------------------------
section 'Concurrency'

# Each client records its own result in a file: a counter incremented inside a
# backgrounded subshell would not survive back into this shell.
concurrent_dir="$(mktemp -d -t webserv-conc.XXXXXX)"
client_pids=()
for i in $(seq 1 50); do
	(curl -s -o /dev/null --max-time 15 "${BASE}/" && touch "${concurrent_dir}/${i}") &
	client_pids+=($!)
done
# Waiting on the collected pids rather than a bare `wait`, which would also
# wait on the server this script started in the background and never return.
for pid in "${client_pids[@]}"; do
	wait "${pid}" 2>/dev/null
done
concurrent_ok="$(find "${concurrent_dir}" -type f | wc -l | tr -d ' ')"
rm -rf "${concurrent_dir}"
check '50 concurrent clients all succeed' 50 "${concurrent_ok}"
check 'the server survives them' 200 "$(status "${BASE}/")"

# A large response must arrive whole: a partial send() that was not resumed
# would truncate it.
dd if=/dev/urandom of="${ROOT}/www/files/large.bin" bs=1024 count=2048 2>/dev/null
expected_size=$(wc -c < "${ROOT}/www/files/large.bin" | tr -d ' ')
actual_size="$(curl -s --max-time 30 "${BASE}/files/large.bin" | wc -c | tr -d ' ')"
check 'a 2 MiB response is delivered whole' "${expected_size}" "${actual_size}"
rm -f "${ROOT}/www/files/large.bin"

# ---------------------------------------------------------------------------
section 'Second server'

check 'the second port answers'   200 "$(status "http://${HOST}:${PORT2}/")"
check_contains 'with its own root' 'The second server' \
	"$(curl -s --max-time 10 "http://${HOST}:${PORT2}/")"

# ---------------------------------------------------------------------------
section 'Shutdown'

check 'the server is still alive at the end' 'yes' "$(alive && echo yes || echo no)"

# ---------------------------------------------------------------------------
printf '\n'
if (( FAILED == 0 )); then
	printf '%s%d passed%s, %d failed\n' "${GREEN}" "${PASSED}" "${RESET}" "${FAILED}"
	exit 0
fi
printf '%d passed, %s%d failed%s\n' "${PASSED}" "${RED}" "${FAILED}" "${RESET}"
exit 1
