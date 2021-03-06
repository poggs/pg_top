/*
 * pg_top - a top PostgreSQL users display for Unix
 *
 * SYNOPSIS:  Linux 1.2.x, 1.3.x, 2.x, using the /proc filesystem
 *
 * DESCRIPTION:
 * This is the machine-dependent module for Linux 1.2.x, 1.3.x or 2.x.
 *
 * LIBS:
 *
 * CFLAGS: -DHAVE_GETOPT -DHAVE_STRERROR -DORDER
 *
 * TERMCAP: -lcurses
 *
 * AUTHOR: Richard Henderson <rth@tamu.edu>
 * Order support added by Alexey Klimkin <kad@klon.tme.mcst.ru>
 * Ported to 2.4 by William LeFebvre
 */

#include "config.h"

#include <sys/types.h>
#include <time.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <bsd/stdlib.h>
#include <bsd/sys/tree.h>
#include <errno.h>
#include <dirent.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/vfs.h>

#include <sys/param.h>			/* for HZ */

#if 0
#include <linux/proc_fs.h>		/* for PROC_SUPER_MAGIC */
#else
#define PROC_SUPER_MAGIC 0x9fa0
#endif

#define BUFFERLEN 255
#define GET_VALUE(v) \
		p = strchr(p, ':'); \
		++p; \
		++p; \
		q = strchr(p, '\n'); \
		len = q - p; \
		if (len >= BUFFERLEN) \
		{ \
			printf("ERROR - value is larger than the buffer: %d\n", __LINE__); \
			exit(1); \
		} \
		strncpy(value, p, len); \
		value[len] = '\0'; \
		v = atoll(value);

#include "machine.h"
#include "utils.h"

#define PROCFS "/proc"
extern char *myname;

/*=PROCESS INFORMATION==================================================*/

struct top_proc
{
	RB_ENTRY(top_proc) entry;
	pid_t		pid;

	/* index for which element is current in data arrays */
	int index;

	/* Data from /proc/<pid>/stat. */
	char	   *name;
	char	   *usename;
	unsigned long size,
				rss;			/* in k */
	int			state;
	int			pgstate;
	unsigned long time;
	unsigned long start_time;
	unsigned long xtime;
	unsigned long qtime;
	unsigned int locks;
	double		pcpu;

	/* Data from /proc/<pid>/io. */
	long long	iops[2]; /* syscr + syscw */
	long long	syscr[2];
	long long	syscw[2];
	long long	read_bytes[2];
	long long	write_bytes[2];

	/* Replication data */
	char	   *application_name;
	char	   *client_addr;
	char	   *repstate;
	char	   *primary;
	char	   *sent;
	char	   *write;
	char	   *flush;
	char	   *replay;
	long long	sent_lag;
	long long	write_lag;
	long long	flush_lag;
	long long	replay_lag;
};

int			topproccmp(struct top_proc *, struct top_proc *);

RB_HEAD(pgproc, top_proc) head_proc = RB_INITIALIZER(&head_proc);
RB_PROTOTYPE(pgproc, top_proc, entry, topproccmp)
RB_GENERATE(pgproc, top_proc, entry, topproccmp)

double		timediff;

/*=STATE IDENT STRINGS==================================================*/

#define NCPUSTATES 5
static char *cpustatenames[NCPUSTATES + 1] =
{
	"user", "nice", "system", "idle", "iowait",
	NULL
};
static int	show_iowait = 0;

#define MEMUSED    0
#define MEMFREE    1
#define MEMSHARED  2
#define MEMBUFFERS 3
#define MEMCACHED  4
#define NMEMSTATS  5
static char *memorynames[NMEMSTATS + 1] =
{
	"K used, ", "K free, ", "K shared, ", "K buffers, ", "K cached",
	NULL
};

enum swap
{
	SWAPUSED,
	SWAPFREE,
	SWAPCACHED,
	SWAPIN,
	SWAPOUT,
	NSWAPSTATS
};
static char *swapnames[NSWAPSTATS + 1] =
{
	"K used, ", "K free, ", "K cached, ", "K in, ", "K out", NULL
};

struct swap_t
{
	int index;
	long long in[2];
	long long out[2];
} swap_activity;

static char fmt_header[] =
"    PID X           SIZE   RES STATE   XTIME  QTIME  %CPU LOCKS COMMAND";

char		fmt_header_io[] =
"    PID  IOPS   IORPS   IOWPS READS WRITES COMMAND";

/* these are names given to allowed sorting orders -- first is default */
static char *ordernames[] =
{
	"cpu", "size", "res", "xtime", "qtime", "iops", "iorps", "iowps", "reads",
	"writes", "locks", "command", "flag", "rlag", "slag", "wlag", NULL
};

