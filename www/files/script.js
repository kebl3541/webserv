// Served as application/javascript.
//
// Note that this file is served, not executed. The server runs nothing on its
// own behalf. Executing code happens only through CGI, in a forked child, for
// paths configured with cgi_extension.

(function () {
    "use strict";

    function timeRequest(url) {
        var started = Date.now();
        return fetch(url)
            .then(function (response) {
                return { status: response.status, ms: Date.now() - started };
            });
    }

    window.timeRequest = timeRequest;
}());
