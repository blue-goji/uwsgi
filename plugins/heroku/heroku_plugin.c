#include <uwsgi.h>

extern struct uwsgi_server uwsgi;

// A bit of a hack; it's not really a "hijacking" as it's used in the other
// plugins. But the hijack_worker plugin hook is the only one executed
// after initial UNIX signal handling is set for generic plugins.
void uwsgi_heroku_hijack(void) {
	// at the very least, reset SIGTERM handler to an alarm of some
	// sort (just use harakiri?)
	return;
}

struct uwsgi_plugin heroku_plugin = {
	.name = "heroku",
	.hijack_worker = uwsgi_heroku_hijack,

};

