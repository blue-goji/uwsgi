#include "uwsgi.h"


extern struct uwsgi_server uwsgi;

#ifdef __BIG_ENDIAN__
uint16_t uwsgi_swap16(uint16_t x) {
	return (uint16_t) ((x & 0xff) << 8 | (x & 0xff00) >> 8);
}

uint32_t uwsgi_swap32(uint32_t x) {
	x = ( (x<<8) & 0xFF00FF00 ) | ( (x>>8) & 0x00FF00FF );
	return (x>>16) | (x<<16);
}

// thanks to ffmpeg project for this idea :P
uint64_t uwsgi_swap64(uint64_t x) {
	union {
		uint64_t ll;
		uint32_t l[2];
	} w, r;
	w.ll = x;
	r.l[0] = uwsgi_swap32(w.l[1]);
	r.l[1] = uwsgi_swap32(w.l[0]);
	return r.ll;
}

#endif

int check_hex(char *str, int len) {
	int i;
	for(i=0;i<len;i++) {
        	if (
                	(str[i] < '0' && str[i] > '9') &&
                	(str[i] < 'a' && str[i] > 'f') &&
                	(str[i] < 'A' && str[i] > 'F')
		) {
			return 0;
		}
        }

	return 1;

}

void inc_harakiri(int sec) {
	if (uwsgi.master_process) {
		uwsgi.workers[uwsgi.mywid].harakiri += sec;
	}
	else {
		alarm(uwsgi.shared->options[UWSGI_OPTION_HARAKIRI] + sec);
	}
}

void set_harakiri(int sec) {
	if (uwsgi.master_process) {
		if (sec == 0) {
			uwsgi.workers[uwsgi.mywid].harakiri = 0;
		}
		else {
			uwsgi.workers[uwsgi.mywid].harakiri = time(NULL) + sec;
		}
	}
	else {
		alarm(sec);
	}
}

void daemonize(char *logfile) {
	pid_t pid;
	int fdin;

	// do not daemonize under emperor
	if (uwsgi.has_emperor) {
		logto(logfile);
		return;
	}

	pid = fork();
	if (pid < 0) {
		uwsgi_error("fork()");
		exit(1);
	}
	if (pid != 0) {
		exit(0);
	}

	if (setsid() < 0) {
		uwsgi_error("setsid()");
		exit(1);
	}

	/* refork... */
	pid = fork();
	if (pid < 0) {
		uwsgi_error("fork()");
		exit(1);
	}
	if (pid != 0) {
		exit(0);
	}

	umask(0);

	/*if (chdir("/") != 0) {
	  uwsgi_error("chdir()");
	  exit(1);
	  } */


	fdin = open("/dev/null", O_RDWR);
	if (fdin < 0) {
		uwsgi_error_open("/dev/null");
		exit(1);
	}

	/* stdin */
	if (dup2(fdin, 0) < 0) {
		uwsgi_error("dup2()");
		exit(1);
	}


	logto(logfile);
}

void logto(char *logfile) {

	int fd;

#ifdef UWSGI_UDP
	char *udp_port;
	struct sockaddr_in udp_addr;

	udp_port = strchr(logfile, ':');
	if (udp_port) {
		udp_port[0] = 0;
		if ( !udp_port[1] || !logfile[0] ) {
			uwsgi_log("invalid udp address\n");
			exit(1);
		}

		fd = socket(AF_INET,  SOCK_DGRAM, 0);
		if (fd < 0) {
			uwsgi_error("socket()");
			exit(1);
		}

		memset(&udp_addr, 0, sizeof(struct sockaddr_in));

		udp_addr.sin_family = AF_INET;
		udp_addr.sin_port = htons(atoi(udp_port+1));
		udp_addr.sin_addr.s_addr = inet_addr(logfile);

		if (connect(fd, (const struct sockaddr *) &udp_addr, sizeof(struct sockaddr_in)) < 0) {
			uwsgi_error("connect()");
			exit(1);
		}
	}
	else {
#endif
		fd = open(logfile, O_RDWR | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR | S_IRGRP);
		if (fd < 0) {
			uwsgi_error_open(logfile);
			exit(1);
		}
#ifdef UWSGI_UDP
		uwsgi.logfile = logfile;
	}
#endif


	/* stdout */
	if (fd != 1) {
		if (dup2(fd, 1) < 0) {
			uwsgi_error("dup2()");
			exit(1);
		}
		close(fd);
	}

	/* stderr */
	if (dup2(1, 2) < 0) {
		uwsgi_error("dup2()");
		exit(1);
	}
}

void log_syslog(char *syslog_opts) {

	if (syslog_opts == NULL) {
		syslog_opts= "uwsgi";
	}

	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, uwsgi.shared->worker_log_pipe)) {
		uwsgi_error("socketpair()\n");
		exit(1);
	}

#ifdef UWSGI_DEBUG
	uwsgi_log("log pipe %d %d\n", uwsgi.shared->worker_log_pipe[0], uwsgi.shared->worker_log_pipe[1]);
#endif

	if (uwsgi.shared->worker_log_pipe[1] != 1) {
		if (dup2(uwsgi.shared->worker_log_pipe[1], 1) < 0) {
			uwsgi_error("dup2()");
			exit(1);
		}
	}
		

#ifdef UWSGI_DEBUG
	uwsgi_log("opening syslog\n");
#endif

	if (dup2(1, 2) < 0) {
		uwsgi_error("dup2()");
		exit(1);
	}

	openlog(syslog_opts, 0, LOG_DAEMON );

			
}

char *uwsgi_get_cwd() {

	int newsize = 256;
	char *cwd;

	cwd = uwsgi_malloc(newsize);

	if (getcwd(cwd, newsize) == NULL) {
		newsize = errno;
		uwsgi_log("need a bigger buffer (%d bytes) for getcwd(). doing reallocation.\n", newsize);
		free(cwd);
		cwd = uwsgi_malloc(newsize);
		if (getcwd(cwd, newsize) == NULL) {
			uwsgi_error("getcwd()");
			exit(1);
		}
	}

	return cwd;

}

void internal_server_error(int fd, char *message) {
	if (uwsgi.shared->options[UWSGI_OPTION_CGI_MODE] == 0) {
		uwsgi.wsgi_req->headers_size = write(fd, "HTTP/1.1 500 Internal Server Error\r\nContent-type: text/html\r\n\r\n", 63);
	}
	else {
		uwsgi.wsgi_req->headers_size = write(fd, "Status: 500 Internal Server Error\r\nContent-type: text/html\r\n\r\n", 62);
	}
	uwsgi.wsgi_req->header_cnt = 2;

	uwsgi.wsgi_req->response_size = write(fd, "<h1>uWSGI Error</h1>", 20);
	uwsgi.wsgi_req->response_size += write(fd, message, strlen(message));
}

