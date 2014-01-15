#include <uwsgi.h>

extern struct uwsgi_server uwsgi;


void gracefully_shutdown(int signum) {

	uwsgi_log("Gracefully shutting down worker %d (pid: %d)...\n", uwsgi.mywid, uwsgi.mypid);
	uwsgi.workers[uwsgi.mywid].manage_next_request = 0;
	if (uwsgi.threads > 1) {
		struct wsgi_request *wsgi_req = current_wsgi_req();
		wait_for_threads();
		if (!uwsgi.workers[uwsgi.mywid].cores[wsgi_req->async_id].in_request) {
			exit(0);
		}
		return;
		// never here
	}

	// still not found a way to gracefully reload in async mode
	if (uwsgi.async > 1) {
		exit(0);
	}

	if (!uwsgi.workers[uwsgi.mywid].cores[0].in_request) {
		exit(0);
	}

#ifdef UWSGI_DEBUG
	uwsgi_log("Not exiting, in request: worker %d (pid: %d)...\n", uwsgi.mywid, uwsgi.mypid);
#endif
}

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

	// Also, we want uWSGI workers to be gracefully and not brutally killed.
	uwsgi_unix_signal(SIGINT, gracefully_kill);

	return;
}

struct uwsgi_plugin heroku_plugin = {
	.name = "heroku",
	.hijack_worker = uwsgi_heroku_hijack,

};