/* forward definitions for comparison functions */
static int	compare_cmd(const void *, const void *);
static int	compare_cpu(const void *, const void *);
static int	compare_iops(const void *, const void *);
static int	compare_lag_flush(const void *, const void *);
static int	compare_lag_replay(const void *, const void *);
static int	compare_lag_sent(const void *, const void *);
static int	compare_lag_write(const void *, const void *);
static int	compare_locks(const void *, const void *);
static int	compare_qtime(const void *, const void *);
static int	compare_reads(const void *, const void *);
static int	compare_res(const void *, const void *);
static int	compare_size(const void *, const void *);
static int	compare_syscr(const void *, const void *);
static int	compare_syscw(const void *, const void *);
static int	compare_writes(const void *, const void *);
static int	compare_xtime(const void *, const void *);

int			(*proc_compares[]) () =
{
	compare_cpu,
		compare_size,
		compare_res,
		compare_xtime,
		compare_qtime,
		compare_iops,
		compare_syscr,
		compare_syscw,
		compare_reads,
		compare_writes,
		compare_locks,
		compare_cmd,
		compare_lag_flush,
		compare_lag_replay,
		compare_lag_sent,
		compare_lag_write,
		NULL
};

/*=SYSTEM STATE INFO====================================================*/

/* these are for calculating cpu state percentages */

static int64_t cp_time[NCPUSTATES];
static int64_t cp_old[NCPUSTATES];
static int64_t cp_diff[NCPUSTATES];

/* for calculating the exponential average */

static struct timeval lasttime;

/* these are for keeping track of processes */

#define INITIAL_ACTIVE_SIZE  (256)
#define PROCBLOCK_SIZE		 (32)
static struct top_proc *pgtable;
static int	proc_index;
static time_t boottime = -1;

/* these are for passing data back to the machine independant portion */

static int64_t cpu_states[NCPUSTATES];
static int	process_states[NPROCSTATES];
static long memory_stats[NMEMSTATS];
static long swap_stats[NSWAPSTATS];

/* usefull macros */
#define bytetok(x)	(((x) + 512) >> 10)
#define pagetok(x)	((x) * sysconf(_SC_PAGESIZE) >> 10)

/*======================================================================*/

static inline long long
diff_stat(long long value[2], int index)
{
	return value[index] - value[(index + 1) % 2];
}

static inline char *
skip_ws(const char *p)
{
	while (isspace(*p))
		p++;
	return (char *) p;
}

static inline char *
skip_token(const char *p)
{
	while (isspace(*p))
		p++;
	while (*p && !isspace(*p))
		p++;
	return (char *) p;
}

int
topproccmp(struct top_proc *e1, struct top_proc *e2)
{
	return (e1->pid < e2->pid ? -1 : e1->pid > e2->pid);
}

static void
xfrm_cmdline(char *p, int len)
{
	while (--len > 0)
	{
		if (*p == '\0')
		{
			*p = ' ';
		}
		p++;
	}
}

int
machine_init(struct statics *statics)
{
	/* make sure the proc filesystem is mounted */
	{
		struct statfs sb;

		if (statfs(PROCFS, &sb) < 0 || sb.f_type != PROC_SUPER_MAGIC)
		{
			fprintf(stderr, "%s: proc filesystem not mounted on " PROCFS "\n",
					myname);
			return -1;
		}
	}

	/* chdir to the proc filesystem to make things easier */
	chdir(PROCFS);

	/* a few preliminary checks */
	{
		int			fd;
		char		buff[128];
		char	   *p;
		int			cnt = 0;
		unsigned long uptime;
		struct timeval tv;

		/* get a boottime */
		if ((fd = open("uptime", 0)) != -1)
		{
			if (read(fd, buff, sizeof(buff)) > 0)
			{
				uptime = strtoul(buff, &p, 10);
				gettimeofday(&tv, 0);
				boottime = tv.tv_sec - uptime;
			}
			close(fd);
		}

		/* see how many states we get from stat */
		if ((fd = open("stat", 0)) != -1)
		{
			if (read(fd, buff, sizeof(buff)) > 0)
			{
				if ((p = strchr(buff, '\n')) != NULL)
				{
					*p = '\0';
					p = buff;
					cnt = 0;
					while (*p != '\0')
					{
						if (*p++ == ' ')
						{
							cnt++;
						}
					}
				}
			}

			close(fd);
		}
		if (cnt > 5)
		{
			/* we have iowait */
			show_iowait = 1;
		}
	}

	/* if we aren't showing iowait, then we have to tweak cpustatenames */
	if (!show_iowait)
	{
		cpustatenames[4] = NULL;
	}

	/* fill in the statics information */
	statics->procstate_names = procstatenames;
	statics->cpustate_names = cpustatenames;
	statics->memory_names = memorynames;
	statics->swap_names = swapnames;
	statics->order_names = ordernames;
	statics->boottime = boottime;
	statics->flags.fullcmds = 1;
	statics->flags.warmup = 1;

	/* all done! */
	return 0;
}

