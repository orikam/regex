/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2020 Mellanox Technologies, Ltd
 *
 * This file contain the application main file
 * This application provides a way to test the RegEx class.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>

#include <rte_eal.h>
#include <rte_common.h>
#include <rte_malloc.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_net.h>
#include <rte_flow.h>
#include <rte_cycles.h>
#include <rte_regexdev.h>


#define HELP_VAL 0
#define RULES_VAL 1
#define DATA_VAL 2
#define JOB_VAL 3
#define MAX_FILE_NAME 255
static char rules_file[MAX_FILE_NAME];
static char data_file[MAX_FILE_NAME];
static int jobs;
static struct rte_mempool *mbuf_mp;
static uint8_t nb_max_matches;

static void
args_parse(int argc, char **argv)
{
	char **argvopt;
	int opt;
	int opt_idx;
	static struct option lgopts[] = {
		{ "help",  0, 0, HELP_VAL },
		{ "rules",  1, 0, RULES_VAL },
		/* Rules database file to load. */
		{ "data",  1, 0, DATA_VAL },
		/* Data file to load. */
		{ "job",  1, 0, JOB_VAL },
		/* Number of jobs to create. */
	};

	argvopt = argv;

	while ((opt = getopt_long(argc, argvopt, "",
				lgopts, &opt_idx)) != EOF) {
		switch (opt) {
		case RULES_VAL:
			strncpy(rules_file, optarg, MAX_FILE_NAME);
			break;
		case DATA_VAL:
			strncpy(data_file, optarg, MAX_FILE_NAME);
			break;
		case JOB_VAL:
			jobs = atoi(optarg);
			break;
		default:
			fprintf(stderr, "Invalid option: %s\n", argv[optind]);
			rte_exit(EXIT_FAILURE, "Invalid option\n");
			break;
		}
	}
}

static long
read_file(char *file, char **buf)
{
	FILE *fp;
	long buf_len = 0;
	size_t read_len;

	fp = fopen(file, "r");
	if (!fp)
		return -EIO;
	if (fseek(fp, 0L, SEEK_END) == 0) {
		buf_len = ftell(fp);
		if (buf_len == -1)
			goto error;
		*buf = rte_malloc(NULL, sizeof(char) * (buf_len + 1), 4096);
		if (fseek(fp, 0L, SEEK_SET) != 0)
			goto error;
		read_len = fread(*buf, sizeof(char), buf_len, fp);
		if (read_len != (unsigned long)buf_len)
			goto error;
	}
	fclose(fp);
	return buf_len;
error:
	if (fp)
		fclose(fp);
	if (*buf)
		rte_free(*buf);
	return -EIO;
}
#define TOTAL_MBUF_NUM 1024
#define MBUF_CACHE_SIZE 256
#define MBUF_SIZE 256


static void
init_port(void)
{
	int ret;
	uint16_t id;
	uint16_t num_devs;
	char *rules = NULL;
	long rules_len;
	struct rte_regexdev_info info;
	struct rte_regexdev_config dev_conf = {
		.nb_queue_pairs = 1,
		.nb_groups = 1,
	};
	struct rte_regexdev_qp_conf qp_conf = {
		.nb_desc = 1024,
	};

	num_devs = rte_regexdev_count();
	if (num_devs == 0)
		rte_exit(EXIT_FAILURE, "Error: no devices detected\n");

	mbuf_mp = rte_pktmbuf_pool_create("mbuf_pool",
					TOTAL_MBUF_NUM, MBUF_CACHE_SIZE,
					0, MBUF_SIZE,
					rte_socket_id());
	if (mbuf_mp == NULL)
		rte_exit(EXIT_FAILURE, "Error: can't init mbuf pool\n");
	rules_len = read_file(rules_file, &rules);
	if (rules_len < 0)
		rte_exit(EXIT_FAILURE, "Error: can't read rule file\n");
	for (id = 0; id < num_devs; id++) {
		ret = rte_regexdev_info_get(id, &info);
		if (ret != 0)
			rte_exit(EXIT_FAILURE,
				"Error during getting device"
				" (port %u) info: %s\n", id, strerror(-ret));
		printf(":: initializing dev: %d\n", id);
		nb_max_matches = info.max_matches;
		if (info.regexdev_capa & RTE_REGEXDEV_SUPP_MATCH_AS_END_F)
			dev_conf.dev_cfg_flags |= RTE_REGEXDEV_CFG_MATCH_AS_END_F;
		dev_conf.nb_max_matches = info.max_matches;
		dev_conf.nb_rules_per_group = info.max_rules_per_group;
		dev_conf.rule_db_len = rules_len;
		dev_conf.rule_db = rules;
		ret = rte_regexdev_configure(id, &dev_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE,
				":: cannot configure device: err=%d, dev=%u\n",
				ret, id);
		ret = rte_regexdev_queue_pair_setup(id, 0, &qp_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE,
				":: cannot configure qp %d: err=%d, dev=%u\n",
				0, ret, id);
		ret = rte_regexdev_start(id);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "can't start dev %d\n", id);
		printf(":: initializing device: %d done\n", id);
	}
}

