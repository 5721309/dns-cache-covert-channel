#include <netdb.h>
#include <string.h>
#include <unistd.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>
#include <math.h>


#define DOMAIN_NAME_MAX_LENGTH 20
#define DOMAIN_NAME_MIN_LENGTH 7
#define REQUESTS_PER_BLOCK 8      
#define BLOCK_DELAY 0  
#define BIT_DELAY 0    
#define INBUFSIZE 4096
#define N_STATS 50


enum mode {
	MODE_UNKNOWN,
	MODE_STATISTICS,
	MODE_SENDER,
	MODE_RECEIVER,
	N_MODE
} mode;
static const uint32_t inbufsize = INBUFSIZE;
static int i_stats;
static unsigned long nextrand = 1;
static bool verbose = false;
static long sum_tau = 0, edge_tau = 10;  // cached (ms)
static long sum_t = 0, edge_t = 30;    // uncached (ms)
static int n_stats = N_STATS;
static long * arrtau = NULL, * arrt = NULL;
static long rpb = REQUESTS_PER_BLOCK;  
static long block_delay_ms = BLOCK_DELAY,  // for each block (ms)
            bit_delay_ms = BIT_DELAY;      // for each request (ms)


static inline struct timespec
diff_timespec(const struct timespec time_end,
              const struct timespec time_start) {
	struct timespec diff = {
		.tv_sec = time_end.tv_sec - time_start.tv_sec,
		.tv_nsec = time_end.tv_nsec - time_start.tv_nsec
	};
	if (diff.tv_nsec < 0) {
		diff.tv_nsec += 1000000000;
		diff.tv_sec--;
	}
	return diff;
}


void
parse_args(int argc, char ** argv)
{
	int opt;

	while (-1 != (opt = getopt(argc, argv, "sre:b:t:T:n:vh"))) {
		switch (opt) {
		case 's': if (mode != MODE_UNKNOWN) goto usage;
			mode = MODE_SENDER; break;
		case 'r': if (mode != MODE_UNKNOWN) goto usage;
			mode = MODE_RECEIVER; break;
		case 'e':
			  errno = 0;
			  edge_tau = strtol(optarg, NULL, 10);
			  if (0 != errno || edge_tau < 1)
				  goto usage;
			  break;
		case 'b':
			  errno = 0;
			  rpb = strtol(optarg, NULL, 10);
			  if (0 != errno || rpb < 1)
				  goto usage;
			  break;
		case 't':
			  errno = 0;
			  bit_delay_ms = strtol(optarg, NULL, 10);
			  if (0 != errno || bit_delay_ms < 0)
				  goto usage;
			  break;
		case 'T':
			  errno = 0;
			  block_delay_ms = strtol(optarg, NULL, 10);
			  if (0 != errno || block_delay_ms < 0)
				  goto usage;
			  break;
		case 'n':
			  errno = 0;
			  n_stats = strtol(optarg, NULL, 10);
			  if (0 != errno || n_stats < 1)
				  goto usage;
			  break;
		case 'v':
			verbose = true; break;
		default:
			goto usage;
		}
	}
	if (optind >= argc) {
		nextrand = time(NULL) / 86400;  // day
	} else {
		errno = 0;
		nextrand = strtoul(argv[optind], NULL, 10);
		if (0 != errno)
			goto usage;
	}
	if (mode == MODE_UNKNOWN) {
		mode = MODE_STATISTICS;
	}
	return;
usage:
	fprintf(stderr, "Usage: %s [OPTIONS] [seed]\n"
		"\targ seed: int PRG(int) will generate domain names.\n"
		"\t-h            print this help message and exit.\n"
		"\t-r            run in receiver mode.\n"
		"\t-s            run in sender mode.\n"
		"\t-e: int (10)  edge time to interpret delay of dns response "
	  "(ms)\n\t              if (delay > int) then interpret as uncached.\n"
		"\t-b: int (10)  how many requests per block\n"
		"\t-T: int (0)   period of sending blocks of requests (ms)\n"
		"\t-t: int (0)   period of sending each request (ms)\n"
		"\t-n: int (50)  tells how much timing stats to collect\n"
		"\t              it will send (2 * int) dns requests\n"
		"\t              (int) for cached and (int) for uncached\n"
		"[Statistics]\n"
		"If -r or -s not specified - running in statistics mode.\n"
		"Output are two values: right limit of cached response time,\n"
		"left limit of uncached response time (3-sigma criteria).\n"
		"If second value is less than the first one, then collected\n"
		"timing statistics don't seem to be normally distributed.\n"
		"Maybe try different seed.\n",
		argv[0]);
	exit(EXIT_FAILURE);
}