void uwsgi_as_root() {

#ifdef __linux__
	char *cgroup_taskfile;
	int i;
	FILE *cgroup;
	char *cgroup_opt;
#endif

	if (!getuid()) {
		if (!uwsgi.master_as_root && !uwsgi.uidname) {
			uwsgi_log("uWSGI running as root, you can use --uid/--gid/--chroot options\n");
		}

#ifdef __linux__
		if (uwsgi.cgroup) {
			if (mkdir(uwsgi.cgroup, S_IRWXU | S_IROTH | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)) {
				uwsgi_log("using Linux cgroup %s\n", uwsgi.cgroup);
			}
			else {
				uwsgi_log("created Linux cgroup %s\n", uwsgi.cgroup);
			}
			cgroup_taskfile = uwsgi_concat2(uwsgi.cgroup, "/tasks");	
			cgroup = fopen(cgroup_taskfile, "w");
			if (!cgroup) {
				uwsgi_error_open(cgroup_taskfile);
				exit(1);
			}
			if (fprintf(cgroup, "%d\n", (int) getpid()) < 0) {
				uwsgi_log( "could not set cgroup\n");
				exit(1);
			}
			fclose(cgroup);
			free(cgroup_taskfile);

			for(i=0;i<uwsgi.cgroup_opt_cnt;i++) {
				cgroup_opt = strchr( uwsgi.cgroup_opt[i], '=' );
				if (!cgroup_opt) {
					cgroup_opt = strchr( uwsgi.cgroup_opt[i], ':' );
					if (!cgroup_opt) {
						uwsgi_log("invalid cgroup-opt syntax\n");
						exit(1);
					}
				}

				cgroup_opt[0] = 0;
				cgroup_opt++;

				cgroup_taskfile = uwsgi_concat3(uwsgi.cgroup, "/", uwsgi.cgroup_opt[i]);
				cgroup = fopen(cgroup_taskfile, "w");
				if (!cgroup) {
					uwsgi_error_open(cgroup_taskfile);
					exit(1);
				}
				if (fprintf(cgroup, "%s\n", cgroup_opt) < 0) {
					uwsgi_log( "could not set cgroup option %s to %s\n", uwsgi.cgroup_opt[i], cgroup_opt);
					exit(1);
				}
				fclose(cgroup);
				free(cgroup_taskfile);
			}
		}
#endif
		if (uwsgi.chroot && !uwsgi.reloads) {
			if (!uwsgi.master_as_root) uwsgi_log("chroot() to %s\n", uwsgi.chroot);
			if (chroot(uwsgi.chroot)) {
				uwsgi_error("chroot()");
				exit(1);
			}
#ifdef __linux__
			if (uwsgi.shared->options[UWSGI_OPTION_MEMORY_DEBUG]) {
				uwsgi_log("*** Warning, on linux system you have to bind-mount the /proc fs in your chroot to get memory debug/report.\n");
			}
#endif
		}

		if (uwsgi.gidname) {
			struct group *ugroup = getgrnam(uwsgi.gidname);
                        if (ugroup) {
                        	uwsgi.gid = ugroup->gr_gid;
                        } else {
                        	uwsgi_log("group %s not found.\n", uwsgi.gidname);
                                exit(1);
                        }
		}
		if (uwsgi.uidname) {
			struct passwd *upasswd = getpwnam(uwsgi.uidname);
                        if (upasswd) {
                        	uwsgi.uid = upasswd->pw_uid;
                        } else {
                        	uwsgi_log("user %s not found.\n", uwsgi.uidname);
                                exit(1);
                        }
		}

		if (uwsgi.logfile_chown) {
			if (fchown(2, uwsgi.uid, uwsgi.gid)) {
				uwsgi_error("fchown()");
				exit(1);
			}
		}
		if (uwsgi.gid) {
			if (!uwsgi.master_as_root) uwsgi_log("setgid() to %d\n", uwsgi.gid);
			if (setgid(uwsgi.gid)) {
				uwsgi_error("setgid()");
				exit(1);
			}
			if (setgroups(0, NULL)) {
                                uwsgi_error("setgroups()");
                                exit(1);
                        }
		}
		if (uwsgi.uid) {
			if (!uwsgi.master_as_root) uwsgi_log("setuid() to %d\n", uwsgi.uid);
			if (setuid(uwsgi.uid)) {
				uwsgi_error("setuid()");
				exit(1);
			}
		}

		if (!getuid()) {
			uwsgi_log(" *** WARNING: you are running uWSGI as root !!! (use the --uid flag) *** \n");
		}
	}
	else {
		if (uwsgi.chroot && !uwsgi.is_a_reload) {
			uwsgi_log("cannot chroot() as non-root user\n");
			exit(1);
		}
		if (uwsgi.gid && getgid() != uwsgi.gid) {
			uwsgi_log("cannot setgid() as non-root user\n");
			exit(1);
		}
		if (uwsgi.uid && getuid() != uwsgi.uid) {
			uwsgi_log("cannot setuid() as non-root user\n");
			exit(1);
		}
	}
}

