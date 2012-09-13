var utils = require("./utils");

var YtCommand = require("./command").that;
var YtDriver = require("./driver").that;
var YtEioWatcher = require("./eio_watcher").that;

var ConfigureSingletons = require("./ytnode").ConfigureSingletons;

////////////////////////////////////////////////////////////////////////////////

var __DBG;

if (process.env.NODE_DEBUG && /YTAPP/.test(process.env.NODE_DEBUG)) {
    __DBG = function(x) { "use strict"; console.error("YT Application:", x); };
} else {
    __DBG = function(){};
}

////////////////////////////////////////////////////////////////////////////////

exports.that = function YtApplication(logger, configuration) {
    "use strict";
    
    __DBG("New");

    ConfigureSingletons(configuration);

    if (typeof(configuration.low_watermark) === "undefined") {
        configuration.low_watermark = parseInt(0.80 * configuration.memory_limit, 10);
    }

    if (typeof(configuration.high_watermark) === "undefined") {
        configuration.high_watermark = parseInt(0.95 * configuration.memory_limit, 10);
    }

    var driver = new YtDriver(false, configuration);
    var watcher = new YtEioWatcher(logger, configuration);

    return function(req, rsp) {
        return (new YtCommand(
            logger, driver, watcher, req, rsp
        )).dispatch();
    };
};
