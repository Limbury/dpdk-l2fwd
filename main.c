#include <signal.h>

#include "main.h"
uint16_t
add_timestamps(uint8_t port __rte_unused, uint16_t qidx __rte_unused,
		struct rte_mbuf **pkts, uint16_t nb_pkts,
		uint16_t max_pkts __rte_unused, void *_ __rte_unused)
{
	unsigned i;
	uint64_t now = rte_rdtsc();

	for (i = 0; i < nb_pkts; i++)
		pkts[i]->udata64 = now;
	return nb_pkts;
}

uint16_t
calc_latency(uint8_t port __rte_unused, uint16_t qidx __rte_unused,
		struct rte_mbuf **pkts, uint16_t nb_pkts, void *_ __rte_unused)
{
	uint64_t cycles = 0;
	uint64_t now = rte_rdtsc();
	unsigned i;

	for (i = 0; i < nb_pkts; i++)
		cycles += now - pkts[i]->udata64;
	latency_numbers.total_cycles += cycles;
	latency_numbers.total_pkts += nb_pkts;

	if (latency_numbers.total_pkts > (100 * 1000 * 1000ULL)) {
		printf("Latency = %"PRIu64" cycles\n",
		latency_numbers.total_cycles / latency_numbers.total_pkts);
		latency_numbers.total_cycles = latency_numbers.total_pkts = 0;
	}
	return nb_pkts;
}

volatile bool force_quit;

static void
signal_handler(int signum) {
    switch (signum) {
    case SIGTERM:
    case SIGINT:
        RTE_LOG(
            INFO, SWITCH,
            "%s: Receive %d signal, prepare to exit...\n",
            __func__, signum
        );
        force_quit = true;
        break;
    }
}

static void
print_stats(void)
{	
	uint16_t i;
	struct rte_eth_stats eth_stats;
	printf("Latency = %"PRIu64" cycles\n",
		latency_numbers.total_cycles / latency_numbers.total_pkts);
	for (i = 0; i < app.n_ports; i++) {
		rte_eth_stats_get(i, &eth_stats);
		printf("\nPort %u stats:\n", i);
		printf(" - Pkts in:   %"PRIu64"\n", eth_stats.ipackets);
		printf(" - Pkts out:  %"PRIu64"\n", eth_stats.opackets);
		printf(" - In Errs:   %"PRIu64"\n", eth_stats.ierrors);
		printf(" - Out Errs:  %"PRIu64"\n", eth_stats.oerrors);
		printf(" - Mbuf Errs: %"PRIu64"\n", eth_stats.rx_nombuf);
	}
}

static void
app_quit(void) {
    uint8_t i;
    /* close ports */
    for (i = 0; i < app.n_ports; i++) {
        uint8_t port = (uint8_t) app.ports[i];
        RTE_LOG(
            INFO, SWITCH,
            "%s: Closing NIC port %u ...\n",
            __func__, port
        );
        rte_eth_dev_stop(port);
        rte_eth_dev_close(port);
    }
    /* free resources */
    /*if (app_cfg.cfg != NULL) {
        cfg_free(app_cfg.cfg);
    }
    if (app_cfg.bm_policy != NULL) {
        free(app_cfg.bm_policy);
    }
    if (app_cfg.qlen_fname != NULL) {
        free(app_cfg.qlen_fname);
    }*/
    /* close files */
    if (app.log_qlen) {
        fclose(app.qlen_file);
    }
    print_stats();
    printf("App quit. Bye...\n");
}

int
main(int argc, char **argv) {
    uint32_t lcore;
    int ret;
    int fd;
    /* Init EAL */
	//fd=fopen("./log_file", "a+");
    //rte_openlog_stream(fd);
    ret = rte_eal_init(argc, argv);
    if (ret < 0)
        return -1;
    argc -= ret;
    argv += ret;

    force_quit = false;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Parse application arguments (after the EAL ones) */
    ret = app_parse_args(argc, argv);
    
    if (ret < 0) {
        app_print_usage();
        return -1;
    }

    /* Init */
    app_init();

    app.start_cycle = rte_get_tsc_cycles();
//	printf("the size of ring is %u",app.ring_rx_size);
    /* Launch per-lcore init on every lcore */
    rte_eal_mp_remote_launch(app_lcore_main_loop, NULL, CALL_MASTER);
    RTE_LCORE_FOREACH_SLAVE(lcore) {
        if (rte_eal_wait_lcore(lcore) < 0) {
            return -1;
        }
    }

    app_quit();
    fflush(stdout);

    return 0;
}

int
app_lcore_main_loop(__attribute__((unused)) void *arg) {
    unsigned lcore, i;

    lcore = rte_lcore_id();

    if (lcore == app.core_rx) {
        app_main_loop_rx();
        return 0;
    }

    if (lcore == app.core_worker) {
        app_main_loop_forwarding();
        return 0;
    }

    if (app.n_lcores >= 2+app.n_ports) {
        for (i = 0; i < app.n_ports; i++) {
            if (lcore == app.core_tx[i]) {
                app_main_loop_tx_each_port(i);
                return 0;
            }
        }
    } else {
        if (lcore == app.core_tx[0]) {
            app_main_loop_tx();
            return 0;
        }
    }

    return 0;
}