void uwsgi_close_request(struct wsgi_request *wsgi_req) {

	int waitpid_status;
	int tmp_id;

	gettimeofday(&wsgi_req->end_of_request, NULL);
	uwsgi.workers[uwsgi.mywid].running_time += (double) (((double) (wsgi_req->end_of_request.tv_sec * 1000000 + wsgi_req->end_of_request.tv_usec) - (double) (wsgi_req->start_of_request.tv_sec * 1000000 + wsgi_req->start_of_request.tv_usec)) / (double) 1000.0);


	// get memory usage
	if (uwsgi.shared->options[UWSGI_OPTION_MEMORY_DEBUG] == 1 || uwsgi.reload_on_as || uwsgi.reload_on_rss)
		get_memusage();


	// close the connection with the webserver
	if (!wsgi_req->fd_closed || wsgi_req->body_as_file) {
		// NOTE, if we close the socket before receiving eventually sent data, socket layer will send a RST
		wsgi_req->socket_proto_close(wsgi_req);
	}
	uwsgi.workers[0].requests++;
	uwsgi.workers[uwsgi.mywid].requests++;

	// after_request hook
	if (uwsgi.p[wsgi_req->uh.modifier1]->after_request) uwsgi.p[wsgi_req->uh.modifier1]->after_request(wsgi_req);

	// leave harakiri mode
	if (uwsgi.shared->options[UWSGI_OPTION_HARAKIRI] > 0) {
		set_harakiri(0);
	}

	// defunct process reaper
	if (uwsgi.shared->options[UWSGI_OPTION_REAPER] == 1 || uwsgi.grunt) {
		while( waitpid(WAIT_ANY, &waitpid_status, WNOHANG) > 0);
	}

	// reset request
	tmp_id = wsgi_req->async_id;
	memset(wsgi_req, 0, sizeof(struct wsgi_request));
	wsgi_req->async_id = tmp_id;

	if (uwsgi.shared->options[UWSGI_OPTION_MAX_REQUESTS] > 0 && uwsgi.workers[uwsgi.mywid].requests >= uwsgi.shared->options[UWSGI_OPTION_MAX_REQUESTS]) {
		goodbye_cruel_world();
	}

	if (uwsgi.reload_on_as &&  (int) (uwsgi.workers[uwsgi.mywid].vsz_size / 1024 / 1024) >= uwsgi.reload_on_as) {
		goodbye_cruel_world();
	}

	if (uwsgi.reload_on_rss && (int) (uwsgi.workers[uwsgi.mywid].rss_size / 1024 / 1024) >= uwsgi.reload_on_rss) {
		goodbye_cruel_world();
	}


	// ready to accept request, if i am a vassal signal Emperor about my loyalty
        if (uwsgi.has_emperor && !uwsgi.loyal) {
                uwsgi_log("announcing my loyalty to the Emperor...\n");
                char byte = 17;
                if (write(uwsgi.emperor_fd, &byte, 1) != 1) {
                        uwsgi_error("write()");
                }
		uwsgi.loyal = 1;
        }

}

void wsgi_req_setup(struct wsgi_request *wsgi_req, int async_id, int socket_id) {

	wsgi_req->poll.events = POLLIN;
	wsgi_req->app_id = uwsgi.default_app;
	wsgi_req->async_id = async_id;
#ifdef UWSGI_SENDFILE
	wsgi_req->sendfile_fd = -1;
#endif

	wsgi_req->hvec = uwsgi.async_hvec[wsgi_req->async_id];
	wsgi_req->buffer = uwsgi.async_buf[wsgi_req->async_id];

#ifdef UWSGI_ROUTING
	wsgi_req->ovector = uwsgi.async_ovector[wsgi_req->async_id];
#endif

	if (uwsgi.post_buffering > 0) {
		wsgi_req->post_buffering_buf = uwsgi.async_post_buf[wsgi_req->async_id];
	}

	if (socket_id > -1) {
	wsgi_req->socket_proto = uwsgi.sockets[socket_id].proto;
	wsgi_req->socket_proto_accept = uwsgi.sockets[socket_id].proto_accept;
        wsgi_req->socket_proto_write = uwsgi.sockets[socket_id].proto_write;
        wsgi_req->socket_proto_writev = uwsgi.sockets[socket_id].proto_writev;
        wsgi_req->socket_proto_write_header = uwsgi.sockets[socket_id].proto_write_header;
        wsgi_req->socket_proto_writev_header = uwsgi.sockets[socket_id].proto_writev_header;
        wsgi_req->socket_proto_sendfile = uwsgi.sockets[socket_id].proto_sendfile;
        wsgi_req->socket_proto_close = uwsgi.sockets[socket_id].proto_close;
	}

}

int wsgi_req_async_recv(struct wsgi_request *wsgi_req) {

	UWSGI_SET_IN_REQUEST;

	gettimeofday(&wsgi_req->start_of_request, NULL);

	if (!wsgi_req->do_not_add_to_async_queue) {
		if (event_queue_add_fd_read( uwsgi.async_queue, wsgi_req->poll.fd ) < 0) return -1;

		async_add_timeout(wsgi_req, uwsgi.shared->options[UWSGI_OPTION_SOCKET_TIMEOUT]);
		uwsgi.async_proto_fd_table[wsgi_req->poll.fd] = wsgi_req;
	}


	// enter harakiri mode
	if (uwsgi.shared->options[UWSGI_OPTION_HARAKIRI] > 0) {
		set_harakiri(uwsgi.shared->options[UWSGI_OPTION_HARAKIRI]);
	}

	return 0;
}

int wsgi_req_recv(struct wsgi_request *wsgi_req) {

	UWSGI_SET_IN_REQUEST;

	gettimeofday(&wsgi_req->start_of_request, NULL);


	if (!uwsgi.edge_triggered) {
		if (!uwsgi_parse_response(&wsgi_req->poll, uwsgi.shared->options[UWSGI_OPTION_SOCKET_TIMEOUT], (struct uwsgi_header *) wsgi_req, wsgi_req->buffer, wsgi_req->socket_proto)) {
			return -1;
		}
	}

	// enter harakiri mode
	if (uwsgi.shared->options[UWSGI_OPTION_HARAKIRI] > 0) {
		set_harakiri(uwsgi.shared->options[UWSGI_OPTION_HARAKIRI]);
	}

	wsgi_req->async_status = uwsgi.p[wsgi_req->uh.modifier1]->request(wsgi_req);

	return 0;
}


int wsgi_req_simple_accept(struct wsgi_request *wsgi_req, int fd) {

	wsgi_req->poll.fd = wsgi_req->socket_proto_accept(wsgi_req, fd);

	if (wsgi_req->poll.fd < 0) {
		return -1;
	}

	if (uwsgi.close_on_exec) {
		fcntl(wsgi_req->poll.fd, F_SETFD, FD_CLOEXEC);
	}

	return 0;
}

