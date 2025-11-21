/* */

/*-
 * Copyright 2011  Morgan Stanley and Co. Incorporated
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <stdarg.h>

#define MAXLOGLEN	1024

struct prefork_ctx;

static void	vlog(struct prefork_ctx *, int, const char *, va_list);
static void	pf_log(struct prefork_ctx *, int, const char *, ...);
static void	prefork_usage(struct prefork_ctx *, const char * const);
static int	prefork_process_args(struct prefork_ctx *, int, char * const *);
static int	prefork_setup(struct prefork_ctx *);
static void	main_loop(struct prefork_ctx *);
static void	start_kid(struct prefork_ctx *);
static void	sighndler(int);

int			die_now = 0;

struct prefork_ctx {
	char		 *progname;
	int		  timeout;
	int		  rate_limit;
	int		  sample_time;
	int		  state;
#define STATE_IDLE	0x1
#define STATE_BACKOFF	0x2
#define STATE_SAMPLING	0x3
	int		  cur_pmt;
	int		  num_kids;
	int		  min_kids;
	int		  max_kids;
	int		  debug;
	int		  fd;
	struct timeval	  wakeup;
	struct timeval	  last_select_seen;
	char		 *kid_prognam;
	char		*const *kid_args;
};

static void
vlog(struct prefork_ctx *ctx, int pri, const char *fmt, va_list ap)
{
	char	buf[MAXLOGLEN];

	if (ctx && !ctx->debug && pri == LOG_DEBUG)
		return;

	vsnprintf(buf, sizeof(buf), fmt, ap);
	syslog(pri, "%s", buf);
}

static void
pf_log(struct prefork_ctx *ctx, int pri, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vlog(ctx, pri, fmt, ap);
	va_end(ap);
}

static void
fatal(struct prefork_ctx *ctx, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vlog(ctx, LOG_ERR, fmt, ap);
	va_end(ap);
	exit(1);
}

static void
prefork_usage(struct prefork_ctx *ctx, const char * const prog)
{

	fatal(ctx, "usage: %s [-d] [-N max_kids] [-n min_kids] "
	      "[-r rate_limit] [-s sample_time]\n", prog);
}

static void
sighndler(int sig)
{

	/*
	 * If we're hupped or termed, we set a global and the rest
	 * of the program will gracefully shutdown.  Parent will kill
	 * the offspring, etc.
	 */

	switch (sig) {
	case SIGTERM:
	case SIGHUP:
		die_now = 1;
		break;

	default:
		break;
	}
}


/*
 * On linux, you have to prepend + to optstring to cause POSIX argument
 * processing to occur.  We hardcode this here rather than rely on the
 * user to set POSIXLY_CORRECT because for programs with a syntax that
 * accepts another program which has arguments, the GNU convention is
 * particularly difficult to use.
 */
#ifdef linux
#define POS "+"
#else
#define POS
#endif

static int
prefork_process_args(struct prefork_ctx *ctx, int argc,
		     char * const *argv)
{
	int	ch;

	memset(ctx, 0, sizeof(*ctx));

	ctx->progname		= strdup(*argv);
	ctx->rate_limit		= 32 * 1024;
	ctx->sample_time	= 16 * 1024;
	ctx->num_kids		= 0;
	ctx->min_kids		= 0;
	ctx->max_kids		= 10;
	ctx->fd			= -1;

	while ((ch = getopt(argc, argv, POS "?N:dn:r:s:")) != -1)
		switch (ch) {
		case 'N':
			ctx->max_kids = strtoul(optarg, NULL, 0);
			break;
		case 'd':
			ctx->debug = 1;
			break;
		case 'n':
			ctx->min_kids = strtoul(optarg, NULL, 0);
			break;
		case 'r':
			ctx->rate_limit = strtoul(optarg, NULL, 0);
			break;
		case 's':
			ctx->sample_time = strtoul(optarg, NULL, 0);
			break;
		case '?':
		default:
			prefork_usage(ctx, ctx->progname);
			break;
		}

	argc -= optind;
	argv += optind;

	ctx->kid_prognam = *argv;
	ctx->kid_args = argv;

	/* XXXrcd: some more sanity checking is required... */

	if (ctx->max_kids < ctx->min_kids) {
		pf_log(ctx, LOG_ERR, "max_kids (%d) < min_kids (%d)",
		    ctx->max_kids, ctx->min_kids);
		return 0;
	}

	return 1;
}