static inline int
myrand()
{
	nextrand = nextrand * 1103515245UL + 12345UL;
	return (nextrand/65536UL) & 0x7FFF;
}

static void
gen_random_string(int len, char* str)
{
	int tmp, i;
	
	for (i = 0; i < len; i += 3) {
		tmp = myrand();  // returns 15 bits
		str[i+0] = 97 + ((tmp >>  0) & 0x1F) % 26;
		str[i+1] = 97 + ((tmp >>  5) & 0x1F) % 26;
		str[i+2] = 97 + ((tmp >> 10) & 0x1F) % 26;
	}
	tmp = myrand();
	for (i = 0; i < len % 3; ++i)
		str[len - 3 + i] = 97 + ((tmp >> (5 * i)) & 0x1F) % 26;	
}


static char*
getname()
{
	static char line[DOMAIN_NAME_MAX_LENGTH];
	static size_t n = 0;

	n = nextrand % (1 + DOMAIN_NAME_MAX_LENGTH - DOMAIN_NAME_MIN_LENGTH);
	n += DOMAIN_NAME_MIN_LENGTH;
	gen_random_string(n - 4, line);
	gen_random_string(3, line + n - 3);
	line[n - 4] = '.';
	line[n] = '\0';
	return line;
}


static long
get_dns_response_time_ms(char* dname)
{
	struct timespec time_start, time_end, time_diff;
	struct addrinfo* ainfo;
	int err;
	long ms = -1;

	timespec_get(&time_start, TIME_UTC);
	err = getaddrinfo(dname, NULL, NULL, &ainfo);
	timespec_get(&time_end, TIME_UTC);
	switch (err) {
		case 0: case EAI_NONAME:
			time_diff = diff_timespec(time_end, time_start);
			ms = time_diff.tv_sec * 1000
			   + time_diff.tv_nsec / 1000000;
			if (verbose)
				fprintf(stderr, "\033[91m%s: %ld ms\033[0m\n", dname, ms);
			if (0 == err)
				freeaddrinfo(ainfo);
			return ms;
		default:
			perror(gai_strerror(err));
			return -1;
	}
}


static void
account_stats(char* dname)
{
	long ms;

	ms = get_dns_response_time_ms(dname);  // uncached
	if (ms >= 0) {
		arrt[i_stats] = ms;
		sum_t += ms;
	} else {
		fprintf(stderr, "domain name %s can't be resolved", dname);
		exit(EXIT_FAILURE);
	}
	ms = get_dns_response_time_ms(dname);  // cached
	if (ms >= 0) {
		arrtau[i_stats] = ms;
		sum_tau += ms;
	} else {
		fprintf(stderr, "domain name %s can't be resolved", dname);
		exit(EXIT_FAILURE);
	}
	i_stats += 1;
}


void
do_mode_statistics()
{
	long i;
	double tmp, m_t, m_tau;
	double sd_tau = 0, sd_t = 0;

	i_stats = 0;
	for (i = 1; i <= n_stats; ++i) {
		account_stats(getname());
		if (0 == i % rpb) {
			nanosleep(&(struct timespec){
				.tv_sec = block_delay_ms / 1000,
				.tv_nsec = (block_delay_ms % 1000) * 1000000
			}, NULL);
		} else {
			nanosleep(&(struct timespec){
				.tv_sec = bit_delay_ms / 1000,
				.tv_nsec = (bit_delay_ms % 1000) * 1000000
			}, NULL);
		}
	}

	m_tau = (double)sum_tau / n_stats;  // cached
	for (i = 0; i < n_stats; ++i) {
		tmp = arrtau[i] - m_tau;
		sd_tau += tmp * tmp;
	}
	sd_tau = sqrt(sd_tau / (n_stats - 1));
	edge_tau = m_tau + ceil(3 * sd_tau);  // 3-sigma criteria

	m_t = (double)sum_t / n_stats;      // uncached
	for (i = 0; i < n_stats; ++i) {
		tmp = arrt[i] - m_t;
		sd_t += tmp * tmp;
	}
	sd_t = sqrt(sd_t / (n_stats - 1));
	edge_t = m_t - ceil(3 * sd_t);  // 3-sigma criteria

	// TODO посчитать вероятность ошибки -- пересечения распределений

	printf("%ld %ld\n", edge_tau, edge_t);
}