int wsgi_req_accept(struct wsgi_request *wsgi_req) {

	int i;
	int ret;
	char uwsgi_signal;

	if (uwsgi.edge_triggered) {
		uwsgi_log("EDGE TRIGGERED\n");
		for(i=0;i<uwsgi.sockets_cnt;i++) {
			if (uwsgi.sockets[i].edge_trigger) {
				uwsgi.sockets_poll[i].revents = POLLIN;
			}
			else {
				uwsgi.sockets_poll[i].revents = 0;
			}
		}
		goto edgetrigger;
	}

polling:
	ret = poll(uwsgi.sockets_poll, uwsgi.sockets_cnt+uwsgi.master_process, -1);

	if (ret < 0) {
		uwsgi_error("poll()");
		return -1;
	}

	if (uwsgi.master_process && uwsgi.sockets_poll[uwsgi.sockets_cnt].revents) {
		if (read(uwsgi.sockets_poll[uwsgi.sockets_cnt].fd, &uwsgi_signal, 1) <= 0) {
			if (uwsgi.no_orphans) {
				uwsgi_log_verbose("uWSGI worker %d screams: UAAAAAAH my master died, i will follow him...\n", uwsgi.mywid);
                		end_me(0);
			}
		}
                else {
                	uwsgi_log_verbose("master sent signal %d to worker %d\n", uwsgi_signal, uwsgi.mywid);
			if (uwsgi_signal_handler(uwsgi_signal)) {
				uwsgi_log_verbose("error managing signal %d on worker %d\n", uwsgi_signal, uwsgi.mywid);
			}
		}
	}


edgetrigger:
	uwsgi_log("scanning...\n");
	for(i=0;i<uwsgi.sockets_cnt;i++) {

		if (uwsgi.sockets_poll[i].revents & POLLIN) {
			int socket_id = i;
			wsgi_req->socket_proto = uwsgi.sockets[socket_id].proto;
			wsgi_req->socket_proto_accept = uwsgi.sockets[socket_id].proto_accept;
        		wsgi_req->socket_proto_write = uwsgi.sockets[socket_id].proto_write;
        		wsgi_req->socket_proto_writev = uwsgi.sockets[socket_id].proto_writev;
        		wsgi_req->socket_proto_write_header = uwsgi.sockets[socket_id].proto_write_header;
        		wsgi_req->socket_proto_writev_header = uwsgi.sockets[socket_id].proto_writev_header;
        		wsgi_req->socket_proto_sendfile = uwsgi.sockets[socket_id].proto_sendfile;
        		wsgi_req->socket_proto_close = uwsgi.sockets[socket_id].proto_close;

			wsgi_req->poll.fd = wsgi_req->socket_proto_accept(wsgi_req, uwsgi.sockets_poll[i].fd);

			if (wsgi_req->poll.fd < 0) {

				return -1;
				if (uwsgi.sockets[i].edge_trigger) { return -1 ;}
				if (errno == EWOULDBLOCK) { uwsgi_log("GOTO polling\n"); goto polling;}
				uwsgi_error("accept()");
				return -1;
			}

// in Linux, new sockets do not inherit attributes
#ifndef __linux__
                	/* re-set blocking socket */
			int arg = uwsgi.sockets[i].arg ;
                	arg &= (~O_NONBLOCK);
                	if (fcntl(wsgi_req->poll.fd, F_SETFL, arg) < 0) {
                        	uwsgi_error("fcntl()");
                        	return -1;
                	}

#endif

			if (uwsgi.close_on_exec) {
				fcntl(wsgi_req->poll.fd, F_SETFD, FD_CLOEXEC);
			}

			// set socket protocol

			return 0;
		}
	}

	return -1;
}

void sanitize_args() {

	if (uwsgi.async > 1) {
		uwsgi.cores = uwsgi.async;
	}

	if (uwsgi.threads > 1) {
		uwsgi.has_threads = 1;
		uwsgi.cores = uwsgi.threads;
	}

	if (uwsgi.shared->options[UWSGI_OPTION_HARAKIRI] > 0) {
		if (!uwsgi.post_buffering) {
			uwsgi_log(" *** WARNING: you have enabled harakiri without post buffering. Slow upload could be rejected on post-unbuffered webservers *** \n");
		}
	}

#ifdef UWSGI_HTTP
	if (uwsgi.http && !uwsgi.http_only) {
		uwsgi.vacuum = 1;
	}
#endif
}

void env_to_arg(char *src, char *dst) {
	int i;
	int val = 0;

	for(i=0;i< (int) strlen(src);i++) {
		if (src[i] == '=') {
			val = 1;
		}
		if (val) {
			dst[i] = src[i];
		}
		else {
			dst[i] = tolower( (int) src[i]);
			if (dst[i] == '_') {
				dst[i] = '-';
			}
		}
	}

	dst[strlen(src)] = 0;
}

void parse_sys_envs(char **envs) {

	char **uenvs = envs;
	char *earg, *eq_pos;

	while(*uenvs) {
		if (!strncmp(*uenvs, "UWSGI_", 6) && strncmp(*uenvs, "UWSGI_RELOADS=",14)) {
			earg = uwsgi_malloc(strlen(*uenvs+6)+1);
			env_to_arg(*uenvs+6, earg);
			eq_pos = strchr(earg, '=');
			if (!eq_pos) {
				break;
			}
			eq_pos[0] = 0;

			add_exported_option(earg, eq_pos+1, 0);
		}
		uenvs++;
	}

}

//use this instead of fprintf to avoid buffering mess with udp logging
void uwsgi_log(const char *fmt, ...) {
	va_list ap;
	char logpkt[4096];
	int rlen = 0;

	struct timeval tv;
	char sftime[64];
	time_t now;

	if (uwsgi.logdate) {
		if (uwsgi.log_strftime) {
			now = time(NULL);
			rlen = strftime( sftime, 64, uwsgi.log_strftime, localtime(&now));
			memcpy( logpkt, sftime, rlen);
			memcpy( logpkt + rlen, " - ", 3);
			rlen += 3;
		}
		else {	
			gettimeofday(&tv, NULL);

			memcpy( logpkt, ctime( (const time_t *) &tv.tv_sec), 24);
			memcpy( logpkt + 24, " - ", 3);

			rlen = 24 + 3;
		}
	}

	va_start (ap, fmt);
	rlen += vsnprintf(logpkt + rlen, 4096 - rlen, fmt, ap );
	va_end(ap);

	// do not check for errors
	rlen = write(2, logpkt, rlen);
}

void uwsgi_log_verbose(const char *fmt, ...) {

	va_list ap;
	char logpkt[4096];
	int rlen = 0;

	struct timeval tv;

	gettimeofday(&tv, NULL);

	memcpy( logpkt, ctime( (const time_t *) &tv.tv_sec), 24);
	memcpy( logpkt + 24, " - ", 3);

	rlen = 24 + 3;

	va_start (ap, fmt);
	rlen += vsnprintf(logpkt + rlen, 4096 - rlen, fmt, ap );
	va_end(ap);

	// do not check for errors
	rlen = write(2, logpkt, rlen);
}

inline int uwsgi_strncmp(char *src, int slen, char *dst, int dlen) {

	if (slen != dlen) return 1;

	return memcmp(src, dst, dlen);

}

inline int uwsgi_starts_with(char *src, int slen, char *dst, int dlen) {

	if (slen < dlen) return -1;

	return memcmp(src, dst, dlen);
}

inline int uwsgi_startswith(char *src, char *what, int wlen) {

	int i;

	for(i=0;i<wlen;i++) {
		if (src[i] != what[i]) return -1;
	}
	
	return 0;
}