void
get_system_info(struct system_info *info)
{
	char		buffer[4096 + 1];
	int			fd,
				len;
	char	   *p;

	/* get load averages */

	if ((fd = open("loadavg", O_RDONLY)) != -1)
	{
		if ((len = read(fd, buffer, sizeof(buffer) - 1)) > 0)
		{
			buffer[len] = '\0';
			info->load_avg[0] = strtod(buffer, &p);
			info->load_avg[1] = strtod(p, &p);
			info->load_avg[2] = strtod(p, &p);
			p = skip_token(p);	/* skip running/tasks */
			p = skip_ws(p);
			if (*p)
			{
				info->last_pid = atoi(p);
			}
			else
			{
				info->last_pid = -1;
			}
		}
		close(fd);
	}

	/* get the cpu time info */
	if ((fd = open("stat", O_RDONLY)) != -1)
	{
		if ((len = read(fd, buffer, sizeof(buffer) - 1)) > 0)
		{
			buffer[len] = '\0';
			p = skip_token(buffer); /* "cpu" */
			cp_time[0] = strtoul(p, &p, 0);
			cp_time[1] = strtoul(p, &p, 0);
			cp_time[2] = strtoul(p, &p, 0);
			cp_time[3] = strtoul(p, &p, 0);
			if (show_iowait)
			{
				cp_time[4] = strtoul(p, &p, 0);
			}

			/* convert cp_time counts to percentages */
			percentages(NCPUSTATES, cpu_states, cp_time, cp_old, cp_diff);
		}
		close(fd);
	}

	/* get system wide memory usage */
	if ((fd = open("meminfo", O_RDONLY)) != -1)
	{
		char	   *p;
		int			mem = 0;
		int			swap = 0;
		unsigned long memtotal = 0;
		unsigned long memfree = 0;
		unsigned long swaptotal = 0;

		if ((len = read(fd, buffer, sizeof(buffer) - 1)) > 0)
		{
			buffer[len] = '\0';
			p = buffer - 1;

			/* iterate thru the lines */
			while (p != NULL)
			{
				p++;
				if (p[0] == ' ' || p[0] == '\t')
				{
					/* skip */
				}
				else if (strncmp(p, "Mem:", 4) == 0)
				{
					p = skip_token(p);	/* "Mem:" */
					p = skip_token(p);	/* total memory */
					memory_stats[MEMUSED] = strtoul(p, &p, 10);
					memory_stats[MEMFREE] = strtoul(p, &p, 10);
					memory_stats[MEMSHARED] = strtoul(p, &p, 10);
					memory_stats[MEMBUFFERS] = strtoul(p, &p, 10);
					memory_stats[MEMCACHED] = strtoul(p, &p, 10);
					memory_stats[MEMUSED] = bytetok(memory_stats[MEMUSED]);
					memory_stats[MEMFREE] = bytetok(memory_stats[MEMFREE]);
					memory_stats[MEMSHARED] = bytetok(memory_stats[MEMSHARED]);
					memory_stats[MEMBUFFERS] =
						bytetok(memory_stats[MEMBUFFERS]);
					memory_stats[MEMCACHED] = bytetok(memory_stats[MEMCACHED]);
					mem = 1;
				}
				else if (strncmp(p, "Swap:", 5) == 0)
				{
					p = skip_token(p);	/* "Swap:" */
					p = skip_token(p);	/* total swap */
					swap_stats[SWAPUSED] = strtoul(p, &p, 10);
					swap_stats[SWAPFREE] = strtoul(p, &p, 10);
					swap_stats[SWAPUSED] = bytetok(swap_stats[SWAPUSED]);
					swap_stats[SWAPFREE] = bytetok(swap_stats[SWAPFREE]);
					swap = 1;
				}
				else if (!mem && strncmp(p, "MemTotal:", 9) == 0)
				{
					p = skip_token(p);
					memtotal = strtoul(p, &p, 10);
				}
				else if (!mem && memtotal > 0 && strncmp(p, "MemFree:", 8) == 0)
				{
					p = skip_token(p);
					memfree = strtoul(p, &p, 10);
					memory_stats[MEMUSED] = memtotal - memfree;
					memory_stats[MEMFREE] = memfree;
				}
				else if (!mem && strncmp(p, "MemShared:", 10) == 0)
				{
					p = skip_token(p);
					memory_stats[MEMSHARED] = strtoul(p, &p, 10);
				}
				else if (!mem && strncmp(p, "Buffers:", 8) == 0)
				{
					p = skip_token(p);
					memory_stats[MEMBUFFERS] = strtoul(p, &p, 10);
				}
				else if (!mem && strncmp(p, "Cached:", 7) == 0)
				{
					p = skip_token(p);
					memory_stats[MEMCACHED] = strtoul(p, &p, 10);
				}
				else if (!swap && strncmp(p, "SwapTotal:", 10) == 0)
				{
					p = skip_token(p);
					swaptotal = strtoul(p, &p, 10);
				}
				else if (!swap && swaptotal > 0 && strncmp(p, "SwapFree:", 9) == 0)
				{
					p = skip_token(p);
					memfree = strtoul(p, &p, 10);
					swap_stats[SWAPUSED] = swaptotal - memfree;
					swap_stats[SWAPFREE] = memfree;
				}
				else if (!mem && strncmp(p, "SwapCached:", 11) == 0)
				{
					p = skip_token(p);
					swap_stats[SWAPCACHED] = strtoul(p, &p, 10);
				}

				/* move to the next line */
				p = strchr(p, '\n');
			}
		}
		close(fd);
	}

	/* get swap activity */
	if ((fd = open("vmstat", O_RDONLY)) != -1)
	{
		unsigned long swpin = -1;
		unsigned long swpout = -1;

		if ((len = read(fd, buffer, sizeof(buffer) - 1)) > 0)
		{
			buffer[len] = '\0';
			p = buffer - 1;
			while (p != NULL)
			{
				p++;

				if (swpin == -1 && strncmp(p, "pswpin", 6) == 0)
				{
					p = skip_token(p);
					swpin = strtoul(p, &p, 10);
				}
				else if (swpout == -1 && strncmp(p, "pswpout", 7) == 0)
				{
					p = skip_token(p);
					swpout = strtoul(p, &p, 10);
				}

				if (swpin != -1 && swpout != -1)
				{
					swap_activity.in[swap_activity.index] = swpin;
					swap_activity.out[swap_activity.index] = swpout;

					swap_stats[SWAPIN] = diff_stat(swap_activity.in,
							swap_activity.index);
					swap_stats[SWAPOUT] = diff_stat(swap_activity.out,
							swap_activity.index);


					swap_activity.index = (swap_activity.index + 1) % 2;
					break;
				}

				/* move to the next line */
				p = strchr(p, '\n');
			}
		}
		close(fd);
	}
	else
	{
		swap_activity.in[swap_activity.index] = -1;
		swap_activity.out[swap_activity.index] = -1;
	}

	/* set arrays and strings */
	info->cpustates = cpu_states;
	info->memory = memory_stats;
	info->swap = swap_stats;
}

