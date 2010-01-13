/* -*- C -*-
 *
 * $HEADER$
 *
 * A program that generates output to be consumed
 * by a pnp listener
 */
#include "constants.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#ifdef HAVE_SIGNAL_H
#include <signal.h>
#endif  /*  HAVE_SIGNAL_H */
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif

#include "opal/dss/dss.h"
#include "opal/event/event.h"
#include "opal/util/output.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/util/name_fns.h"
#include "orte/runtime/orte_globals.h"
#include "orte/runtime/orte_wait.h"

#include "mca/pnp/pnp.h"
#include "runtime/runtime.h"

#define ORCM_TEST_CLIENT_SERVER_TAG     12345

static struct opal_event term_handler;
static struct opal_event int_handler;
static void abort_exit_callback(int fd, short flags, void *arg);
static void send_data(int fd, short flags, void *arg);
static void recv_input(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       opal_buffer_t *buf,
                       void *cbdata);

static int32_t flag=0;
static int msg_num;

int main(int argc, char* argv[])
{
    int rc;
    struct timespec tp;
    int delay;

    /* init the ORCM library - this includes registering
     * a multicast recv so we hear announcements and
     * their responses from other apps
     */
    if (ORCM_SUCCESS != (rc = orcm_init(OPENRCM_APP))) {
        fprintf(stderr, "Failed to init: error %d\n", rc);
        exit(1);
    }
    
    /** setup callbacks for abort signals - from this point
     * forward, we need to abort in a manner that allows us
     * to cleanup
     */
    opal_signal_set(&term_handler, SIGTERM,
                    abort_exit_callback, &term_handler);
    opal_signal_add(&term_handler, NULL);
    opal_signal_set(&int_handler, SIGINT,
                    abort_exit_callback, &int_handler);
    opal_signal_add(&int_handler, NULL);
    
    /* announce our existence */
    if (ORCM_SUCCESS != (rc = orcm_pnp.announce("CLIENT", "1.0", "alpha"))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* for this application, register an input to hear direct responses */
    if (ORCM_SUCCESS != (rc = orcm_pnp.register_input_buffer("SERVER", "1.0", "alpha",
                                                             ORCM_TEST_CLIENT_SERVER_TAG, recv_input))) {
        ORTE_ERROR_LOG(rc);
        goto cleanup;
    }
    
    /* compute a wait time */
    delay = 200*(ORTE_PROC_MY_NAME->vpid + 1);
    opal_output(0, "sending data every %d microseconds", delay);
    
    /* init the msg number */
    msg_num = 0;
    
    /* wake up every delay microseconds and send something */
    tp.tv_sec = delay/200;
    tp.tv_nsec = 0; /*1000*delay;*/
    while (1) {
        nanosleep(&tp, NULL);
        send_data(0, 0, NULL);
    }
    
    /* just sit here */
    opal_event_dispatch();

cleanup:
    orcm_finalize();
    return rc;
}

static void cbfunc(int status, orte_process_name_t *name, orcm_pnp_tag_t tag,
                   struct iovec *msg, int count, void *cbdata)
{
    opal_output(0, "%s send complete", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
}

static void send_data(int fd, short flags, void *arg)
{
    int32_t *ptr;
    int rc;
    int j, n;
    float randval;
    struct iovec *msg;
    int count;

    /* prep the message */
    if (0 == ORTE_PROC_MY_NAME->vpid) {
        count = 1;
    } else {
        count = 2;
    }
    msg = (struct iovec*)malloc(count * sizeof(struct iovec));
    for (j=0; j < count; j++) {
        msg[j].iov_len = 5 * sizeof(int32_t);
        msg[j].iov_base = (void*)malloc(msg[j].iov_len);
        ptr = (int32_t*)msg[j].iov_base;
        for (n=0; n < 5; n++) {
            *ptr = msg_num;
            ptr++;
        }
    }
    
    /* output the values */
    opal_output(0, "%s sending data for msg number %d", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), msg_num);
    if (ORCM_SUCCESS != (rc = orcm_pnp.output_nb(NULL, ORCM_PNP_TAG_OUTPUT, msg, count, cbfunc, NULL))) {
        ORTE_ERROR_LOG(rc);
    }

    /* increment the msg number */
    msg_num++;
}

static void abort_exit_callback(int fd, short ign, void *arg)
{
    int j;
    orte_job_t *jdata;
    opal_list_item_t *item;
    int ret;
    
    /* Remove the TERM and INT signal handlers */
    opal_signal_del(&term_handler);
    opal_signal_del(&int_handler);
    
    orcm_finalize();
    exit(1);
}

static void recv_input(int status,
                       orte_process_name_t *sender,
                       orcm_pnp_tag_t tag,
                       opal_buffer_t *buf,
                       void *cbdata)
{
    opal_output(0, "%s recvd message from server %s on tag %d",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                ORTE_NAME_PRINT(sender), (int)tag);
    
}