char *uwsgi_concatn(int c, ...) {

	va_list s;
	char *item;
	int j = c;
	char *buf;
	size_t len = 1;
	size_t tlen = 1;

	va_start( s, c);
	while(j>0) {
		item = va_arg( s, char *);
		if (item == NULL) {
			break;
		}
		len += va_arg( s, int);
		j--;
	}
	va_end( s );


	buf = uwsgi_malloc(len);
	memset( buf, 0, len);

	j = c;

	len = 0;

	va_start( s, c);
	while(j>0) {
		item = va_arg( s, char *);
		if (item == NULL) {
			break;
		}
		tlen = va_arg( s, int);
		memcpy(buf + len, item, tlen);
		len += tlen;
		j--;
	}
	va_end( s );


	return buf;

}

char *uwsgi_concat2(char *one, char *two) {

	char *buf;
	size_t len = strlen(one) + strlen(two) + 1;


	buf = uwsgi_malloc(len);
	buf[len-1] = 0;

	memcpy( buf, one, strlen(one));
	memcpy( buf + strlen(one) , two, strlen(two));

	return buf;

}

char *uwsgi_concat4(char *one, char *two, char *three, char *four) {

	char *buf;
	size_t len = strlen(one) + strlen(two) + strlen(three) + strlen(four) + 1;


	buf = uwsgi_malloc(len);
	buf[len-1] = 0;

	memcpy( buf, one, strlen(one));
	memcpy( buf + strlen(one) , two, strlen(two));
	memcpy( buf + strlen(one) + strlen(two) , three, strlen(three));
	memcpy( buf + strlen(one) + strlen(two) + strlen(three) , four, strlen(four));

	return buf;

}


char *uwsgi_concat3(char *one, char *two, char *three) {

	char *buf;
	size_t len = strlen(one) + strlen(two) + strlen(three) + 1;


	buf = uwsgi_malloc(len);
	buf[len-1] = 0;

	memcpy( buf, one, strlen(one));
	memcpy( buf + strlen(one) , two, strlen(two));
	memcpy( buf + strlen(one) + strlen(two) , three, strlen(three));

	return buf;

}

char *uwsgi_concat2n(char *one, int s1, char *two, int s2) {

	char *buf;
	size_t len = s1 + s2 + 1;


	buf = uwsgi_malloc(len);
	buf[len-1] = 0;

	memcpy( buf, one, s1);
	memcpy( buf + s1, two, s2);

	return buf;

}

char *uwsgi_concat2nn(char *one, int s1, char *two, int s2, int *len) {

        char *buf;
        *len = s1 + s2 + 1;


        buf = uwsgi_malloc(*len);
        buf[*len-1] = 0;

        memcpy( buf, one, s1);
        memcpy( buf + s1, two, s2);

        return buf;

}


char *uwsgi_concat3n(char *one, int s1, char *two, int s2, char *three, int s3) {

	char *buf;
	size_t len = s1 + s2 + s3 + 1;


	buf = uwsgi_malloc(len);
	buf[len-1] = 0;

	memcpy( buf, one, s1);
	memcpy( buf + s1, two, s2);
	memcpy( buf + s1 + s2, three, s3);

	return buf;

}

char *uwsgi_concat4n(char *one, int s1, char *two, int s2, char *three, int s3, char *four, int s4) {

	char *buf;
	size_t len = s1 + s2 + s3 + s4 + 1;


	buf = uwsgi_malloc(len);
	buf[len-1] = 0;

	memcpy( buf, one, s1);
	memcpy( buf + s1, two, s2);
	memcpy( buf + s1 + s2, three, s3);
	memcpy( buf + s1 + s2 + s3, four, s4);

	return buf;

}



char *uwsgi_concat(int c, ... ) {

	va_list s;
	char *item;
	size_t len = 1;
	int j = c;
	char *buf;

	va_start( s, c);
	while(j>0) {
		item = va_arg( s, char *);
		if (item == NULL) {
			break;
		}
		len += (int) strlen(item);
		j--;
	}
	va_end( s );


	buf = uwsgi_malloc(len);
	memset( buf, 0, len);

	j = c;

	len = 0;

	va_start( s, c);
	while(j>0) {
		item = va_arg( s, char *);
		if (item == NULL) {
			break;
		}
		memcpy(buf + len, item, strlen(item));
		len += strlen(item);	
		j--;
	}
	va_end( s );


	return buf;

}

char *uwsgi_strncopy(char *s, int len) {

	char *buf;

	buf = uwsgi_malloc(len + 1);
	buf[len] = 0;

	memcpy(buf, s, len);

	return buf;

}


int uwsgi_get_app_id(char *script_name, int script_name_len, int modifier1) {

	int i;
	struct stat st;

	for(i=0;i<uwsgi.apps_cnt;i++) {
		//uwsgi_log("searching for %.*s in %.*s %p\n", script_name_len, script_name, uwsgi.apps[i].mountpoint_len, uwsgi.apps[i].mountpoint, uwsgi.apps[i].callable);
		if (!uwsgi.apps[i].mountpoint_len) {
			continue;
		}	
		if (!uwsgi_strncmp(uwsgi.apps[i].mountpoint, uwsgi.apps[i].mountpoint_len, script_name, script_name_len)) {
			if (uwsgi.apps[i].touch_reload) {
				if (!stat(uwsgi.apps[i].touch_reload, &st)) {
					if (st.st_mtime != uwsgi.apps[i].touch_reload_mtime) {
						// serve the new request and reload
						uwsgi.workers[uwsgi.mywid].manage_next_request = 0;
						return -1;	
					}
				}
			}
			if (modifier1 == -1) return i;
			if (modifier1 == uwsgi.apps[i].modifier1) return i;
		}
	}

	return -1;
}

int count_options(struct option *lopt) {
	struct option *aopt;
	int count = 0;

	while ( (aopt = lopt) ) {
		if (!aopt->name) break;
		count++;
		lopt++;
	}

	return count;
}

int uwsgi_read_whole_body_in_mem(struct wsgi_request *wsgi_req, char *buf) {

	size_t post_remains = wsgi_req->post_cl;
	int ret;
	ssize_t len;
	char *ptr = buf;

	while(post_remains) {
                if (uwsgi.shared->options[UWSGI_OPTION_HARAKIRI] > 0) {
                        inc_harakiri(uwsgi.shared->options[UWSGI_OPTION_SOCKET_TIMEOUT]);
                }

                ret = uwsgi_waitfd(wsgi_req->poll.fd, uwsgi.shared->options[UWSGI_OPTION_SOCKET_TIMEOUT]);
                if (ret < 0) {
                        return 0;
                }

                if (!ret) {
                        uwsgi_log("buffering POST data timedout !!!\n");
			return 0;
                }

                len = read(wsgi_req->poll.fd, ptr, post_remains);
                if (len <= 0) {
                        uwsgi_error("read()");
			return 0;
                }
		ptr += len;
                post_remains -= len;
        }

	return 1;
	
}