static void
read_one_proc_stat(struct top_proc *proc, struct process_select *sel)
{
	char		buffer[4096],
			   *p,
			   *q;
	int			fd,
				len;
	int			fullcmd;
	char		value[BUFFERLEN + 1];

	long long	tmp;

	/* if anything goes wrong, we return with proc->state == 0 */
	proc->state = 0;

	/* full cmd handling */
	fullcmd = sel->fullcmd;
	if (fullcmd == 1)
	{
		sprintf(buffer, "%d/cmdline", proc->pid);
		if ((fd = open(buffer, O_RDONLY)) != -1)
		{
			/* read command line data */
			/* (theres no sense in reading more than we can fit) */
			if ((len = read(fd, buffer, MAX_COLS)) > 1)
			{
				buffer[len] = '\0';
				xfrm_cmdline(buffer, len);
				update_str(&proc->name, buffer);
				printable(proc->name);
			}
			else
			{
				fullcmd = 0;
			}
			close(fd);
		}
		else
		{
			fullcmd = 0;
		}
	}

	/* grab the proc stat info in one go */
	sprintf(buffer, "%d/stat", proc->pid);

	fd = open(buffer, O_RDONLY);
	len = read(fd, buffer, sizeof(buffer) - 1);
	close(fd);

	buffer[len] = '\0';

	/* parse out the status, described in 'man proc' */

	/* skip pid and locate command, which is in parentheses */
	if ((p = strchr(buffer, '(')) == NULL)
	{
		return;
	}
	if ((q = strrchr(++p, ')')) == NULL)
	{
		return;
	}

	/* set the procname */
	*q = '\0';
	if (!fullcmd)
	{
		update_str(&proc->name, p);
		printable(proc->name);
	}

	/* scan the rest of the line */
	p = q + 1;
	p = skip_ws(p);
	switch (*p++)				/* state */
	{
		case 'R':
			proc->state = 1;
			break;
		case 'S':
			proc->state = 2;
			break;
		case 'D':
			proc->state = 3;
			break;
		case 'Z':
			proc->state = 4;
			break;
		case 'T':
			proc->state = 5;
			break;
		case 'W':
			proc->state = 6;
			break;
		case '\0':
			return;
	}

	p = skip_token(p);			/* skip ppid */
	p = skip_token(p);			/* skip pgrp */
	p = skip_token(p);			/* skip session */
	p = skip_token(p);			/* skip tty nr */
	p = skip_token(p);			/* skip tty pgrp */
	p = skip_token(p);			/* skip flags */
	p = skip_token(p);			/* skip min flt */
	p = skip_token(p);			/* skip cmin flt */
	p = skip_token(p);			/* skip maj flt */
	p = skip_token(p);			/* skip cmaj flt */

	proc->time = strtoul(p, &p, 10);	/* utime */
	proc->time += strtoul(p, &p, 10);	/* stime */

	p = skip_token(p);			/* skip cutime */
	p = skip_token(p);			/* skip cstime */
	p = skip_token(p);			/* skip priority */
	p = skip_token(p);			/* skip nice */
	p = skip_token(p);			/* skip num_threads */
	p = skip_token(p);			/* skip itrealvalue, 0 */
	proc->start_time = strtoul(p, &p, 10);	/* start_time */
	proc->size = bytetok(strtoul(p, &p, 10));	/* vsize */
	proc->rss = pagetok(strtoul(p, &p, 10));	/* rss */

#if 0
	/* for the record, here are the rest of the fields */
	p = skip_token(p);			/* skip rlim */
	p = skip_token(p);			/* skip start_code */
	p = skip_token(p);			/* skip end_code */
	p = skip_token(p);			/* skip start_stack */
	p = skip_token(p);			/* skip esp */
	p = skip_token(p);			/* skip eip */
	p = skip_token(p);			/* skip signal */
	p = skip_token(p);			/* skip sigblocked */
	p = skip_token(p);			/* skip sigignore */
	p = skip_token(p);			/* skip sigcatch */
	p = skip_token(p);			/* skip wchan */
	p = skip_token(p);			/* skip nswap, not maintained */
	p = skip_token(p);			/* exit signal */
	p = skip_token(p);			/* processor */
	p = skip_token(p);			/* rt_priority */
	p = skip_token(p);			/* policy */
	p = skip_token(p);			/* delayacct_blkio_ticks */
#endif

	/* Get the io stats. */
	sprintf(buffer, "%d/io", proc->pid);
	fd = open(buffer, O_RDONLY);
	if (fd == -1)
	{
		/*
		 * CONFIG_TASK_IO_ACCOUNTING is not enabled in the Linux kernel or
		 * this version of Linux may not support collecting i/o statistics per
		 * pid.
		 */
		return;
	}
	len = read(fd, buffer, sizeof(buffer) - 1);
	close(fd);

	buffer[len] = '\0';
	p = buffer;

	p = skip_token(p);			/* rchar */
	p = skip_token(p);			/* wchar */

	GET_VALUE(tmp);				/* syscr */
	proc->syscr[proc->index] = tmp;
	proc->iops[proc->index] = tmp;

	GET_VALUE(tmp);				/* syscw */
	proc->syscw[proc->index] = tmp;
	proc->iops[proc->index] += tmp;

	GET_VALUE(tmp);				/* read_bytes */
	proc->read_bytes[proc->index] = tmp;

	GET_VALUE(tmp);				/* write_bytes */
	proc->write_bytes[proc->index] = tmp;

	GET_VALUE(tmp);				/* cancelled_write_bytes */
	proc->write_bytes[proc->index] -= tmp;
}