static void
extbuf_free_cb(void *addr __rte_unused, void *fcb_opaque __rte_unused)
{

}

#define START_BURST_SIZE 32

static void
main_loop(void)
{
	char *buf;
	long buf_len;
	long job_len;
	uint32_t i;
	struct rte_regex_ops *ops[TOTAL_MBUF_NUM];
	struct rte_mbuf *release_arr[TOTAL_MBUF_NUM];
	uint16_t dev_id = 0;
	uint16_t qp_id = 0;
	uint32_t job_ind = 0;
	uint8_t nb_matches;
	struct rte_regexdev_match *match;
	long pos;
	uint32_t burst_size = RTE_MIN(START_BURST_SIZE, jobs);
	unsigned long e_ind = 0;
	unsigned long d_ind = 0;
	struct rte_mbuf_ext_shared_info shinfo;
	uint32_t dequeue;
	uint32_t enqueue;
	uint32_t ind;

	shinfo.free_cb = extbuf_free_cb;
	for (i = 0; i < TOTAL_MBUF_NUM; i++) {
		ops[i] = rte_malloc(NULL, sizeof(*ops[0]) + nb_max_matches *
				    sizeof(struct rte_regexdev_match), 0);
		if (!ops[i])
			goto main_loop_err;
		ops[i]->mbuf = rte_pktmbuf_alloc(mbuf_mp);
		if (!ops[i]->mbuf)
			goto main_loop_err;
	}
	ops[0]->group_id0 = 0;
	buf_len = read_file(data_file, &buf);
	if (buf_len <= 0)
		return;
	job_len = buf_len / jobs;
	pos = 0;
	for (i = 0; (pos < buf_len) && (i < burst_size); i++) {
		job_len = RTE_MIN(job_len, buf_len - pos);
		rte_pktmbuf_attach_extbuf(ops[i]->mbuf, &buf[pos], 0, job_len,
					  &shinfo);
		ops[i]->mbuf->data_len = job_len;
		ops[i]->mbuf->pkt_len = job_len;
		ops[i]->user_id = i;
		release_arr[i] = ops[i]->mbuf;
		pos += job_len;
	}
	while (pos < buf_len) {
		enqueue = rte_regexdev_enqueue_burst(dev_id, qp_id,
						     &ops[e_ind &
						     TOTAL_MBUF_NUM],
						     burst_size);
		e_ind += enqueue;
		burst_size -= enqueue;
		while ((dequeue =
		        rte_regexdev_dequeue_burst(dev_id, qp_id, ops, 1)) ==
		       0);
		dequeue += d_ind;
		for (; i < dequeue; d_ind++) {
			nb_matches = ops[d_ind & TOTAL_MBUF_NUM]->nb_matches;
			printf("Job id %d number of matches = %d\n", job_ind,
			       nb_matches);
			match = ops[d_ind & TOTAL_MBUF_NUM]->matches;
			for (i = 0; i < nb_matches; i++) {
				printf("match %d, rule = %d, start = %d,"
				       "len = %d\n", i, match->rule_id,
				       match->start_offset, match->len);
				match++;
			}
			rte_pktmbuf_detach_extbuf
				(release_arr[ops[d_ind]->user_id]);
		}
		for (;burst_size < START_BURST_SIZE && pos < buf_len;
		     burst_size++) {
			job_len = RTE_MIN(job_len, buf_len - pos);
			rte_pktmbuf_attach_extbuf
				(ops[e_ind & TOTAL_MBUF_NUM]->mbuf, &buf[pos],
				 0, job_len, &shinfo);
			ind = e_ind & TOTAL_MBUF_NUM;
			ops[ind]->mbuf->data_len = job_len;
			ops[ind]->mbuf->pkt_len = job_len;
			ops[ind]->user_id = i;
			release_arr[ind] = ops[ind]->mbuf;
			pos += job_len;
		}
	}
main_loop_err:
	for (i = 0; i < TOTAL_MBUF_NUM; i++) {
		if (ops[i]) {
			if (ops[i]->mbuf)
				rte_pktmbuf_free(ops[i]->mbuf);
			rte_free(ops[i]);
		}
	}
}

int
main(int argc, char **argv)
{
	int ret;

	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "EAL init failed\n");
	argc -= ret;
	argv += ret;
	if (argc > 1)
		args_parse(argc, argv);
	init_port();
	main_loop();
	return 0;
}
