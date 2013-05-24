var utils = require("../utils");

////////////////////////////////////////////////////////////////////////////////

exports.that = function Middleware__YtAcao() {
    "use strict";

    return function(req, rsp, next) {
        "use strict";

        if (req.method === "GET") {
            rsp.setHeader("Access-Control-Allow-Origin", "*");
        }

        if (req.method === "OPTIONS") {
            rsp.setHeader("Access-Control-Allow-Origin", "*");
            rsp.setHeader("Access-Control-Allow-Methods", "POST, PUT, GET, OPTIONS");
            rsp.setHeader("Access-Control-Allow-Headers", "authorization, origin, content-type, accept, x-yt-parameters, x-yt-input-format, x-yt-output-format");
            rsp.setHeader("Access-Control-Max-Age", "3600");
            return void utils.dispatchAs(rsp);
        }

        next();
    };
};