caddr_t
get_process_info(struct system_info *si,
				 struct process_select *sel,
				 int compare_index, struct pg_conninfo_ctx *conninfo, int mode)
{
	struct timeval thistime;
	double		tickdiff;

	/* calculate the time difference since our last check */
	gettimeofday(&thistime, 0);
	if (lasttime.tv_sec)
	{
		timediff = ((thistime.tv_sec - lasttime.tv_sec) +
					(thistime.tv_usec - lasttime.tv_usec) * 1e-6);
	}
	else
	{
		timediff = 0;
	}
	lasttime = thistime;

	tickdiff = timediff * HZ;				/* convert to ticks */

	/* read the process information */
	{
		int			total_procs = 0;
		int			active_procs = 0;

		int			show_idle = sel->idle;

		int			i;
		int			rows;
		PGresult   *pgresult = NULL;

		struct top_proc *n,
				   *p;

		memset(process_states, 0, sizeof(process_states));

		connect_to_db(conninfo);
		if (conninfo->connection != NULL)
		{
			if (mode == MODE_REPLICATION)
			{
				pgresult = pg_replication(conninfo->connection);
			}
			else
			{
				pgresult = pg_processes(conninfo->connection);
			}
			rows = PQntuples(pgresult);
		}
		else
		{
			rows = 0;
		}

		if (rows > 0)
		{
			p = reallocarray(pgtable, rows, sizeof(struct top_proc));
			if (p == NULL)
			{
				fprintf(stderr, "reallocarray error\n");
				if (pgresult != NULL)
					PQclear(pgresult);
				disconnect_from_db(conninfo);
				exit(1);
			}
			pgtable = p;
		}

		for (i = 0; i < rows; i++)
		{
			unsigned long otime;

			n = malloc(sizeof(struct top_proc));
			if (n == NULL)
			{
				fprintf(stderr, "malloc error\n");
				if (pgresult != NULL)
					PQclear(pgresult);
				disconnect_from_db(conninfo);
				exit(1);
			}
			memset(n, 0, sizeof(struct top_proc));
			n->pid = atoi(PQgetvalue(pgresult, i, 0));
			p = RB_INSERT(pgproc, &head_proc, n);
			if (p != NULL)
			{
				free(n);
				n = p;
			}
			else
			{
				n->time = 0;
			}

			otime = n->time;

			if (mode == MODE_REPLICATION)
			{
				update_str(&n->usename, PQgetvalue(pgresult, i, REP_USENAME));
				update_str(&n->application_name,
						PQgetvalue(pgresult, i, REP_APPLICATION_NAME));
				update_str(&n->client_addr,
						PQgetvalue(pgresult, i, REP_CLIENT_ADDR));
				update_str(&n->repstate, PQgetvalue(pgresult, i, REP_STATE));
				update_str(&n->primary,
						PQgetvalue(pgresult, i, REP_WAL_INSERT));
				update_str(&n->sent, PQgetvalue(pgresult, i, REP_SENT));
				update_str(&n->write, PQgetvalue(pgresult, i, REP_WRITE));
				update_str(&n->flush, PQgetvalue(pgresult, i, REP_FLUSH));
				update_str(&n->replay, PQgetvalue(pgresult, i, REP_REPLAY));
				n->sent_lag = atol(PQgetvalue(pgresult, i, REP_SENT_LAG));
				n->write_lag = atol(PQgetvalue(pgresult, i, REP_WRITE_LAG));
				n->flush_lag = atol(PQgetvalue(pgresult, i, REP_FLUSH_LAG));
				n->replay_lag = atol(PQgetvalue(pgresult, i, REP_REPLAY_LAG));

				memcpy(&pgtable[active_procs++], n, sizeof(struct top_proc));
			}
			else
			{
				read_one_proc_stat(n, sel);
				if (sel->fullcmd == 2)
				{
					update_str(&n->name, PQgetvalue(pgresult, i, PROC_QUERY));
					printable(n->name);
				}
				update_state(&n->pgstate, PQgetvalue(pgresult, i, PROC_STATE));
				update_str(&n->usename, PQgetvalue(pgresult, i, PROC_USENAME));
				n->xtime = atol(PQgetvalue(pgresult, i, PROC_XSTART));
				n->qtime = atol(PQgetvalue(pgresult, i, PROC_QSTART));
				n->locks = atoi(PQgetvalue(pgresult, i, PROC_LOCKS));

				process_states[n->pgstate]++;

				if (tickdiff > 0.0)
				{
					if ((n->pcpu = (n->time - otime) / tickdiff) < 0.0001)
					{
						n->pcpu = 0;
					}
				}

				if ((show_idle || n->pgstate != STATE_IDLE) &&
					(sel->usename[0] == '\0' ||
					 strcmp(n->usename, sel->usename) == 0))
					memcpy(&pgtable[active_procs++], n,
						   sizeof(struct top_proc));
			}
			n->index = (n->index + 1) % 2;
			total_procs++;
		}
		if (pgresult != NULL)
			PQclear(pgresult);
		disconnect_from_db(conninfo);

		si->p_active = active_procs;
		si->p_total = total_procs;
		si->procstates = process_states;
	}

	/* if requested, sort the "active" procs */
	if (compare_index >= 0 && si->p_active)
	{
		qsort(pgtable, si->p_active, sizeof(struct top_proc),
			  proc_compares[compare_index]);
	}

	/* don't even pretend that the return value thing here isn't bogus */
	proc_index = 0;
	return (caddr_t) 0;
}

