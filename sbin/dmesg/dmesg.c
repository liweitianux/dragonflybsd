/*-
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * @(#) Copyright (c) 1991, 1993 The Regents of the University of California.  All rights reserved.
 * @(#)dmesg.c	8.1 (Berkeley) 6/5/93
 * $FreeBSD: src/sbin/dmesg/dmesg.c,v 1.11.2.3 2001/08/08 22:32:15 obrien Exp $
 */

#include <sys/types.h>
#include <sys/msgbuf.h>
#include <sys/sysctl.h>
#include <sys/stat.h>

#include <err.h>
#include <fcntl.h>
#include <kvm.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vis.h>
#include <sys/syslog.h>

static struct nlist nl[] = {
#define	X_MSGBUF	0
	{ "msgbufp",	0, 0, 0, 0 },
#define	X_BOOTTIME	1
	{ "boottime",	0, 0, 0, 0 },
	{ NULL,		0, 0, 0, 0 },
};

/*
 * Kernel message timestamps have the form "[ 1234.567890] ", where the
 * fractional part can contain 0 to 9 digits.  A timestamp can optionally be
 * preceded by a "<pri>" syslog prefix, and the prefix is printed by dmesg -a.
 */
#define	TSMAX	32

/*
 * State carried across dumpbuf() calls.  The kvm and -f paths feed the
 * buffer in chunks, so a timestamp or priority prefix can straddle two
 * calls.
 */
struct dumpstate {
	int	newl;		/* at start of a line */
	int	skip;		/* suppress the rest of the line */
	int	pri;		/* accumulated syslog priority */
	int	atmsg;		/* at start of message content */
	int	pricand;	/* collecting a <pri> candidate */
	int	prilen;
	char	pribuf[16];
	int	tscand;		/* collecting a timestamp candidate */
	int	tslen;
	char	tsbuf[TSMAX];
	int	tsstate;
	int	tsfracdigits;
	unsigned long long tssec;
	unsigned long long tsfrac;
};

static void dumpbuf(char *bp, size_t bufpos, size_t buflen,
		    struct dumpstate *ds);
static void dumpflush(struct dumpstate *ds);
static void usage(void) __dead2;

#define	KREAD(addr, var) \
	kvm_read(kd, addr, &var, sizeof(var)) != sizeof(var)

#define	INCRBUFSIZE	65536

static int all_opt;
static int strip_opt;
static int human_opt;
static struct timespec boottime;

static void
emit_char(struct dumpstate *ds, int ch)
{
	char buf[5];

	vis(buf, ch, VIS_NOSLASH, 0);
	if (buf[1] == 0)
		putchar(buf[0]);
	else
		printf("%s", buf);
	ds->newl = (ch == '\n');
	ds->atmsg = (ch == '\n');
}

/*
 * Print a kernel timestamp as local wall-clock time.  If the conversion
 * fails, print the original text so no information is lost.
 */
static void
print_kernel_time(struct dumpstate *ds, unsigned long long sec,
    unsigned long long frac, int prec, const char *raw, int rawlen)
{
	static const long long tsdiv[10] = {
		1000000000LL, 100000000LL, 10000000LL, 1000000LL, 100000LL,
		10000LL, 1000LL, 100LL, 10LL, 1LL
	};
	struct tm tm;
	time_t t;
	char buf[64];
	long long nsec;
	int i;

	nsec = (prec > 0) ? (long long)(frac * tsdiv[prec]) : 0;
	t = boottime.tv_sec + (time_t)sec;
	if ((long long)boottime.tv_nsec + nsec >= 1000000000LL)
		t++;
	if (localtime_r(&t, &tm) != NULL &&
	    strftime(buf, sizeof(buf), "%a %b %e %H:%M:%S %Z %Y", &tm) != 0) {
		printf("[%s] ", buf);
		return;
	}
	for (i = 0; i < rawlen; i++)
		emit_char(ds, (u_char)raw[i]);
}

static void
ts_abort(struct dumpstate *ds)
{
	int i;

	ds->tscand = 0;
	for (i = 0; i < ds->tslen; i++)
		emit_char(ds, (u_char)ds->tsbuf[i]);
}

static void
ts_done(struct dumpstate *ds)
{
	if (human_opt)
		print_kernel_time(ds, ds->tssec, ds->tsfrac,
		    ds->tsfracdigits, ds->tsbuf, ds->tslen);
	ds->tscand = 0;
	ds->newl = 0;
	ds->atmsg = 0;
}

static void
pri_abort(struct dumpstate *ds)
{
	int i;

	ds->pricand = 0;
	emit_char(ds, '<');
	for (i = 0; i < ds->prilen; i++)
		emit_char(ds, (u_char)ds->pribuf[i]);
}

