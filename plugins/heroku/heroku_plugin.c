#include <uwsgi.h>

extern struct uwsgi_server uwsgi;

// A bit of a hack; it's not really a "hijacking" as it's used in the other
// plugins. But the hijack_worker plugin hook is the only one executed
// after initial UNIX signal handling is set for generic plugins, and it 
// seems safest to set this as late as possible.
void uwsgi_heroku_hijack(void) {
	// all signals are blocked on most threads already; we also block
	// SIGTERM on the main thread so Heroku can't directly kill worker processes.
	sigset_t smask;
	sigemptyset(&smask);
	sigaddset(&smask, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &smask, NULL);
	return;
}

struct uwsgi_plugin heroku_plugin = {
	.name = "heroku",
	.hijack_worker = uwsgi_heroku_hijack,

};