char *
format_header(char *uname_field)
{
	int			uname_len = strlen(uname_field);

	if (uname_len > 8)
		uname_len = 8;

	memcpy(strchr(fmt_header, 'X'), uname_field, uname_len);

	return fmt_header;
}

char *
format_next_io(caddr_t handle)
{
	static char fmt[MAX_COLS];	/* static area where result is built */
	struct top_proc *p = &pgtable[proc_index++];

	snprintf(fmt, sizeof(fmt),
			"%5d %7.0f %7.0f %7.0f %5s %6s %s",
			p->pid,
			diff_stat(p->iops, p->index) / timediff,
			diff_stat(p->syscr, p->index) / timediff,
			diff_stat(p->syscw, p->index) / timediff,
			format_b(diff_stat(p->read_bytes, p->index) / timediff),
			format_b(diff_stat(p->write_bytes, p->index) / timediff),
			p->name);

	return (fmt);
}

char *
format_next_process(caddr_t handle)
{
	static char fmt[MAX_COLS];	/* static area where result is built */
	struct top_proc *p = &pgtable[proc_index++];

	snprintf(fmt, sizeof(fmt),
			 "%7d %-10.8s %5s %5s %-6s %5s %5s %5.1f %5d %s",
			 p->pid,
			 p->usename,
			 format_k(p->size),
			 format_k(p->rss),
			 backendstatenames[p->pgstate],
			 format_time(p->xtime),
			 format_time(p->qtime),
			 p->pcpu * 100.0,
			 p->locks,
			 p->name);

	/* return the result */
	return (fmt);
}

char *
format_next_replication(caddr_t handle)
{
	static char fmt[MAX_COLS];	/* static area where result is built */
	struct top_proc *p = &pgtable[proc_index++];

	snprintf(fmt, sizeof(fmt),
			 "%5d %-8.8s %-11.11s %15s %-9.9s %-10.10s %-10.10s %-10.10s %-10.10s %-10.10s %5s %5s %5s %5s",
			 p->pid,
			 p->usename,
			 p->application_name,
			 p->client_addr,
			 p->repstate,
			 p->primary,
			 p->sent,
			 p->write,
			 p->flush,
			 p->replay,
			 format_b(p->sent_lag),
			 format_b(p->write_lag),
			 format_b(p->flush_lag),
			 format_b(p->replay_lag));

	/* return the result */
	return (fmt);
}