int uwsgi_read_whole_body(struct wsgi_request *wsgi_req, char *buf, size_t len) {

	size_t post_remains = wsgi_req->post_cl;
	ssize_t post_chunk;
	int ret,i;
	int upload_progress_fd = -1;
	char *upload_progress_filename = NULL;
	const char *x_progress_id = "X-Progress-ID=";
	char *xpi_ptr = (char *) x_progress_id ;
	

	wsgi_req->async_post = tmpfile();
	if (!wsgi_req->async_post) {
		uwsgi_error("tmpfile()");
		return 0;
	}

	if (uwsgi.upload_progress) {
		// first check for X-Progress-ID size
		// separator + 'X-Progress-ID' + '=' + uuid	
		if (wsgi_req->uri_len > 51) {
			for(i=0;i<wsgi_req->uri_len;i++) {
				if (wsgi_req->uri[i] == xpi_ptr[0]) {
					if (xpi_ptr[0] == '=') {
						if (wsgi_req->uri+i+36 <= wsgi_req->uri+wsgi_req->uri_len) {
							upload_progress_filename = wsgi_req->uri+i+1 ;
						}
						break;
					}
					xpi_ptr++;
				}
				else {
					xpi_ptr = (char *) x_progress_id ;
				}
			}

			// now check for valid uuid (from spec available at http://en.wikipedia.org/wiki/Universally_unique_identifier)
			if (upload_progress_filename) {

				uwsgi_log("upload progress uuid = %.*s\n", 36, upload_progress_filename);
				if (!check_hex(upload_progress_filename, 8)) goto cycle;
				if (upload_progress_filename[8] != '-') goto cycle;

				if (!check_hex(upload_progress_filename+9, 4)) goto cycle;
				if (upload_progress_filename[13] != '-') goto cycle;

				if (!check_hex(upload_progress_filename+14, 4)) goto cycle;
				if (upload_progress_filename[18] != '-') goto cycle;

				if (!check_hex(upload_progress_filename+19, 4)) goto cycle;
				if (upload_progress_filename[23] != '-') goto cycle;

				if (!check_hex(upload_progress_filename+24, 12)) goto cycle;

				upload_progress_filename = uwsgi_concat4n(uwsgi.upload_progress, strlen(uwsgi.upload_progress), "/",1,upload_progress_filename, 36, ".js", 3);
				// here we use O_EXCL to avoid eventual application bug in uuid generation/using
				upload_progress_fd = open(upload_progress_filename, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR | S_IRGRP);	
				if (upload_progress_fd < 0) {
					uwsgi_error_open(upload_progress_filename);
					free(upload_progress_filename);
				}
			}
		}
	}

cycle:
	if (upload_progress_filename && upload_progress_fd == -1) {
		uwsgi_log("invalid X-Progress-ID value: must be a UUID\n");
	}
	// manage buffered data and upload progress
	while(post_remains > 0) {
		if (uwsgi.shared->options[UWSGI_OPTION_HARAKIRI] > 0) {
			inc_harakiri(uwsgi.shared->options[UWSGI_OPTION_SOCKET_TIMEOUT]);
		}

		ret = poll(&wsgi_req->poll, 1, uwsgi.shared->options[UWSGI_OPTION_SOCKET_TIMEOUT] * 1000);
        	if (ret < 0) {
                	uwsgi_error("poll()");
			goto end;		
        	}

		if (!ret) {
			uwsgi_log("buffering POST data timedout !!!\n");
			goto end;		
		}

		if (post_remains > len) {
			post_chunk = read(wsgi_req->poll.fd, buf, len);
		}
		else {
			post_chunk = read(wsgi_req->poll.fd, buf, post_remains);
		} 
		if (post_chunk < 0) {
			uwsgi_error("read()");
			goto end;		
		}
		if (!fwrite(buf, post_chunk, 1, wsgi_req->async_post)) {
			uwsgi_error("fwrite()");
			goto end;		
		}
		if (upload_progress_fd > -1) {
			//write json data to the upload progress file
			if (lseek(upload_progress_fd, 0, SEEK_SET)) {
				uwsgi_error("lseek()");
				goto end;		
			}
			
			// resue buf for json buffer
			ret = snprintf(buf, len, "{ \"state\" : \"uploading\", \"received\" : %d, \"size\" : %d }\r\n", (int) (wsgi_req->post_cl - post_remains), (int) wsgi_req->post_cl);
			if (ret < 0) {
				uwsgi_log("unable to write JSON data in upload progress file %s\n", upload_progress_filename);
				goto end;
			}
			if (write(upload_progress_fd, buf, ret) < 0) {
				uwsgi_error("write()");
				goto end;
			}

			if (fsync(upload_progress_fd)) {
				uwsgi_error("fsync()");
			}
		}
		post_remains -= post_chunk;
	}
	rewind(wsgi_req->async_post);

	if (upload_progress_fd > -1) {
		close(upload_progress_fd);
		if (unlink(upload_progress_filename)) {
			uwsgi_error("unlink()");
		}
		free(upload_progress_filename);
	}

	return 1;

end:
	if (upload_progress_fd > -1) {
		close(upload_progress_fd);
		if (unlink(upload_progress_filename)) {
			uwsgi_error("unlink()");
		}
		free(upload_progress_filename);
	}
	return 0;
}

void add_exported_option(char *key, char *value, int configured) {

	if (!uwsgi.exported_opts) {
		uwsgi.exported_opts = uwsgi_malloc(sizeof(struct uwsgi_opt*));
	}
	else {
		uwsgi.exported_opts = realloc(uwsgi.exported_opts, sizeof(struct uwsgi_opt*) * (uwsgi.exported_opts_cnt+1));
		if (!uwsgi.exported_opts) {
			uwsgi_error("realloc()");
			exit(1);
		}
	}

	uwsgi.exported_opts[uwsgi.exported_opts_cnt] = uwsgi_malloc(sizeof(struct uwsgi_opt));
	uwsgi.exported_opts[uwsgi.exported_opts_cnt]->key = key;
	uwsgi.exported_opts[uwsgi.exported_opts_cnt]->value = value;
	uwsgi.exported_opts[uwsgi.exported_opts_cnt]->configured = configured;
	uwsgi.exported_opts_cnt++;

}