static int
prefork_setup(struct prefork_ctx *ctx)
{
	struct sigaction sa;

	pf_log(ctx, LOG_DEBUG, "enter prefork setup");

	memset(&sa, 0x0, sizeof(sa));
	sa.sa_handler = sighndler;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);

	if (sigaction(SIGCHLD, &sa, NULL) < 0)
		fatal(ctx, "could not reset SIGCHLD handler");

	if (sigaction(SIGHUP, &sa, NULL) < 0)
		fatal(ctx, "could not reset SIGHUP handler");

	if (sigaction(SIGTERM, &sa, NULL) < 0)
		fatal(ctx, "could not reset SIGTERM handler");

	if (setpgid(0, getpid()) == -1 && errno != EPERM)
		fatal(ctx, "failed to set process group: %s", strerror(errno));

	return 0;
}

struct timeval tv_zero = { 0, 0 };

static void
tv_normalise(struct timeval *t)
{

	while (t->tv_usec >= 1000000) {
		t->tv_usec -= 1000000;
		t->tv_sec  += 1;
	}

	while (t->tv_usec < 0) {
		t->tv_usec += 1000000;
		t->tv_sec  -= 1;
	}
}

static void
tv_plus(struct timeval *lhs, struct timeval *t1, struct timeval *t2)
{

	lhs->tv_sec  = t1->tv_sec  + t2->tv_sec;
	lhs->tv_usec = t1->tv_usec + t2->tv_usec;

	tv_normalise(lhs);
}

static void
tv_minus(struct timeval *lhs, struct timeval *t1, struct timeval *t2)
{

	lhs->tv_sec  = t1->tv_sec  - t2->tv_sec;
	lhs->tv_usec = t1->tv_usec - t2->tv_usec;

	tv_normalise(lhs);
}

static
int tv_gt(struct timeval *left, struct timeval *right)
{

	if (left->tv_sec > right->tv_sec)
		return 1;

	if (left->tv_sec < right->tv_sec)
		return 0;

	return left->tv_usec > right->tv_usec;
}

/*
 * wait_for_incoming() will return:
 *
 *	 1 if there is an outstanding connexion
 *	 0 if there are no outstanding connexions
 *	-1 if the idle timeout has been exceeded
 *
 * It encapsulates the logic about rate limiting and so on which are
 * defined in the ctx.
 */

static int
wait_for_incoming(struct prefork_ctx *ctx)
{
	struct timeval	cur;
	struct timeval	tv;
	fd_set		fds;
	int		ret;

	gettimeofday(&cur, NULL);

	if (ctx->state == STATE_BACKOFF) {
		tv.tv_sec  = ctx->rate_limit / 1000000;
		tv.tv_usec = ctx->rate_limit % 1000000;

		tv_plus(&ctx->wakeup, &tv, &cur);
		ctx->state = STATE_IDLE;
		return 0;
	}

	tv_minus(&tv, &ctx->wakeup, &cur);
	if (tv_gt(&tv, &tv_zero)) {
		select(0, NULL, NULL, NULL, &tv);
		return 0;
	}

	FD_ZERO(&fds);
	FD_SET(ctx->fd, &fds);

	tv.tv_sec  = 0;
	tv.tv_usec = 0;

	if (ctx->state == STATE_IDLE) {
		tv.tv_sec = 60;

		/*
		 * If we are idle, have a timeout and have no kids, then 
		 * we need to make sure that we do not select beyond our
		 * timeout value.  We return -1 if we should exit.
		 */

		if (ctx->timeout > 0 && ctx->num_kids == 0 &&
		    ctx->min_kids == 0) {
			tv.tv_sec  = ctx->timeout;

			tv_plus(&tv, &ctx->last_select_seen, &tv);
			tv_minus(&tv, &tv, &cur);

			if (tv_gt(&tv_zero, &tv))
				return -1;
		}
	}

	ret = select(ctx->fd + 1, &fds, NULL, NULL, &tv);

	if (ret == 0)
		ctx->state = STATE_IDLE;

	if (ret == -1 || ret == 0)
		return 0;

	gettimeofday(&ctx->last_select_seen, NULL);

	if (ctx->state == STATE_IDLE) {
		ctx->state = STATE_SAMPLING;
		ctx->cur_pmt = ctx->sample_time;
	}

	ctx->cur_pmt /= 2;

	/* If we have no children, then we do not need to sample */
	if (ctx->num_kids == 0 || ctx->cur_pmt < 1) {
		ctx->state = STATE_BACKOFF;
		return 1;
	}

	tv.tv_sec  = ctx->cur_pmt / 1000000;
	tv.tv_usec = ctx->cur_pmt % 1000000;

	gettimeofday(&cur, NULL);
	tv_plus(&ctx->wakeup, &cur, &tv);

	return 0;
}

/*
 * make_kid returns the number of kids it created, always 0 or 1.
 */