static inline long
send_bit(bool bit)
{
	static int sent = 0;
	static long ms;
	char* dname = getname();  // always must be called
	
	ms = 0;
	if (bit) {
		ms = get_dns_response_time_ms(dname);
		sent += 1;
		if (sent % rpb == 0) {
			nanosleep(&(struct timespec){
				.tv_sec = block_delay_ms / 1000,
				.tv_nsec = (block_delay_ms % 1000) * 1000000
			}, NULL);
		}
	} 
	nanosleep(&(struct timespec){
		.tv_sec = bit_delay_ms / 1000,
		.tv_nsec = (bit_delay_ms % 1000) * 1000000
	}, NULL);
	return ms;
}

static inline bool
receive_bit()
{
	static int n_ones = 0;
	static bool bit;
	char* dname = getname();  // always must be called

	bit = get_dns_response_time_ms(dname) <= edge_tau;
	if (bit) {
		n_ones += 1;
		if (n_ones % rpb == 0) {
			nanosleep(&(struct timespec){
				.tv_sec = block_delay_ms / 1000,
				.tv_nsec = (block_delay_ms % 1000) * 1000000
			}, NULL);
		}
	}
	nanosleep(&(struct timespec){
		.tv_sec = bit_delay_ms / 1000,
		.tv_nsec = (bit_delay_ms % 1000) * 1000000
	}, NULL);
	return bit;
}

static void
send_byte(uint8_t byte)
{
	send_bit((byte >> 0) & 1);
	send_bit((byte >> 1) & 1);
	send_bit((byte >> 2) & 1);
	send_bit((byte >> 3) & 1);
	send_bit((byte >> 4) & 1);
	send_bit((byte >> 5) & 1);
	send_bit((byte >> 6) & 1);
	send_bit((byte >> 7) & 1);
}

static uint8_t
receive_byte()
{
	uint8_t byte = 0;
	byte |= receive_bit() << 0;
	byte |= receive_bit() << 1;
	byte |= receive_bit() << 2;
	byte |= receive_bit() << 3;
	byte |= receive_bit() << 4;
	byte |= receive_bit() << 5;
	byte |= receive_bit() << 6;
	byte |= receive_bit() << 7;
	return byte;
}


static void
do_mode_sender()
{
	uint8_t buf[INBUFSIZE];
	int i;
	uint32_t n_bytes;

	while (0 < (n_bytes = read(STDIN_FILENO, buf, inbufsize))) {
		// first, send n_bytes as a length of a PDU
		send_byte(n_bytes >> 0);
		send_byte(n_bytes >> 8);
		send_byte(n_bytes >> 16);
		send_byte(n_bytes >> 24);
		if (verbose) {
			fprintf(stderr,
			  "\033[93msending chunk of length %u\033[0m\n",
			  n_bytes);
		}
		// then, send message itself
		for (i = 0; i < n_bytes; ++i) {
			send_byte(buf[i]);
		}
	}
}

static void
do_mode_receiver()
{
	uint8_t buf[INBUFSIZE];
	int i;
	uint32_t n_bytes;

	do {
		n_bytes = 0;
		// first, receive n_bytes - length of a PDU
		n_bytes |= (uint32_t)receive_byte() << 0;
		n_bytes |= (uint32_t)receive_byte() << 8;
		n_bytes |= (uint32_t)receive_byte() << 16;
		n_bytes |= (uint32_t)receive_byte() << 24;
		if (verbose) {
			fprintf(stderr,
			  "\033[31mreceiving chunk of length %u\033[0m\n",
			  n_bytes);
		}
		// then, receive the message itself
		for (i = 0; i < n_bytes; ++i) {
			buf[i] = receive_byte();
		}
		if (n_bytes > 0)
			write(STDOUT_FILENO, buf, n_bytes);
	} while (0 != n_bytes);
}


int
main(int argc, char** argv)
{
	parse_args(argc, argv);
	arrtau = calloc(n_stats, 2 * sizeof(long));
	arrt = arrtau + n_stats;
	switch (mode) {
	case MODE_STATISTICS: do_mode_statistics(); break;
	case MODE_SENDER:     do_mode_sender();     break;
	case MODE_RECEIVER:   do_mode_receiver();   break;
	default: __builtin_unreachable();
	}

	return EXIT_SUCCESS;
}