int uwsgi_waitfd(int fd, int timeout) {

	int ret;
	struct pollfd upoll[1];
	char oob ;
	ssize_t rlen;


	if (!timeout) timeout = uwsgi.shared->options[UWSGI_OPTION_SOCKET_TIMEOUT];

	timeout = timeout*1000;
	if (timeout < 0) timeout = -1;

	upoll[0].fd = fd;
	upoll[0].events = POLLIN | POLLPRI;
	upoll[0].revents = 0;
	ret = poll(upoll, 1, timeout);

	if (ret < 0) {
		uwsgi_error("poll()");
	}
	else if (ret > 0) {
		if (upoll[0].revents & POLLIN) {
			return ret;
		}

		if (upoll[0].revents & POLLPRI) {
			uwsgi_log("DETECTED PRI DATA\n");
			rlen = recv(fd, &oob, 1, MSG_OOB);
			uwsgi_log("RECEIVE OOB DATA %d !!!\n", rlen);
			if (rlen < 0) {
				return -1;	
			}
			return 0;
		}
	}

	return ret;
}


inline void *uwsgi_malloc(size_t size) {

	char *ptr = malloc(size);
	if (ptr == NULL) {
		uwsgi_error("malloc()");
		exit(1);
	}	

	return ptr;
}


char *uwsgi_cheap_string(char *buf, int len) {

	int i;
	char *cheap_buf = buf-1;


	for(i=0;i<len;i++) {
		*cheap_buf++= buf[i];
	}

	
	buf[len-1] = 0;

	return buf-1;
}

char *uwsgi_resolve_ip(char *domain) {

	struct hostent *he;
	
	he = gethostbyname(domain);
	if (!he || !*he->h_addr_list || he->h_addrtype != AF_INET) {
		return NULL;
	}

	return inet_ntoa( *( struct in_addr*) he->h_addr_list[0]);
}

char *uwsgi_open_and_read(char *url, int *size, int add_zero, char *magic_table[]) {

	int fd;
	struct stat sb;
	char *buffer = NULL;
	char byte;
	ssize_t len;
	char *uri, *colon;
	char *domain ;
	char *ip ;
	int body = 0;
	char *magic_buf;

	// http url ?

	if (!strncmp("http://", url, 7)) {
		domain = url+7;
		uri = strchr(domain, '/');
		if (!uri) {
			uwsgi_log("invalid http url\n");
			exit(1);
		}	
		uri[0] = 0;
		uwsgi_log("domain: %s\n", domain);

		colon = uwsgi_get_last_char(domain, ':');

		if (colon) {
			colon[0] = 0;
		}


		ip = uwsgi_resolve_ip(domain);
		if (!ip) {
			uwsgi_log("unable to resolve address %s\n", domain);
			exit(1);
		}

		if (colon) {
			colon[0] = ':';
			ip = uwsgi_concat2(ip, colon);
		}
		else {
			ip = uwsgi_concat2(ip, ":80");
		}

		fd = uwsgi_connect(ip, 0, 0);
	
		if (fd < 0) {
			exit(1);
		}

		uri[0] = '/';

		len = write(fd, "GET ", 4);
		len = write(fd, uri, strlen(uri));
		len = write(fd, " HTTP/1.0\r\n", 11 );
		len = write(fd, "Host: ", 6);

		uri[0] = 0;
		len = write(fd, domain, strlen(domain));
		uri[0] = '/';

		len = write(fd, "\r\nUser-Agent: uWSGI on ", 23);
		len = write(fd, uwsgi.hostname, uwsgi.hostname_len);
		len = write(fd, "\r\n\r\n", 4);

		int http_status_code_ptr = 0;

		while( read(fd, &byte, 1) == 1) {
			if (byte == '\r' && body == 0) {
				body = 1;
			}
			else if (byte == '\n' && body == 1) {
				body = 2;
			}
			else if (byte == '\r' && body == 2) {
				body = 3;
			}
			else if (byte == '\n' && body == 3) {
				body = 4;
			}
			else if (body == 4) {
				*size = *size+1;
				buffer = realloc(buffer, *size);
				if (!buffer) {
					uwsgi_error("realloc()");
					exit(1);
				}
				buffer[*size-1] = byte;
			}
			else {
				body = 0;
				http_status_code_ptr++;
				if (http_status_code_ptr == 10) {
					if (byte != '2') {
						uwsgi_log("Not usable HTTP response: %cxx\n", byte);
						if (uwsgi.has_emperor) {
							exit(UWSGI_EXILE_CODE);
						}
						else {
							exit(1);
						}
					}
				}
			}
		}

		close(fd);

		if (add_zero) {
			*size = *size + 1;
			buffer = realloc(buffer, *size);
			buffer[*size-1] = 0;
		}

	}
	else if (!strncmp("emperor://", url, 10)) {
		if (uwsgi.emperor_fd_config < 0) {
			uwsgi_log("this is not a vassal instance\n");
			exit(1);
		}
		char *tmp_buffer[4096];
		ssize_t rlen = 1;
		*size = 0;
		while(rlen > 0) {
			rlen = read(uwsgi.emperor_fd_config, tmp_buffer, 4096);
			if (rlen > 0) {
				*size += rlen;
				buffer = realloc(buffer, *size);
				if (!buffer) {
					uwsgi_error("realloc()");
					exit(1);
				}
				memcpy(buffer+(*size-rlen), tmp_buffer, rlen);
			}
		}
		close(uwsgi.emperor_fd_config);
		uwsgi.emperor_fd_config = -1;

		if (add_zero) {
			*size = *size + 1;
			buffer = realloc(buffer, *size);
			buffer[*size-1] = 0;
		}
	}
	// fallback to file
	else {
		fd = open(url, O_RDONLY);
        	if (fd < 0) {
                	uwsgi_error_open(url);
                	exit(1);
        	}

        	if (fstat(fd, &sb)) {
                	uwsgi_error("fstat()");
                	exit(1);
        	}


        	buffer = malloc(sb.st_size+add_zero);

        	if (!buffer) {
                	uwsgi_error("malloc()");
                	exit(1);
        	}


        	len = read(fd, buffer, sb.st_size);
        	if (len != sb.st_size) {
                	uwsgi_error("read()");
                	exit(1);
        	}

		close(fd);

		*size = sb.st_size+add_zero;

		if (add_zero)
        		buffer[sb.st_size] = 0;
	}

	if (magic_table) {

		magic_buf = magic_sub(buffer, *size, size, magic_table);
		free(buffer);
		return magic_buf;
	}

	return buffer;
}

char *magic_sub(char *buffer, int len, int *size, char *magic_table[]) {

	int i;
	size_t magic_len = 0;
	char *magic_buf = uwsgi_malloc(len);
	char *magic_ptr = magic_buf;
	char *old_magic_buf;

	for(i=0;i<len;i++) {
		if (buffer[i] == '%' && (i+1) < len && magic_table[(int)buffer[i+1]]) {
			old_magic_buf = magic_buf;
			magic_buf = uwsgi_concat3n(old_magic_buf, magic_len, magic_table[(int)buffer[i+1]], strlen(magic_table[(int)buffer[i+1]]), buffer+i+2, len-i);
			free(old_magic_buf);
			magic_len+= strlen(magic_table[(int)buffer[i+1]]);
			magic_ptr = magic_buf + magic_len;
			i++;
		}
		else {
			*magic_ptr = buffer[i];
			magic_ptr++;
			magic_len++;
		}
	}

	*size = magic_len;

	return magic_buf;

}