static int
make_kid(struct prefork_ctx *ctx)
{
	pid_t	pid;

	pid = fork();
	switch (pid) {
	case 0:
		start_kid(ctx);
		exit(0);
	case -1:
		pf_log(ctx, LOG_ERR, "fork failed: %s", strerror(errno));
		sleep(1);	/* back off */
		return 0;	/* not much to do but continue... */
	default:
		break;
	}
	return 1;
}

/*
 * The main loop maintains a set of pre-forked children that it will
 * try to ensure is at least min_kids---but it makes no guarantees of
 * this.  When the parent is able to non-blockingly accept a new
 * connexion, it will make a new kid.  It will also create kids any
 * time it makes it through the loop and the kid count < min_kids.
 * Since the only way we block is on a select on the accepting socket,
 * we can be assured that if there are pending connexions then the
 * loop will not become stuck.  We setup an empty SIG_CHLD handler
 * so that it is likely that we'll be bounced out of the select when
 * kids bite the dust, but we are not relying on this behaviour for
 * correct functioning---just for the cosmetics of burying the Zombies.
 */

static void
main_loop(struct prefork_ctx *ctx)
{
	int	status;
	int	ret;

	for (;;) {
		if (die_now) {			/* I've been told to die. */
			killpg(0, SIGHUP);	/* Kill the kids again */
			break;
		}

		if (ctx->num_kids >= ctx->max_kids) {
			if (waitpid(-1, &status, 0) != -1)
				ctx->num_kids--;
			else
				pf_log(ctx, LOG_ERR, "blocking waitpid(2): %s",
				    strerror(errno));
		}

		if (ctx->num_kids < ctx->min_kids) {
			ctx->num_kids += make_kid(ctx);
			continue;
		}

		while (waitpid(-1, &status, WNOHANG) > 0)
			ctx->num_kids--;

		ret = wait_for_incoming(ctx);

		pf_log(ctx, LOG_DEBUG, "incoming connexions = %d\n", ret);

		switch (ret) {
		case 0:
			continue;

		case -1:
			return;

		default:
			ctx->num_kids += make_kid(ctx);
			break;
		}
	}
}


static void
start_kid(struct prefork_ctx *ctx)
{
	int	  ret;

	ret = dup2(ctx->fd, 0);
	if (ret == -1) {
		pf_log(ctx, LOG_ERR, "dup2 failed: %s", strerror(errno));
		_exit(0);
	}

	if (ctx->fd > 0)
		close(ctx->fd);

	/* XXXrcd: deal with close-on-exec flags? */

	pf_log(ctx, LOG_INFO, "starting %s", ctx->kid_prognam);
	execv(ctx->kid_prognam, ctx->kid_args);

	pf_log(ctx, LOG_ERR, "execv failed: %s", strerror(errno));
	_exit(0); /* XXXrcd: no real better error code... */
}


static int
swizzle_fds(struct prefork_ctx *ctx)
{
	int	fd;
	int	nullfd;

        /* Stop dumb libraries (and us) from printing to the network */
        if ((nullfd = open("/dev/null", O_RDWR)) < 0) {
                pf_log(ctx, LOG_ERR, "can't open /dev/null: %s",
		    strerror(errno));
                return -1;
        }

        if (dup2(nullfd, STDOUT_FILENO) < 0) {
                pf_log(ctx, LOG_ERR, "failed to nullify STDOUT_FILENO: %s",
		    strerror(errno));
                return -1;
        }

        if (dup2(nullfd, STDERR_FILENO) < 0) {
                pf_log(ctx, LOG_ERR, "failed to nullify STDERR_FILENO: %s",
		    strerror(errno));
                return -1;
        }

	fd = dup(0);
	if (fd == -1) {
                pf_log(ctx, LOG_ERR, "failed to dup STDOUT_FILENO: %s",
		    strerror(errno));
                return -1;
	}

        if (dup2(nullfd, STDIN_FILENO) < 0) {
                pf_log(ctx, LOG_ERR, "failed to nullify STDOUT_FILENO: %s",
		    strerror(errno));
                return -1;
        }

	return fd;
}


int
main(int argc, char **argv)
{
	struct prefork_ctx	 the_ctx;
	struct prefork_ctx	*ctx = &the_ctx;

	openlog(*argv, LOG_PID, LOG_AUTH);

	syslog(LOG_INFO, "starting");
	if (!prefork_process_args(ctx, argc, argv))
		exit(1);

	prefork_setup(ctx);

	ctx->fd = swizzle_fds(ctx);
	if (ctx->fd == -1)
		exit(EXIT_FAILURE);

	main_loop(ctx);

	return 0;
}