/* comparison routines for qsort */

/*
 * There are currently four possible comparison routines.  main selects
 * one of these by indexing in to the array proc_compares.
 *
 * Possible keys are defined as macros below.  Currently these keys are
 * defined:  percent cpu, cpu ticks, process state, resident set size,
 * total virtual memory usage.	The process states are ordered as follows
 * (from least to most important):	WAIT, zombie, sleep, stop, start, run.
 * The array declaration below maps a process state index into a number
 * that reflects this ordering.
 */

/* First, the possible comparison keys.  These are defined in such a way
   that they can be merely listed in the source code to define the actual
   desired ordering.
 */

#define ORDERKEY_IOPS   if ((result = diff_stat(p2->iops, p2->index) - \
			                          diff_stat(p1->iops, p1->index)) == 0)
#define ORDERKEY_LAG_FLUSH  if ((result = p2->flush_lag - p1->flush_lag) == 0)
#define ORDERKEY_LAG_REPLAY if ((result = p2->replay_lag - \
                                          p1->replay_lag) == 0)
#define ORDERKEY_LAG_SENT   if ((result = p2->sent_lag - p1->sent_lag) == 0)
#define ORDERKEY_LAG_WRITE  if ((result = p2->write_lag - p1->write_lag) == 0)
#define ORDERKEY_LOCKS   if ((result = p2->locks - p1->locks) == 0)
#define ORDERKEY_MEM     if ((result = p2->size - p1->size) == 0)
#define ORDERKEY_NAME    if ((result = strcmp(p1->name, p2->name)) == 0)
#define ORDERKEY_PCTCPU  if ((result = (int)(p2->pcpu - p1->pcpu)) == 0)
#define ORDERKEY_QTIME   if ((result = p2->qtime - p1->qtime) == 0)
#define ORDERKEY_READS   if ((result = diff_stat(p2->read_bytes, p2->index) - \
			                           diff_stat(p1->read_bytes, p1->index)) == 0)
#define ORDERKEY_RSSIZE  if ((result = p2->rss - p1->rss) == 0)
#define ORDERKEY_STATE   if ((result = p1->pgstate < p2->pgstate))
#define ORDERKEY_SYSCR   if ((result = diff_stat(p2->syscr, p2->index) - \
                                       diff_stat(p1->syscr, p1->index)) == 0)
#define ORDERKEY_SYSCW   if ((result = diff_stat(p2->syscw, p2->index) - \
                                       diff_stat(p1->syscw, p1->index)) == 0)
#define ORDERKEY_WRITES  if ((result = diff_stat(p2->write_bytes, p2->index) - \
			                           diff_stat(p1->write_bytes, p1->index)) == 0)
#define ORDERKEY_XTIME   if ((result = p2->xtime - p1->xtime) == 0)

/* compare_cmd - the comparison function for sorting by command name */

static int
compare_cmd(const void *v1, const void *v2)
{
	struct top_proc *p1 = (struct top_proc *) v1;
	struct top_proc *p2 = (struct top_proc *) v2;
	int			result;

	ORDERKEY_NAME
		ORDERKEY_PCTCPU
		ORDERKEY_STATE
		ORDERKEY_RSSIZE
		ORDERKEY_MEM
		;

	return (result);
}

/* compare_cpu - the comparison function for sorting by cpu percentage */

static int
compare_cpu(const void *v1, const void *v2)
{
	struct top_proc *p1 = (struct top_proc *) v1;
	struct top_proc *p2 = (struct top_proc *) v2;
	int			result;

	ORDERKEY_PCTCPU
		ORDERKEY_STATE
		ORDERKEY_RSSIZE
		ORDERKEY_MEM
		;

	return (result);
}

/* compare_iops - the comparison function for sorting by iops */

static int
compare_iops(const void *v1, const void *v2)
{
	struct top_proc *p1 = (struct top_proc *) v1;
	struct top_proc *p2 = (struct top_proc *) v2;
	int			result;

	ORDERKEY_IOPS
		ORDERKEY_SYSCW
		ORDERKEY_SYSCR
		ORDERKEY_READS
		ORDERKEY_WRITES
		ORDERKEY_NAME
		;

	return (result);
}

static int
compare_lag_flush(const void *v1, const void *v2)
{
	struct top_proc *p1 = (struct top_proc *) v1;
	struct top_proc *p2 = (struct top_proc *) v2;
	int			result;

	ORDERKEY_LAG_FLUSH
		ORDERKEY_PCTCPU
		ORDERKEY_STATE
		ORDERKEY_MEM
		ORDERKEY_RSSIZE
		;

	return (result);
}

static int
compare_lag_replay(const void *v1, const void *v2)
{
	struct top_proc *p1 = (struct top_proc *) v1;
	struct top_proc *p2 = (struct top_proc *) v2;
	int			result;

	ORDERKEY_LAG_REPLAY
		ORDERKEY_PCTCPU
		ORDERKEY_STATE
		ORDERKEY_MEM
		ORDERKEY_RSSIZE
		;

	return (result);
}