void init_magic_table(char *magic_table[]) {

	int i;
	for(i=0;i<0xff;i++) {
		magic_table[i] = "";
	}

	magic_table['%'] = "%";
	magic_table['('] = "%(";
}

char *uwsgi_get_last_char(char *what, char c) {
	int i,j=0;
	char *ptr = NULL;

	if (!strncmp("http://", what, 7)) j = 7;
	if (!strncmp("emperor://", what, 10)) j = 10;

	for(i=j;i<(int)strlen(what);i++) {
		if (what[i] == c) {
			ptr = what+i;
		}
	}

	return ptr;
}

int uwsgi_attach_daemon(char *command) {

	struct uwsgi_daemon *d;
        int ret = -1;

        uwsgi_lock(uwsgi.daemon_table_lock);

        if (uwsgi.shared->daemons_cnt < MAX_DAEMONS) {
                d = &uwsgi.shared->daemons[uwsgi.shared->daemons_cnt];

                memcpy(d->command, command, UMIN(strlen(command), 0xff-1));
		d->registered = 0;
		d->status = 0;

                uwsgi.shared->daemons_cnt++;

                ret = 0;
                uwsgi_log("registered daemon %s\n", command);
        }

        uwsgi_unlock(uwsgi.daemon_table_lock);

        return ret;
	
}

void spawn_daemon(struct uwsgi_daemon *ud) {

	int i;
	char *argv[64];
	char *a;
	int cnt = 1;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, ud->pipe)) {
		uwsgi_error("socketpair()");
		return;
	}

	pid_t pid = fork();
	if (pid < 0) {
		uwsgi_error("fork()");
		return;
	}

	if (pid > 0) {
		close(ud->pipe[1]);
		ud->pid = pid;
		ud->status = 1;
		if (ud->respawns == 0) {
			ud->born = time(NULL);
		}

		ud->respawns++;
		ud->last_spawn = time(NULL);
	
	}
	else {

		// close uwsgi sockets
		for(i=0;i<uwsgi.sockets_cnt;i++) {
			close(uwsgi.sockets[i].fd);
		}
		close(ud->pipe[0]);

		// stdin will become the pipe
		if (ud->pipe[1] != 0) {
			if (dup2(ud->pipe[1], 0)) {
				uwsgi_error("dup2()");
				exit(1);
			}
		}

#ifdef __linux__
		if (prctl(PR_SET_PDEATHSIG, SIGKILL, 0,0,0)) {
                        uwsgi_error("prctl()");
                }
#endif
		memcpy(ud->tmp_command, ud->command, 0xff);

		a = strtok(ud->tmp_command, " ");
		if (a) {
			argv[0] = a;
        		while (a != NULL) {
                		a = strtok(NULL, " ");
				if (a) {
					argv[cnt] = a;
					cnt++;
				}
        		}
		}
		else {
			argv[0] = ud->tmp_command;
		}

		argv[cnt] = NULL;
		
		if (execvp(argv[0], argv)) {
			uwsgi_error("execvp()");
		}
	
		// never here;
		exit(1);
	}

	return;
}

char *uwsgi_num2str(int num) {
	
	char *str = uwsgi_malloc(11);	

	snprintf(str, 11, "%d", num);
	return str;
}

int uwsgi_num2str2(int num, char *ptr) {

	return snprintf(ptr, 11, "%d", num);
}

int is_unix(char *socket_name, int len) {
	int i;
	for(i=0;i<len;i++) {
		if (socket_name[i] == ':') return 0;
	}

	return 1;
}

int is_a_number(char *what) {
	int i;

	for(i=0;i<(int)strlen(what);i++) {
		if (!isdigit((int)what[i])) return 0;
	}

	return 1;
}

void uwsgi_unix_signal(int signum, void (*func)(int)) {
		
	struct sigaction   sa;

	memset(&sa, 0, sizeof(struct sigaction));

	sa.sa_handler = func;

	sigemptyset(&sa.sa_mask);

	if (sigaction(signum, &sa, NULL) < 0) {
		uwsgi_error("sigaction()");
	}
}

char *uwsgi_get_exported_opt(char *key) {

	int i;

	for (i = 0; i < uwsgi.exported_opts_cnt; i++) {
		if (!strcmp(uwsgi.exported_opts[i]->key, key)) {
			return uwsgi.exported_opts[i]->value;
		}	
	}

	return NULL;
}

char *uwsgi_get_optname_by_index(int index) {

        struct option *aopt;
	struct option *lopt = uwsgi.long_options;

        while ( (aopt = lopt) ) {
                if (!aopt->name) break;
		if (aopt->val == index) {
			return (char *) aopt->name;
		}
                lopt++;
        }

        return NULL;
}

int uwsgi_list_has_num(char *list, int num) {
	
	char *list2 = uwsgi_concat2(list, "");

	char *p = strtok(list2, ",");
        while (p != NULL) {
		if (atoi(p) == num) {
			free(list2);
			return 1;
		}
                p = strtok(NULL, ",");
	}

	free(list2);
	return 0;
}

int uwsgi_list_has_str(char *list, char *str) {
	
	char *list2 = uwsgi_concat2(list+1, "");

	char *p = strtok(list2, " ");
        while (p != NULL) {
		if (!strcasecmp(p, str)) {
			free(list2);
			return 1;
		}
                p = strtok(NULL, " ");
	}

	free(list2);
	return 0;
}

int uwsgi_str2_num(char *str) {

	int num = 0;

	num = 10 * (str[0] - 48);
	num += str[1] - 48;

	return num;
}

int uwsgi_str3_num(char *str) {

        int num = 0;

        num = 100 * (str[0] - 48);
        num += 10 * (str[1] - 48);
        num += str[2] - 48;

        return num;
}


int uwsgi_str4_num(char *str) {

	int num = 0;

	num = 1000 * (str[0] - 48);
	num += 100 * (str[1] - 48);
	num += 10 * (str[2] - 48);
	num += str[3] - 48;

	return num;
}

int uwsgi_str_num(char *str, int len) {
	
	int i;
	int num = 0;

	int delta = pow(10, len);

	for(i=0;i<len;i++) {
		delta = delta/10;
		num += delta * (str[i] - 48);
	}

	return num;
}