int
main(int argc, char **argv)
{
	struct dumpstate ds;
	struct msgbuf *bufp, cur;
	char *bp, *memf, *nlistf;
	kvm_t *kd;
	int ch;
	int clear = 0;
	int tailmode = 0;
	int kno;
	size_t buflen, bufpos;
	struct stat st;

	setlocale(LC_CTYPE, "");
	memf = nlistf = NULL;
	while ((ch = getopt(argc, argv, "acfM:N:n:tT")) != -1) {
		switch(ch) {
		case 'a':
			all_opt++;
			break;
		case 'c':
			clear = 1;
			break;
		case 'f':
			++tailmode;
			break;
		case 'M':
			memf = optarg;
			break;
		case 'N':
			nlistf = optarg;
			break;
		case 'n':
			kno = strtol(optarg, NULL, 0);
			asprintf(&memf, "vmcore.%d", kno);
			asprintf(&nlistf, "kern.%d", kno);
			if (stat(memf, &st) || stat(nlistf, &st)) {
				free(memf);
				free(nlistf);
				asprintf(&memf, "/var/crash/vmcore.%d", kno);
				asprintf(&nlistf, "/var/crash/kern.%d", kno);
			}
			break;
		case 't':
			strip_opt = 1;
			break;
		case 'T':
			human_opt = 1;
			break;
		case '?':
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (strip_opt && human_opt)
		errx(1, "-t and -T are mutually exclusive");

	memset(&ds, 0, sizeof(ds));
	ds.newl = 1;
	ds.atmsg = 1;

	if (memf == NULL && nlistf == NULL && tailmode == 0) {
		/* Running kernel. Use sysctl. */
		if (human_opt) {
			size_t len = sizeof(boottime);

			if (sysctlbyname("kern.boottime", &boottime, &len,
			    NULL, 0) == -1)
				err(1, "sysctl kern.boottime");
		}
		buflen = 0;
		if (sysctlbyname("kern.msgbuf", NULL, &buflen, NULL, 0) == -1)
			err(1, "sysctl kern.msgbuf");
		if (buflen == 0)	/* can happen if msgbuf was cleared */
			buflen = 1;

		if ((bp = malloc(buflen)) == NULL)
			errx(1, "malloc failed");
		if (sysctlbyname("kern.msgbuf", bp, &buflen, NULL, 0) == -1)
			err(1, "sysctl kern.msgbuf");
		/* We get a dewrapped buffer using sysctl. */
		bufpos = 0;
		dumpbuf(bp, bufpos, buflen, &ds);
	} else {
		u_int rindex;
		u_int xindex;
		u_int ri;
		u_int xi;
		u_int n;

		/* Read in kernel message buffer, do sanity checks. */
		kd = kvm_open(nlistf, memf, NULL, O_RDONLY, "dmesg");
		if (kd == NULL)
			exit (1);
		if (kvm_nlist(kd, nl) == -1)
			errx(1, "kvm_nlist: %s", kvm_geterr(kd));
		if (nl[X_MSGBUF].n_type == 0)
			errx(1, "%s: msgbufp not found",
			    nlistf ? nlistf : "namelist");
		if (human_opt) {
			if (nl[X_BOOTTIME].n_type == 0)
				errx(1, "%s: boottime not found",
				    nlistf ? nlistf : "namelist");
			if (KREAD(nl[X_BOOTTIME].n_value, boottime))
				errx(1, "kvm_read: %s", kvm_geterr(kd));
		}
		bp = malloc(INCRBUFSIZE);
		if (!bp)
			errx(1, "malloc failed");
		if (KREAD(nl[X_MSGBUF].n_value, bufp))
			errx(1, "kvm_read: %s", kvm_geterr(kd));
		if (KREAD((long)bufp, cur))
			errx(1, "kvm_read: %s", kvm_geterr(kd));
		if (cur.msg_magic != MSG_MAGIC && cur.msg_magic != MSG_OMAGIC)
			errx(1, "kernel message buffer has different magic "
			    "number");

		/*
		 * NOTE: current algorithm is compatible with both old and
		 *	 new msgbuf structures.  The new structure doesn't
		 *	 modulo the indexes (so we do), and adds a separate
		 *	 log index which we don't access here.
		 */

		rindex = cur.msg_bufr;

		for (;;) {
			/*
			 * Calculate index for dump and do sanity clipping.
			 */
			xindex = cur.msg_bufx;
			n = xindex - rindex;
			if (n > cur.msg_size - 1024) {
				rindex = xindex - cur.msg_size + 2048;
				n = xindex - rindex;
			}
			ri = rindex % cur.msg_size;
			xi = xindex % cur.msg_size;

			if (ri < xi)
				buflen = xi - ri;
			else
				buflen = cur.msg_size - ri;
			if (buflen > n)
				buflen = n;
			if (buflen > INCRBUFSIZE)
				buflen = INCRBUFSIZE;

			if (kvm_read(kd, (long)cur.msg_ptr + ri,
				     bp, buflen) != (ssize_t)buflen) {
				errx(1, "kvm_read: %s", kvm_geterr(kd));
			}
			if (buflen)
				dumpbuf(bp, 0, buflen, &ds);
			ri = (ri + buflen) % cur.msg_size;
			n = n - buflen;
			rindex += buflen;
			if ((int)n <= 0) {
				if (tailmode == 0)
					break;
				fflush(stdout);
				if (tailmode == 1)
					sleep(1);
			}
			if (KREAD((long)bufp, cur))
				errx(1, "kvm_read: %s", kvm_geterr(kd));
		}
		kvm_close(kd);
	}
	dumpflush(&ds);
	if (!ds.newl)
		putchar('\n');
	if (clear) {
		if (sysctlbyname("kern.msgbuf_clear", NULL, NULL,
				 &clear, sizeof(int)) != 0) {
			err(1, "sysctl kern.msgbuf_clear");
		}
	}
	return(0);
}

/*
 * Process one character from the kernel message buffer.
 *
 * The parser recognizes two optional prefixes at the start of a message:
 * a syslog "<pri>" sequence and a kernel timestamp.  Everything else is
 * printed after vis(3) encoding.
 */
static void
dumpbuf(char *bp, size_t bufpos, size_t buflen, struct dumpstate *ds)
{
	int ch;
	char *p, *ep;

	/*
	 * The message buffer is circular.  If the buffer has wrapped, the
	 * write pointer points to the oldest data.  Otherwise, the write
	 * pointer points to \0's following the data.  Read the entire
	 * buffer starting at the write pointer and ignore nulls so that
	 * we effectively start at the oldest data.
	 */
	p = bp + bufpos;
	ep = (bufpos == 0 ? bp + buflen : p);
	do {
		if (p == bp + buflen)
			p = bp;
		ch = *p;
		if (ch == '\0')
			continue;

		if (ds->skip) {
			if (ch == '\n') {
				ds->skip = 0;
				ds->newl = 1;
				ds->atmsg = 1;
			}
			continue;
		}

		if (ds->pricand) {
			if (ch >= '0' && ch <= '9' && ds->prilen < 9) {
				ds->pribuf[ds->prilen++] = ch;
				ds->pri = ds->pri * 10 + (ch - '0');
				continue;
			}
			if (ch == '>') {
				ds->pricand = 0;
				if (all_opt) {
					int i;

					emit_char(ds, '<');
					for (i = 0; i < ds->prilen; i++)
						emit_char(ds,
						    (u_char)ds->pribuf[i]);
					emit_char(ds, '>');
					ds->newl = 0;
					ds->atmsg = 1;
				} else {
					ds->newl = 0;
					ds->atmsg = 1;
					if (LOG_FAC(ds->pri) != LOG_KERN)
						ds->skip = 1;
				}
				continue;
			}
			pri_abort(ds);
			/* FALLTHROUGH and process ch normally. */
		}

		if (ds->tscand) {
			int bad = 0;

			if (ds->tslen >= TSMAX - 1) {
				ts_abort(ds);
				bad = 2;		/* ch was not buffered */
			} else {
				ds->tsbuf[ds->tslen++] = ch;
				switch (ds->tsstate) {
				case 0:		/* after '[' */
					if (ch == ' ')
						break;
					if (ch >= '0' && ch <= '9') {
						ds->tssec = ch - '0';
						ds->tsstate = 1;
						break;
					}
					bad = 1;
					break;
				case 1:		/* seconds */
					if (ch >= '0' && ch <= '9') {
						if (ds->tssec >
						    (ULLONG_MAX -
						     (ch - '0')) / 10) {
							bad = 1;
							break;
						}
						ds->tssec = ds->tssec * 10 +
						    (ch - '0');
					} else if (ch == '.') {
						ds->tsstate = 2;
					} else if (ch == ']') {
						ds->tsstate = 3;
					} else {
						bad = 1;
					}
					break;
				case 2:		/* fraction */
					if (ch >= '0' && ch <= '9') {
						if (ds->tsfracdigits >= 9) {
							bad = 1;
							break;
						}
						ds->tsfrac = ds->tsfrac * 10 +
						    (ch - '0');
						ds->tsfracdigits++;
					} else if (ch == ']') {
						ds->tsstate = 3;
					} else {
						bad = 1;
					}
					break;
				case 3:		/* trailing space */
					if (ch == ' ')
						ts_done(ds);
					else
						bad = 1;
					break;
				}
			}
			if (bad == 0)
				continue;
			if (bad == 1) {
				ts_abort(ds);
				continue;
			}
			/* FALLTHROUGH and process ch normally if not buffered. */
		}

		if (ds->newl && ch == '<') {
			ds->pricand = 1;
			ds->prilen = 0;
			ds->pri = 0;
			continue;
		}
		if (ds->atmsg && (strip_opt || human_opt) && ch == '[') {
			ds->tscand = 1;
			ds->tslen = 0;
			ds->tsstate = 0;
			ds->tsfrac = 0;
			ds->tsfracdigits = 0;
			ds->tsbuf[ds->tslen++] = ch;
			continue;
		}
		emit_char(ds, ch);
	} while (++p != ep);
}

/*
 * Flush a partial prefix at the end of the buffer.
 */
static void
dumpflush(struct dumpstate *ds)
{
	if (ds->pricand)
		pri_abort(ds);
	if (ds->tscand)
		ts_abort(ds);
}

static void
usage(void)
{
	fprintf(stderr, "usage: dmesg [-acf] [-M core] [-N system] "
	    "[-n dumpnr] [-t | -T]\n");
	exit(1);
}