static int
compare_lag_sent(const void *v1, const void *v2)
{
	struct top_proc *p1 = (struct top_proc *) v1;
	struct top_proc *p2 = (struct top_proc *) v2;
	int			result;

	ORDERKEY_LAG_SENT
		ORDERKEY_PCTCPU
		ORDERKEY_STATE
		ORDERKEY_MEM
		ORDERKEY_RSSIZE
		;

	return (result);
}

static int
compare_lag_write(const void *v1, const void *v2)
{
	struct top_proc *p1 = (struct top_proc *) v1;
	struct top_proc *p2 = (struct top_proc *) v2;
	int			result;

	ORDERKEY_LAG_WRITE
		ORDERKEY_PCTCPU
		ORDERKEY_STATE
		ORDERKEY_MEM
		ORDERKEY_RSSIZE
		;

	return (result);
}

/*
 * compare_locks - the comparison function for sorting by total locks ancquired
 */

static int
compare_locks(const void *v1, const void *v2)
{
	struct top_proc *p1 = (struct top_proc *) v1;
	struct top_proc *p2 = (struct top_proc *) v2;
	int			result;

	ORDERKEY_LOCKS
		ORDERKEY_QTIME
		ORDERKEY_PCTCPU
		ORDERKEY_STATE
		ORDERKEY_MEM
		ORDERKEY_RSSIZE
		;

	return (result);
}

/* compare_qtime - the comparison function for sorting by total cpu qtime */

static int
compare_qtime(const void *v1, const void *v2)
{
	struct top_proc *p1 = (struct top_proc *) v1;
	struct top_proc *p2 = (struct top_proc *) v2;
	int			result;

	ORDERKEY_QTIME
		ORDERKEY_PCTCPU
		ORDERKEY_STATE
		ORDERKEY_MEM
		ORDERKEY_RSSIZE
		;

	return (result);
}

static int
compare_reads(const void *v1, const void *v2)
{
	struct top_proc *p1 = (struct top_proc *) v1;
	struct top_proc *p2 = (struct top_proc *) v2;
	int			result;

	ORDERKEY_READS
		ORDERKEY_SYSCR
		ORDERKEY_IOPS
		ORDERKEY_SYSCW
		ORDERKEY_WRITES
		ORDERKEY_NAME
		;

	return (result);
}

/* compare_res - the comparison function for sorting by resident set size */

static int
compare_res(const void *v1, const void *v2)
{
	struct top_proc *p1 = (struct top_proc *) v1;
	struct top_proc *p2 = (struct top_proc *) v2;
	int			result;

	ORDERKEY_RSSIZE
		ORDERKEY_MEM
		ORDERKEY_PCTCPU
		ORDERKEY_STATE
		;

	return (result);
}

/* compare_size - the comparison function for sorting by total memory usage */

static int
compare_size(const void *v1, const void *v2)
{
	struct top_proc *p1 = (struct top_proc *) v1;
	struct top_proc *p2 = (struct top_proc *) v2;
	int			result;

	ORDERKEY_MEM
		ORDERKEY_RSSIZE
		ORDERKEY_PCTCPU
		ORDERKEY_STATE
		;

	return (result);
}

static int
compare_syscr(const void *v1, const void *v2)
{
	struct top_proc *p1 = (struct top_proc *) v1;
	struct top_proc *p2 = (struct top_proc *) v2;
	int			result;

	ORDERKEY_SYSCR
		ORDERKEY_IOPS
		ORDERKEY_SYSCW
		ORDERKEY_READS
		ORDERKEY_WRITES
		ORDERKEY_NAME
		;

	return (result);
}

static int
compare_syscw(const void *v1, const void *v2)
{
	struct top_proc *p1 = (struct top_proc *) v1;
	struct top_proc *p2 = (struct top_proc *) v2;
	int			result;

	ORDERKEY_SYSCW
		ORDERKEY_IOPS
		ORDERKEY_SYSCR
		ORDERKEY_READS
		ORDERKEY_WRITES
		ORDERKEY_NAME
		;

	return (result);
}

/* compare_xtime - the comparison function for sorting by total cpu xtime */

static int
compare_xtime(const void *v1, const void *v2)
{
	struct top_proc *p1 = (struct top_proc *) v1;
	struct top_proc *p2 = (struct top_proc *) v2;
	int			result;

	ORDERKEY_XTIME
		ORDERKEY_PCTCPU
		ORDERKEY_STATE
		ORDERKEY_MEM
		ORDERKEY_RSSIZE
		;

	return (result);
}

static int
compare_writes(const void *v1, const void *v2)
{
	struct top_proc *p1 = (struct top_proc *) v1;
	struct top_proc *p2 = (struct top_proc *) v2;
	int			result;

	ORDERKEY_WRITES
		ORDERKEY_IOPS
		ORDERKEY_SYSCR
		ORDERKEY_SYSCW
		ORDERKEY_READS
		ORDERKEY_NAME
		;

	return (result);
}
