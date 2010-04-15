/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "openrcm_config_private.h"
#include "include/constants.h"

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#include <errno.h>

#include "opal/dss/dss.h"
#include "opal/class/opal_list.h"
#include "opal/class/opal_pointer_array.h"
#include "opal/util/output.h"
#include "opal/threads/threads.h"
#include "opal/mca/sysinfo/sysinfo.h"

#include "orte/mca/rmcast/rmcast.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/rml/rml.h"
#include "orte/runtime/orte_globals.h"

#include "mca/leader/leader.h"
#include "runtime/runtime.h"

#include "mca/pnp/pnp.h"
#include "mca/pnp/base/private.h"
#include "mca/pnp/default/pnp_default.h"

/* API functions */

static int default_init(void);
static int announce(char *app, char *version, char *release,
                    orcm_pnp_announce_fn_t cbfunc);
static orcm_pnp_channel_t open_channel(char *app, char *version, char *release);
static int register_input(char *app,
                          char *version,
                          char *release,
                          orcm_pnp_channel_t channel,
                          orcm_pnp_tag_t tag,
                          orcm_pnp_callback_fn_t cbfunc);
static int register_input_buffer(char *app,
                                 char *version,
                                 char *release,
                                 orcm_pnp_channel_t channel,
                                 orcm_pnp_tag_t tag,
                                 orcm_pnp_callback_buffer_fn_t cbfunc);
static int deregister_input(char *app,
                            char *version,
                            char *release,
                            orcm_pnp_channel_t channel,
                            orcm_pnp_tag_t tag);
static int default_output(orcm_pnp_channel_t channel,
                          orte_process_name_t *recipient,
                          orcm_pnp_tag_t tag,
                          struct iovec *msg, int count);
static int default_output_nb(orcm_pnp_channel_t channel,
                             orte_process_name_t *recipient,
                             orcm_pnp_tag_t tag,
                             struct iovec *msg, int count,
                             orcm_pnp_callback_fn_t cbfunc,
                             void *cbdata);
static int default_output_buffer(orcm_pnp_channel_t channel,
                                 orte_process_name_t *recipient,
                                 orcm_pnp_tag_t tag,
                                 opal_buffer_t *buffer);
static int default_output_buffer_nb(orcm_pnp_channel_t channel,
                                    orte_process_name_t *recipient,
                                    orcm_pnp_tag_t tag,
                                    opal_buffer_t *buffer,
                                    orcm_pnp_callback_buffer_fn_t cbfunc,
                                    void *cbdata);
static orcm_pnp_tag_t define_new_tag(void);
static int default_finalize(void);

/* The module struct */

orcm_pnp_base_module_t orcm_pnp_default_module = {
    default_init,
    announce,
    open_channel,
    register_input,
    register_input_buffer,
    deregister_input,
    default_output,
    default_output_nb,
    default_output_buffer,
    default_output_buffer_nb,
    define_new_tag,
    default_finalize
};

/* Local functions */
static void recv_announcements(int status,
                               orte_process_name_t *sender,
                               orcm_pnp_tag_t tag,
                               opal_buffer_t *buf, void *cbdata);
static void recv_inputs(int status,
                        orte_rmcast_channel_t channel,
                        orte_rmcast_tag_t tag,
                        orte_process_name_t *sender,
                        struct iovec *msg, int count, void *cbdata);
static void recv_input_buffers(int status,
                               orte_rmcast_channel_t channel,
                               orte_rmcast_tag_t tag,
                               orte_process_name_t *sender,
                               opal_buffer_t *buf, void *cbdata);
static int pack_announcement(opal_buffer_t *buf, orte_process_name_t *name);

static void rmcast_callback_buffer(int status,
                                   orte_rmcast_channel_t channel,
                                   orte_rmcast_tag_t tag,
                                   orte_process_name_t *sender,
                                   opal_buffer_t *buf, void* cbdata);

static void rmcast_callback(int status,
                            orte_rmcast_channel_t channel,
                            orte_rmcast_tag_t tag,
                            orte_process_name_t *sender,
                            struct iovec *msg, int count, void* cbdata);

static void rml_callback(int status,
                         struct orte_process_name_t* peer,
                         struct iovec* msg,
                         int count,
                         orte_rml_tag_t tag,
                         void* cbdata);

static void rml_callback_buffer(int status,
                                struct orte_process_name_t* peer,
                                struct opal_buffer_t* buffer,
                                orte_rml_tag_t tag,
                                void* cbdata);

static void* recv_messages(opal_object_t *obj);

static void recv_direct_msgs(int status, orte_process_name_t* sender,
                             opal_buffer_t* buffer, orte_rml_tag_t tag,
                             void* cbdata);

static orcm_pnp_channel_tracker_t* get_channel(char *app,
                                               char *version,
                                               char *release);

static orcm_pnp_channel_tracker_t* find_channel(orcm_pnp_channel_t channel);

static void setup_recv_request(orcm_pnp_channel_tracker_t *tracker,
                               orcm_pnp_tag_t tag,
                               orcm_pnp_callback_fn_t cbfunc,
                               orcm_pnp_callback_buffer_fn_t cbfunc_buf);

static orcm_pnp_pending_request_t* find_request(opal_list_t *list,
                                                orcm_pnp_tag_t tag);

/* Local variables */
static opal_pointer_array_t groups, channels;
static int32_t packet_number = 0;
static bool recv_on = false;
static char *my_app = NULL;
static char *my_version = NULL;
static char *my_release = NULL;
static orcm_pnp_announce_fn_t my_announce_cbfunc = NULL;
static orte_rmcast_channel_t my_channel;
static orcm_pnp_channel_t my_pnp_channels = ORCM_PNP_DYNAMIC_CHANNELS;

/* local thread support */
static opal_mutex_t lock;
static opal_condition_t cond;
static bool active = false;

static int default_init(void)
{
    int ret;
    orcm_pnp_group_t *group;
    orcm_pnp_pending_request_t *request;
    orcm_pnp_source_t *src;
    orcm_pnp_channel_tracker_t *tracker;
    
    /* init the array of known application groups */
    OBJ_CONSTRUCT(&groups, opal_pointer_array_t);
    opal_pointer_array_init(&groups, 8, INT_MAX, 8);
    
    /* init the array of channels */
    OBJ_CONSTRUCT(&channels, opal_pointer_array_t);
    opal_pointer_array_init(&channels, 8, INT_MAX, 8);
    
    /* setup the threading support */
    OBJ_CONSTRUCT(&lock, opal_mutex_t);
    OBJ_CONSTRUCT(&cond, opal_condition_t);
    
    /* record my channel */
    my_channel = orte_rmcast.query_channel();
    
    /* setup a recv to catch any announcements */
    if (!recv_on) {
        /* setup the respective public address channel */
        if (ORCM_PROC_IS_MASTER || ORCM_PROC_IS_DAEMON || ORCM_PROC_IS_TOOL) {
            /* setup a group */
            group = OBJ_NEW(orcm_pnp_group_t);
            group->app = strdup("ORCM_SYSTEM");
            group->channel = ORTE_RMCAST_SYS_CHANNEL;
            /* add to the list of groups */
            opal_pointer_array_add(&groups, group);
            /* add this channel to our list */
            tracker = OBJ_NEW(orcm_pnp_channel_tracker_t);
            tracker->app = strdup(group->app);
            tracker->channel = ORCM_PNP_SYS_CHANNEL;
            opal_pointer_array_add(&channels, tracker);
            OBJ_RETAIN(group);
            opal_pointer_array_add(&tracker->groups, group);
            /* record the request for future use */
            request = OBJ_NEW(orcm_pnp_pending_request_t);
            request->tag = ORTE_RMCAST_TAG_ANNOUNCE;
            request->cbfunc_buf = recv_announcements;
            opal_list_append(&group->requests, &request->super);
            /* open a channel to this group - will just return if
             * the channel already exists
             */
            if (ORCM_SUCCESS != (ret = orte_rmcast.open_channel(&group->channel,
                                                                group->app,
                                                                NULL, -1, NULL,
                                                                ORTE_RMCAST_RECV))) {
                ORTE_ERROR_LOG(ret);
                return ret;
            }
            /* setup to listen to it - will just return if we already are */
            if (ORTE_SUCCESS != (ret = orte_rmcast.recv_buffer_nb(group->channel, request->tag,
                                                                  ORTE_RMCAST_PERSISTENT,
                                                                  recv_input_buffers, NULL))) {
                ORTE_ERROR_LOG(ret);
                return ret;
            }
            
        } else if (ORCM_PROC_IS_APP) {
            /* setup a group */
            group = OBJ_NEW(orcm_pnp_group_t);
            group->app = strdup("ORCM_APP_ANNOUNCE");
            group->channel = ORTE_RMCAST_APP_PUBLIC_CHANNEL;
            /* add to the list of groups */
            opal_pointer_array_add(&groups, group);
            /* add this channel to our list */
            tracker = OBJ_NEW(orcm_pnp_channel_tracker_t);
            tracker->app = strdup(group->app);
            tracker->channel = ORTE_RMCAST_APP_PUBLIC_CHANNEL;
            opal_pointer_array_add(&channels, tracker);
            OBJ_RETAIN(group);
            opal_pointer_array_add(&tracker->groups, group);
            /* record the request for future use */
            request = OBJ_NEW(orcm_pnp_pending_request_t);
            request->tag = ORTE_RMCAST_TAG_ANNOUNCE;
            request->cbfunc_buf = recv_announcements;
            opal_list_append(&group->requests, &request->super);
            
            /* open the channel */
            if (ORTE_SUCCESS != (ret = orte_rmcast.recv_buffer_nb(group->channel, request->tag,
                                                                  ORTE_RMCAST_PERSISTENT,
                                                                  recv_input_buffers, NULL))) {
                ORTE_ERROR_LOG(ret);
                return ret;
            }
            /* setup an RML recv to catch any direct messages */
            if (ORTE_SUCCESS != (ret = orte_rml.recv_buffer_nb(ORTE_NAME_WILDCARD,
                                                               ORTE_RML_TAG_MULTICAST_DIRECT,
                                                               ORTE_RML_NON_PERSISTENT,
                                                               recv_direct_msgs,
                                                               NULL))) {
                ORTE_ERROR_LOG(ret);
                return ret;
            }
        }
        recv_on = true;
    }
    
    return ORCM_SUCCESS;
}

static int announce(char *app, char *version, char *release,
                    orcm_pnp_announce_fn_t cbfunc)
{
    int ret;
    opal_buffer_t buf;
    orcm_pnp_group_t *group;
    orcm_pnp_channel_tracker_t *tracker;
    orcm_pnp_channel_t chan;
    
    /* bozo check */
    if (NULL == app || NULL == version || NULL == release) {
        ORTE_ERROR_LOG(ORCM_ERR_BAD_PARAM);
        return ORCM_ERR_BAD_PARAM;
    }
    
    /* protect against threading */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);
    
    if (NULL != my_app) {
        /* must have been called before */
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ORCM_SUCCESS;
    }
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:announce app %s version %s release %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         app, version, release));
    
    /* retain a local record of my info */
    my_app = strdup(app);
    my_version = strdup(version);
    my_release = strdup(release);
    
    /* retain the callback function */
    my_announce_cbfunc = cbfunc;
    
    /* add to the list of groups */
    group = OBJ_NEW(orcm_pnp_group_t);
    group->app = strdup(my_app);
    group->version = strdup(my_version);
    group->release = strdup(my_release);
    group->channel = my_channel;
    opal_pointer_array_add(&groups, group);
    
    /* add it to our list of channels */
    tracker = OBJ_NEW(orcm_pnp_channel_tracker_t);
    tracker->app = strdup(my_app);
    tracker->version = strdup(my_version);
    tracker->release = strdup(my_release);
    tracker->channel = ORCM_PNP_GROUP_OUTPUT_CHANNEL;
    opal_pointer_array_add(&channels, tracker);
    OBJ_RETAIN(group);
    opal_pointer_array_add(&tracker->groups, group);

    /* no need to hold the lock any further */
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
    
    /* assemble the announcement message */
    OBJ_CONSTRUCT(&buf, opal_buffer_t);
    
    /* pack the common elements */
    if (ORCM_SUCCESS != (ret = pack_announcement(&buf, ORTE_NAME_INVALID))) {
        ORTE_ERROR_LOG(ret);
        OBJ_DESTRUCT(&buf);
        OPAL_THREAD_UNLOCK(&lock);
        return ret;
    }
    
    /* select the channel */
    if (ORCM_PROC_IS_APP) {
        chan = ORTE_RMCAST_APP_PUBLIC_CHANNEL;
    } else {
        chan = ORTE_RMCAST_SYS_CHANNEL;
    }
    
    /* send it */
    if (ORCM_SUCCESS != (ret = orte_rmcast.send_buffer(chan, ORTE_RMCAST_TAG_ANNOUNCE, &buf))) {
        ORTE_ERROR_LOG(ret);
    }
    
    /* cleanup */
    OBJ_DESTRUCT(&buf);
    
    return ret;
}

static orcm_pnp_channel_t open_channel(char *app, char *version, char *release)
{
    orcm_pnp_channel_tracker_t *tracker;
    orcm_pnp_group_t *grp;
    orcm_pnp_pending_request_t *request;
    opal_list_item_t *item;
    int i, rc;
    
    /* bozo check */
    if (NULL == app) {
        return ORCM_PNP_INVALID_CHANNEL;
    }
    
    /* protect the global arrays */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);

    /* see if we already have this channel - automatically
     * creates it if not
     */
    tracker = get_channel(app, version, release);
    
    /* if this channel already existed, it may have groups in it - so we
     * need to loop across them and open a channel, if not already done
     */
    for (i=0; i < tracker->groups.size; i++) {
        if (NULL == (grp = (orcm_pnp_group_t*)opal_pointer_array_get_item(&tracker->groups, i))) {
            continue;
        }
        if (ORTE_RMCAST_INVALID_CHANNEL == grp->channel) {
            /* don't know this one yet */
            continue;
        }
        /* open a channel to this group - will just return if
         * the channel already exists
         */
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:open_channel opening channel %d",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), grp->channel));
        if (ORCM_SUCCESS != (rc = orte_rmcast.open_channel(&grp->channel,
                                                           grp->app, NULL, -1, NULL,
                                                           ORTE_RMCAST_BIDIR))) {
            ORTE_ERROR_LOG(rc);
            break;
        }
        /* if there are any pending requests, setup a recv for them */
        if (0 < opal_list_get_size(&grp->requests)) {
            /* open the channel */
            OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                                 "%s pnp:default:open_channel setup recv for channel %d",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), grp->channel));
            /* setup the recvs */
            if (ORTE_SUCCESS != (rc = orte_rmcast.recv_nb(grp->channel,
                                                          ORTE_RMCAST_TAG_WILDCARD,
                                                          ORTE_RMCAST_PERSISTENT,
                                                          recv_inputs, NULL))) {
                ORTE_ERROR_LOG(rc);
                continue;
            }
            if (ORTE_SUCCESS != (rc = orte_rmcast.recv_buffer_nb(grp->channel,
                                                                 ORTE_RMCAST_TAG_WILDCARD,
                                                                 ORTE_RMCAST_PERSISTENT,
                                                                 recv_input_buffers, NULL))) {
                ORTE_ERROR_LOG(rc);
                continue;
            }
        }
    }
    
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
    
    return tracker->channel;
}

static int register_input(char *app,
                          char *version,
                          char *release,
                          orcm_pnp_channel_t channel,
                          orcm_pnp_tag_t tag,
                          orcm_pnp_callback_fn_t cbfunc)
{
    orcm_pnp_channel_tracker_t *tracker;
    orcm_pnp_group_t *grp;
    int i, ret=ORCM_SUCCESS;

    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:register_input app %s version %s release %s tag %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (NULL == app) ? "NULL" : app,
                         (NULL == version) ? "NULL" : version,
                         (NULL == release) ? "NULL" : release, tag));
    
    /* since we are modifying global lists, lock the thread */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);
    
    if (ORCM_PNP_GROUP_OUTPUT_CHANNEL == channel) {
        /* get a tracker object for this triplet - creates
         * it if one doesn't already exist
         */
        tracker = get_channel(app, version, release);        
    } else {
        /* if the channel is something other than the group output
         * channel for this triplet, then lookup the tracker object
         * for that channel
         */
        if (NULL == (tracker = find_channel(channel))) {
            ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
            ret = ORTE_ERR_NOT_FOUND;
            goto cleanup;
        }
    }
    
    /* record the request - will just return if this request
     * already exists
     */
    setup_recv_request(tracker, tag, cbfunc, NULL);
    
    /* ensure we are listening to any pre-known groups */
    for (i=0; i < tracker->groups.size; i++) {
        if (NULL == (grp = (orcm_pnp_group_t*)opal_pointer_array_get_item(&tracker->groups, i))) {
            continue;
        }
        if (ORTE_RMCAST_INVALID_CHANNEL == grp->channel) {
            /* don't know this one yet */
            continue;
        }
        /* open a channel to this group - will just return if
         * the channel already exists
         */
        if (ORCM_SUCCESS != (ret = orte_rmcast.open_channel(&grp->channel,
                                                            grp->app,
                                                            NULL, -1, NULL,
                                                            ORTE_RMCAST_BIDIR))) {
            ORTE_ERROR_LOG(ret);
            goto cleanup;
        }
        /* setup to listen to it - will just return if we already are */
        if (ORTE_SUCCESS != (ret = orte_rmcast.recv_nb(grp->channel, tag,
                                                       ORTE_RMCAST_PERSISTENT,
                                                       recv_inputs, cbfunc))) {
            ORTE_ERROR_LOG(ret);
            goto cleanup;
        }
    }
    
cleanup:
    /* clear the thread */
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
    
    return ret;
}

static int register_input_buffer(char *app,
                                 char *version,
                                 char *release,
                                 orcm_pnp_channel_t channel,
                                 orcm_pnp_tag_t tag,
                                 orcm_pnp_callback_buffer_fn_t cbfunc)
{
    orcm_pnp_channel_tracker_t *tracker;
    orcm_pnp_group_t *grp;
    int i, ret=ORCM_SUCCESS;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:register_input_buffer app %s version %s release %s tag %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (NULL == app) ? "NULL" : app,
                         (NULL == version) ? "NULL" : version,
                         (NULL == release) ? "NULL" : release, tag));
    
    /* since we are modifying global lists, lock
     * the thread
     */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);
    
    if (ORCM_PNP_GROUP_OUTPUT_CHANNEL == channel) {
        /* get a tracker object for this triplet - creates
         * it if one doesn't already exist
         */
        tracker = get_channel(app, version, release);        
    } else {
        /* if the channel is something other than the group output
         * channel for this triplet, then lookup the tracker object
         * for that channel
         */
        if (NULL == (tracker = find_channel(channel))) {
            ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
            ret = ORTE_ERR_NOT_FOUND;
            goto cleanup;
        }
    }
    
    /* record the request - will just return if this request
     * already exists
     */
    setup_recv_request(tracker, tag, NULL, cbfunc);
    
    /* ensure we are listening to any pre-known groups */
    for (i=0; i < tracker->groups.size; i++) {
        if (NULL == (grp = (orcm_pnp_group_t*)opal_pointer_array_get_item(&tracker->groups, i))) {
            continue;
        }
        if (ORTE_RMCAST_INVALID_CHANNEL == grp->channel) {
            /* don't know this one yet */
            continue;
        }
        /* open a channel to this group - will just return if
         * the channel already exists
         */
        if (ORCM_SUCCESS != (ret = orte_rmcast.open_channel(&grp->channel,
                                                            grp->app,
                                                            NULL, -1, NULL,
                                                            ORTE_RMCAST_BIDIR))) {
            ORTE_ERROR_LOG(ret);
            goto cleanup;
        }
        /* setup to listen to it - will just return if we already are */
        if (ORTE_SUCCESS != (ret = orte_rmcast.recv_buffer_nb(grp->channel, tag,
                                                              ORTE_RMCAST_PERSISTENT,
                                                              recv_input_buffers, cbfunc))) {
            ORTE_ERROR_LOG(ret);
            goto cleanup;
        }
    }
    
cleanup:
    /* clear the thread */
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
    
    return ret;
}

static int deregister_input(char *app,
                            char *version,
                            char *release,
                            orcm_pnp_channel_t channel,
                            orcm_pnp_tag_t tag)
{
    orcm_pnp_group_t *group;
    orcm_pnp_channel_tracker_t *tracker;
    orcm_pnp_pending_request_t *request;
    opal_list_item_t *item;
    int i, j, k;
    int ret=ORCM_SUCCESS;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:deregister_input app %s version %s release %s channel %d tag %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         app, (NULL == version) ? "NULL" : version,
                         (NULL == release) ? "NULL" : release, channel, tag));
    
    /* since we are modifying global lists, lock
     * the thread
     */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);
    
    if (ORCM_PNP_GROUP_OUTPUT_CHANNEL == channel) {
        /* get a tracker object for this triplet - creates
         * it if one doesn't already exist
         */
        tracker = get_channel(app, version, release);        
    } else {
        /* if the channel is something other than the group output
         * channel for this triplet, then lookup the tracker object
         * for that channel
         */
        if (NULL == (tracker = find_channel(channel))) {
            ORTE_ERROR_LOG(ORTE_ERR_NOT_FOUND);
            ret = ORTE_ERR_NOT_FOUND;
            goto cleanup;
        }
    }
    
    /* if this is a wildcard tag, remove all recvs */
    if (ORCM_PNP_TAG_WILDCARD == tag) {
        while (NULL != (item = opal_list_remove_first(&tracker->requests))) {
            OBJ_RELEASE(item);
        }
    } else {
        /* remove the recv for this tag, if it exists */
        if (NULL != (request = find_request(&tracker->requests, tag))) {
            opal_list_remove_item(&tracker->requests, &request->super);
            OBJ_RELEASE(request);
        }
    }
    
    /* now cycle through the groups and remove the corresponding
     * recvs from them
     */
    for (j=0; j < tracker->groups.size; j++) {
        if (NULL == (group = (orcm_pnp_group_t*)opal_pointer_array_get_item(&tracker->groups, j))) {
            continue;
        }
        if (ORCM_PNP_TAG_WILDCARD == tag) {
            while (NULL != (item = opal_list_remove_first(&group->requests))) {
                OBJ_RELEASE(item);
            }
        } else {
            if (NULL != (request = find_request(&group->requests, tag))) {
                opal_list_remove_item(&group->requests, &request->super);
                OBJ_RELEASE(request);
            }
        }
    }

cleanup:
    /* clear the thread */
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
    
    return ret;
}

static int default_output(orcm_pnp_channel_t channel,
                          orte_process_name_t *recipient,
                          orcm_pnp_tag_t tag,
                          struct iovec *msg, int count)
{
    orcm_pnp_channel_tracker_t *tracker;
    orcm_pnp_group_t *grp;
    int i, ret;
    opal_buffer_t buf;
    int8_t flag;
    int32_t cnt;
    int sz;
    
    /* protect against threading */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);
    
    /* find the channel */
    if (NULL == (tracker = find_channel(channel))) {
        /* don't know this channel - throw bits on floor */
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ORCM_SUCCESS;
    }
    
    /* if this is intended for everyone who might be listening on this channel,
     * multicast it to all groups in this channel
     */
    if (NULL == recipient ||
       (ORTE_JOBID_WILDCARD == recipient->jobid &&
        ORTE_VPID_WILDCARD == recipient->vpid)) {
           OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                                "%s pnp:default:sending multicast of %d iovecs to tag %d",
                                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), count, tag));
           
           for (i=0; i < tracker->groups.size; i++) {
               if (NULL == (grp = (orcm_pnp_group_t*)opal_pointer_array_get_item(&tracker->groups, i))) {
                   continue;
               }
               if (ORTE_RMCAST_INVALID_CHANNEL == grp->channel) {
                   /* don't know this one yet */
                   continue;
               }
               /* send the iovecs to the channel */
               if (ORCM_SUCCESS != (ret = orte_rmcast.send(grp->channel, tag, msg, count))) {
                   ORTE_ERROR_LOG(ret);
               }
           }
           OPAL_RELEASE_THREAD(&lock, &cond, &active);
           return ORCM_SUCCESS;
       }
    
    /* if only one name field is WILDCARD, I don't know how to send
     * it - at least, not right now
     */
    if (ORTE_JOBID_WILDCARD == recipient->jobid ||
        ORTE_VPID_WILDCARD == recipient->vpid) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_IMPLEMENTED);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ORTE_ERR_NOT_IMPLEMENTED;
    }
    
    /* intended for a specific recipient, send it over p2p */
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:sending p2p message of %d iovecs to tag %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), count, tag));
    /* make a tmp buffer */
    OBJ_CONSTRUCT(&buf, opal_buffer_t);
    /* pack our channel so the recipient can figure out who it came from */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(&buf, &my_channel, 1, ORTE_RMCAST_CHANNEL_T))) {
        ORTE_ERROR_LOG(ret);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ret;
    }    
    /* flag the buffer as containing iovecs */
    flag = 0;
    if (ORCM_SUCCESS != (ret = opal_dss.pack(&buf, &flag, 1, OPAL_INT8))) {
        ORTE_ERROR_LOG(ret);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ret;
    }
    /* pass the target PNP tag */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(&buf, &tag, 1, ORCM_PNP_TAG_T))) {
        ORTE_ERROR_LOG(ret);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ret;
    }
    /* pack the number of iovecs */
    cnt = count;
    if (ORCM_SUCCESS != (ret = opal_dss.pack(&buf, &cnt, 1, OPAL_INT32))) {
        ORTE_ERROR_LOG(ret);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ret;
    }    
    
    /* pack each iovec into a buffer in prep for sending
     * so we can recreate the array at the other end
     */
    for (sz=0; sz < count; sz++) {
        /* pack the size */
        cnt = msg[sz].iov_len;
        if (ORCM_SUCCESS != (ret = opal_dss.pack(&buf, &cnt, 1, OPAL_INT32))) {
            ORTE_ERROR_LOG(ret);
            OPAL_RELEASE_THREAD(&lock, &cond, &active);
            return ret;
        }
        if (0 < cnt) {
            /* pack the bytes */
            if (ORCM_SUCCESS != (ret = opal_dss.pack(&buf, msg[sz].iov_base, cnt, OPAL_UINT8))) {
                ORTE_ERROR_LOG(ret);
                OPAL_RELEASE_THREAD(&lock, &cond, &active);
                return ret;
            }            
        }
    }
    
    /* release the thread prior to send */
    OPAL_RELEASE_THREAD(&lock, &cond, &active);

    /* send the msg */
    if (0 > (ret = orte_rml.send_buffer(recipient, &buf, ORTE_RML_TAG_MULTICAST_DIRECT, 0))) {
        ORTE_ERROR_LOG(ret);
    } else {
        ret = ORCM_SUCCESS;
    }
    OBJ_DESTRUCT(&buf);
    return ret;
}

static int default_output_nb(orcm_pnp_channel_t channel,
                             orte_process_name_t *recipient,
                             orcm_pnp_tag_t tag,
                             struct iovec *msg, int count,
                             orcm_pnp_callback_fn_t cbfunc,
                             void *cbdata)
{
    orcm_pnp_channel_tracker_t *tracker;
    orcm_pnp_group_t *grp;
    int i, ret;
    orcm_pnp_send_t *send;
    opal_buffer_t *buf;
    int8_t flag;
    int32_t cnt;
    int sz;
    
    /* protect against threading */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);
    
    /* find the channel */
    if (NULL == (tracker = find_channel(channel))) {
        /* don't know this channel - throw bits on floor */
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ORCM_SUCCESS;
    }
    
    send = OBJ_NEW(orcm_pnp_send_t);
    send->tag = tag;
    send->msg = msg;
    send->count = count;
    send->cbfunc = cbfunc;
    send->cbdata = cbdata;
    
    /* if this is intended for everyone who might be listening to my output,
     * multicast it
     */
    if (NULL == recipient ||
        (ORTE_JOBID_WILDCARD == recipient->jobid &&
         ORTE_VPID_WILDCARD == recipient->vpid)) {
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                            "%s pnp:default:sending multicast of %d iovecs to tag %d",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), count, tag));
            
            for (i=0; i < tracker->groups.size; i++) {
                if (NULL == (grp = (orcm_pnp_group_t*)opal_pointer_array_get_item(&tracker->groups, i))) {
                    continue;
                }
                if (ORTE_RMCAST_INVALID_CHANNEL == grp->channel) {
                    /* don't know this one yet */
                    continue;
                }
                /* maintain accounting */
                OBJ_RETAIN(send);
                /* send the iovecs to the channel */
                if (ORCM_SUCCESS != (ret = orte_rmcast.send_nb(grp->channel, tag,
                                                               msg, count, rmcast_callback, send))) {
                    ORTE_ERROR_LOG(ret);
                }
            }
            /* correct accounting */
            OBJ_RELEASE(send);
            OPAL_RELEASE_THREAD(&lock, &cond, &active);
            return ORCM_SUCCESS;
        }
    
    /* if only one name field is WILDCARD, I don't know how to send
     * it - at least, not right now
     */
    if (ORTE_JOBID_WILDCARD == recipient->jobid ||
        ORTE_VPID_WILDCARD == recipient->vpid) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_IMPLEMENTED);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ORTE_ERR_NOT_IMPLEMENTED;
    }
    
    /* intended for a specific recipient, send it over p2p */
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:sending p2p message of %d iovecs to tag %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), count, tag));
    /* make a tmp buffer */
    buf = OBJ_NEW(opal_buffer_t);
    /* pack our channel so the recipient can figure out who it came from */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &my_channel, 1, ORTE_RMCAST_CHANNEL_T))) {
        ORTE_ERROR_LOG(ret);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ret;
    }    
    /* flag the buffer as containing iovecs */
    flag = 0;
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &flag, 1, OPAL_INT8))) {
        ORTE_ERROR_LOG(ret);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ret;
    }    
    /* pass the target tag */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &tag, 1, ORCM_PNP_TAG_T))) {
        ORTE_ERROR_LOG(ret);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ret;
    }    
    /* pack the number of iovecs */
    cnt = count;
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &cnt, 1, OPAL_INT32))) {
        ORTE_ERROR_LOG(ret);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ret;
    }    
    
    /* pack each iovec into a buffer in prep for sending
     * so we can recreate the array at the other end
     */
    for (sz=0; sz < count; sz++) {
        /* pack the size */
        cnt = msg[sz].iov_len;
        if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &cnt, 1, OPAL_INT32))) {
            ORTE_ERROR_LOG(ret);
            OPAL_RELEASE_THREAD(&lock, &cond, &active);
            return ret;
        }        
        if (0 < cnt) {
            /* pack the bytes */
            if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, msg[sz].iov_base, cnt, OPAL_UINT8))) {
                ORTE_ERROR_LOG(ret);
                OPAL_RELEASE_THREAD(&lock, &cond, &active);
                return ret;
            }            
        }
    }
    
    /* release thread prior to send */
    OPAL_RELEASE_THREAD(&lock, &cond, &active);

    /* send the msg */
    if (0 > (ret = orte_rml.send_buffer_nb(recipient, buf,
                                           ORTE_RML_TAG_MULTICAST_DIRECT, 0,
                                           rml_callback_buffer, send))) {
        ORTE_ERROR_LOG(ret);
    } else {
        ret = ORCM_SUCCESS;
    }
    return ret;
}

static int default_output_buffer(orcm_pnp_channel_t channel,
                                 orte_process_name_t *recipient,
                                 orcm_pnp_tag_t tag,
                                 opal_buffer_t *buffer)
{
    orcm_pnp_channel_tracker_t *tracker;
    orcm_pnp_group_t *grp;
    int i, ret;
    opal_buffer_t buf;
    int8_t flag;
    
    /* protect against threading */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);
    
    /* find the channel */
    if (NULL == (tracker = find_channel(channel))) {
        /* don't know this channel - throw bits on floor */
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:output_buffer unrecognized channel %d tag %d",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             (int)grp->channel, tag));
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ORCM_SUCCESS;
    }
    
    /* if this is intended for everyone who might be listening on this channel,
     * multicast it to all groups in this channel
     */
    if (NULL == recipient ||
        (ORTE_JOBID_WILDCARD == recipient->jobid &&
         ORTE_VPID_WILDCARD == recipient->vpid)) {
            
        for (i=0; i < tracker->groups.size; i++) {
            if (NULL == (grp = (orcm_pnp_group_t*)opal_pointer_array_get_item(&tracker->groups, i))) {
                continue;
            }
            if (ORTE_RMCAST_INVALID_CHANNEL == grp->channel) {
                /* don't know this one yet */
                continue;
            }
            OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                                 "%s pnp:default:sending multicast buffer of %d bytes to channel %d tag %d",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (int)buffer->bytes_used,
                                 (int)grp->channel, tag));
            /* send the buffer to the group output channel */
            if (ORCM_SUCCESS != (ret = orte_rmcast.send_buffer(grp->channel, tag, buffer))) {
                ORTE_ERROR_LOG(ret);
            }
            OPAL_RELEASE_THREAD(&lock, &cond, &active);
            return ORCM_SUCCESS;
        }
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:output_buffer unrecognized channel %d tag %d",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             (int)grp->channel, tag));
        return ORCM_ERR_NOT_FOUND;
    }            

    /* if only one name field is WILDCARD, I don't know how to send
     * it - at least, not right now
     */
    if (ORTE_JOBID_WILDCARD == recipient->jobid ||
        ORTE_VPID_WILDCARD == recipient->vpid) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_IMPLEMENTED);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ORCM_ERR_NOT_IMPLEMENTED;
    }
    
    /* intended for a specific recipient, send it over p2p */
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:sending p2p buffer of %d bytes to tag %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (int)buffer->bytes_used, tag));
    /* make a tmp buffer */
    OBJ_CONSTRUCT(&buf, opal_buffer_t);
    /* pack our channel so the recipient can figure out who it came from */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(&buf, &my_channel, 1, ORTE_RMCAST_CHANNEL_T))) {
        ORTE_ERROR_LOG(ret);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ret;
    }    
    /* flag that we sent a buffer */
    flag = 1;
    if (ORCM_SUCCESS != (ret = opal_dss.pack(&buf, &flag, 1, OPAL_INT8))) {
        ORTE_ERROR_LOG(ret);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ret;
    }    
    /* pass the target PNP tag */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(&buf, &tag, 1, ORCM_PNP_TAG_T))) {
        ORTE_ERROR_LOG(ret);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ret;
    }    
    /* copy the payload */
    if (ORTE_SUCCESS != (ret = opal_dss.copy_payload(&buf, buffer))) {
        ORTE_ERROR_LOG(ret);
        OBJ_DESTRUCT(&buf);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ret;
    }
    
    /* release thread prior to send */
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
        
    if (0 > (ret = orte_rml.send_buffer(recipient, &buf, ORTE_RML_TAG_MULTICAST_DIRECT, 0))) {
        ORTE_ERROR_LOG(ret);
    } else {
        ret = ORCM_SUCCESS;
    }
    OBJ_DESTRUCT(&buf);
    return ret;
}

static int default_output_buffer_nb(orcm_pnp_channel_t channel,
                                    orte_process_name_t *recipient,
                                    orcm_pnp_tag_t tag,
                                    opal_buffer_t *buffer,
                                    orcm_pnp_callback_buffer_fn_t cbfunc,
                                    void *cbdata)
{
    orcm_pnp_channel_tracker_t *tracker;
    orcm_pnp_group_t *grp;
    int i, ret;
    orcm_pnp_send_t *send;
    opal_buffer_t *buf;
    int8_t flag;
    
    /* protect against threading */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);
    
    /* find the channel */
    if (NULL == (tracker = find_channel(channel))) {
        /* don't know this channel - throw bits on floor */
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ORCM_SUCCESS;
    }
    
    send = OBJ_NEW(orcm_pnp_send_t);
    send->tag = tag;
    send->buffer = buffer;
    send->cbfunc_buf = cbfunc;
    send->cbdata = cbdata;
    
    /* if this is intended for everyone who might be listening to my output,
     * multicast it
     */
    if (NULL == recipient ||
        (ORTE_JOBID_WILDCARD == recipient->jobid &&
         ORTE_VPID_WILDCARD == recipient->vpid)) {
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:sending multicast buffer of %d bytes to tag %d",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (int)buffer->bytes_used, tag));
            
            for (i=0; i < tracker->groups.size; i++) {
                if (NULL == (grp = (orcm_pnp_group_t*)opal_pointer_array_get_item(&tracker->groups, i))) {
                    continue;
                }
                if (ORTE_RMCAST_INVALID_CHANNEL == grp->channel) {
                    /* don't know this one yet */
                    continue;
                }
                /* maintain accounting */
                OBJ_RETAIN(send);
                /* send the iovecs to the channel */
                if (ORCM_SUCCESS != (ret = orte_rmcast.send_buffer_nb(grp->channel, tag, buffer,
                                                                      rmcast_callback_buffer, send))) {
                    ORTE_ERROR_LOG(ret);
                }
            }
            /* correct accounting */
            OBJ_RELEASE(send);
            OPAL_RELEASE_THREAD(&lock, &cond, &active);
            return ORCM_SUCCESS;
        }
    
    /* if only one name field is WILDCARD, I don't know how to send
     * it - at least, not right now
     */
    if (ORTE_JOBID_WILDCARD == recipient->jobid ||
        ORTE_VPID_WILDCARD == recipient->vpid) {
        ORTE_ERROR_LOG(ORTE_ERR_NOT_IMPLEMENTED);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ORTE_ERR_NOT_IMPLEMENTED;
    }
    
    /* intended for a specific recipient, send it over p2p */
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:sending p2p buffer of %d bytes to tag %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (int)buffer->bytes_used, tag));
    /* make a tmp buffer */
    buf = OBJ_NEW(opal_buffer_t);
    /* pack our channel so the recipient can figure out who it came from */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &my_channel, 1, ORTE_RMCAST_CHANNEL_T))) {
        ORTE_ERROR_LOG(ret);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ret;
    }    
    /* flag that we sent a buffer */
    flag = 1;
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &flag, 1, OPAL_INT8))) {
        ORTE_ERROR_LOG(ret);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ret;
    }    
    /* pass the target tag */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &tag, 1, ORCM_PNP_TAG_T))) {
        ORTE_ERROR_LOG(ret);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ret;
    }    
    /* copy the payload */
    if (ORTE_SUCCESS != (ret = opal_dss.copy_payload(buf, buffer))) {
        ORTE_ERROR_LOG(ret);
        OBJ_RELEASE(buf);
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        return ret;
    }
    
    /* release thread prior to send */
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
    
    if (0 > (ret = orte_rml.send_buffer_nb(recipient, buf,
                                           ORTE_RML_TAG_MULTICAST_DIRECT, 0,
                                           rml_callback_buffer, send))) {
        ORTE_ERROR_LOG(ret);
    } else {
        ret = ORCM_SUCCESS;
    }
    return ret;
}

static orcm_pnp_tag_t define_new_tag(void)
{
    return ORCM_PNP_TAG_INVALID;
}

static int default_finalize(void)
{
    int i;
    orcm_pnp_channel_tracker_t *tracker;
    orcm_pnp_group_t *group;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default: finalizing",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
    /* cancel the recvs, if active */
    if (recv_on) {
        orte_rmcast.cancel_recv(ORTE_RMCAST_APP_PUBLIC_CHANNEL, ORTE_RMCAST_TAG_ANNOUNCE);
        orte_rml.recv_cancel(ORTE_NAME_WILDCARD, ORTE_RML_TAG_MULTICAST_DIRECT);
        recv_on = false;
    }
    
    /* destruct the threading support */
    OBJ_DESTRUCT(&lock);
    OBJ_DESTRUCT(&cond);
    
    /* release the array of known application groups */
    for (i=0; i < groups.size; i++) {
        if (NULL != (group = (orcm_pnp_group_t*)opal_pointer_array_get_item(&groups, i))) {
            OBJ_RELEASE(group);
        }
    }
    OBJ_DESTRUCT(&groups);
    /* release the array of known channels */
    for (i=0; i < channels.size; i++) {
        if (NULL != (tracker = (orcm_pnp_channel_tracker_t*)opal_pointer_array_get_item(&channels, i))) {
            OBJ_RELEASE(tracker);
        }
    }
    OBJ_DESTRUCT(&channels);
    
    return ORCM_SUCCESS;
}


/****    LOCAL  FUNCTIONS    ****/
static void recv_announcements(int status,
                               orte_process_name_t *fake,
                               orcm_pnp_tag_t tag,
                               opal_buffer_t *buf, void *cbdata)
{
    opal_list_item_t *itm2;
    orcm_pnp_channel_tracker_t *tracker;
    orcm_pnp_group_t *group, *grp;
    orcm_pnp_source_t *source;
    char *app=NULL, *version=NULL, *release=NULL, *nodename=NULL;
    orte_process_name_t originator;
    opal_buffer_t ann;
    int rc, n, i, j;
    orcm_pnp_pending_request_t *request, *req;
    orte_rmcast_channel_t output;
    orte_process_name_t sender;
    orcm_pnp_send_t *pkt;
    orcm_pnp_channel_t chan;
    orte_job_t *daemons;
    orte_proc_t *proc;
    
    /* unpack the name of the sender */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &sender, &n, ORTE_NAME))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    
    /* unpack the app's name */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &app, &n, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    
    /* get its version */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &version, &n, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    
    /* get its release */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &release, &n, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    
    /* get its multicast channel */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &output, &n, ORTE_RMCAST_CHANNEL_T))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    
    /* get its nodename */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &nodename, &n, OPAL_STRING))) {
        ORTE_ERROR_LOG(rc);
        return;
    }
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received announcement from group app %s version %s release %s channel %d on node %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         app, version, release, output, nodename));
    
    /* since we are accessing global lists, acquire the thread */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);
    
    /* do we already know this application group? */
    for (i=0; i < groups.size; i++) {
        if (NULL == (group = (orcm_pnp_group_t*)opal_pointer_array_get_item(&groups, i))) {
            continue;
        }
        
        /* the triplet must be unique */
        if (0 != strcasecmp(group->app, app)) {
            continue;
        }
        if (0 != strcasecmp(group->version, version)) {
            continue;
        }
        if (NULL != group->release && 0 != strcasecmp(group->release, release)) {
            continue;
        }

        /* record the multicast channel it is on */
        group->channel = output;
        goto recvs;
    }
    
    /* if we get here, then this is a new application
     * group - add it to our list
     */
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received_announcement has new group",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
    group = OBJ_NEW(orcm_pnp_group_t);
    group->app = strdup(app);
    group->version = strdup(version);
    group->release = strdup(release);
    group->channel = output;
    opal_pointer_array_add(&groups, group);
    
    /* check which channels it might belong to */
    for (i=0; i < channels.size; i++) {
        if (NULL == (tracker = (orcm_pnp_channel_tracker_t*)opal_pointer_array_get_item(&channels, i))) {
            continue;
        }
        if (0 != strcasecmp(group->app, tracker->app)) {
            continue;
        }
        if (NULL != tracker->version && 0 != strcasecmp(tracker->version, group->version)) {
            continue;
        }
        if (NULL != tracker->release && 0 != strcasecmp(tracker->release, group->release)) {
            continue;
        }
        /* have a match - add the group */
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:adding group %s:%s:%s to tracker %s:%s:%s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             group->app, group->version, group->release,
                             tracker->app, (NULL == tracker->version) ? "NULL" : tracker->version,
                             (NULL == tracker->release) ? "NULL" : tracker->release));
        OBJ_RETAIN(group);
        opal_pointer_array_add(&tracker->groups, group);
        /* open the channel */
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:recvd_ann opening channel %d",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), output));
        /* open a channel to this group - will just return if
         * the channel already exists
         */
        if (ORCM_SUCCESS != (rc = orte_rmcast.open_channel(&output,
                                                           group->app, NULL, -1, NULL,
                                                           ORTE_RMCAST_BIDIR))) {
            ORTE_ERROR_LOG(rc);
            goto RELEASE;
        }
        /* add any pending requests associated with this channel */
        for (itm2 = opal_list_get_first(&tracker->requests);
             itm2 != opal_list_get_end(&tracker->requests);
             itm2 = opal_list_get_next(itm2)) {
            req = (orcm_pnp_pending_request_t*)itm2;
            if (NULL != find_request(&group->requests, req->tag)) {
                /* already assigned */
                continue;
            }
            /* add it */
            request = OBJ_NEW(orcm_pnp_pending_request_t);
            request->tag = req->tag;
            request->cbfunc = req->cbfunc;
            request->cbfunc_buf = req->cbfunc_buf;
            opal_list_append(&group->requests, &request->super);
        }
    }
    
recvs:
    /* do we want to listen to this group? */
    if (0 < opal_list_get_size(&group->requests)) {
        /* open the channel */
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:recvd_ann setup recv for channel %d",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), output));
        /* setup the recvs */
        if (ORTE_SUCCESS != (rc = orte_rmcast.recv_nb(output,
                                                      ORTE_RMCAST_TAG_WILDCARD,
                                                      ORTE_RMCAST_PERSISTENT,
                                                      recv_inputs, NULL))) {
            ORTE_ERROR_LOG(rc);
            goto RELEASE;
        }
        if (ORTE_SUCCESS != (rc = orte_rmcast.recv_buffer_nb(output,
                                                             ORTE_RMCAST_TAG_WILDCARD,
                                                             ORTE_RMCAST_PERSISTENT,
                                                             recv_input_buffers, NULL))) {
            ORTE_ERROR_LOG(rc);
            goto RELEASE;
        }
    }

    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received announcement from source %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(&sender)));
    
    /* do we already know this source? */
    source = (orcm_pnp_source_t*)opal_pointer_array_get_item(&group->members, sender.vpid);
    if (NULL == source) {
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:received adding source %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(&sender)));
        
        source = OBJ_NEW(orcm_pnp_source_t);
        source->name.jobid = sender.jobid;
        source->name.vpid = sender.vpid;
        opal_pointer_array_set_item(&group->members, sender.vpid, source);
    }
    
    /* who are they responding to? */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buf, &originator, &n, ORTE_NAME))) {
        ORTE_ERROR_LOG(rc);
        goto RELEASE;
    }
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received announcement from originator %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(&originator)));
    
    /* if they were responding to an announcement by someone,
     * then don't respond or else we'll go into an infinite
     * loop of announcements
     */
    if (originator.jobid != ORTE_JOBID_INVALID &&
        originator.vpid != ORTE_VPID_INVALID) {
        /* nothing more to do */
        goto RELEASE;
    }
    
    /* if we get here, then this is an original announcement */
    if (ORCM_PROC_IS_APP) {
        chan = ORTE_RMCAST_APP_PUBLIC_CHANNEL;
    } else {
        chan = ORTE_RMCAST_SYS_CHANNEL;
    }

    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received announcement sending response",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
    /* assemble the announcement response */
    OBJ_CONSTRUCT(&ann, opal_buffer_t);
    
    /* pack the common elements */
    if (ORCM_SUCCESS != (rc = pack_announcement(&ann, &sender))) {
        if (ORCM_ERR_NOT_AVAILABLE != rc) {
            /* not-avail => have not announced ourselves yet */
            ORTE_ERROR_LOG(rc);
        }
        OBJ_DESTRUCT(&ann);
        goto RELEASE;
    }
    
    /* send it - have to release the thread in case we receive something right away */
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
    if (ORCM_SUCCESS != (rc = orte_rmcast.send_buffer(chan,
                                                      ORTE_RMCAST_TAG_ANNOUNCE,
                                                      &ann))) {
        ORTE_ERROR_LOG(rc);
    }
    
    /* cleanup */
    OBJ_DESTRUCT(&ann);
    goto CALLBACK;
    
RELEASE:
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
    
CALLBACK:
    /* if they wanted a callback, now is the time to do it */
    if (NULL != my_announce_cbfunc) {
        my_announce_cbfunc(app, version, release, &sender, nodename);
    }
    if (NULL != app) {
        free(app);
    }
    if (NULL != version) {
        free(version);
    }
    if (NULL != release) {
        free(release);
    }
    if (NULL != nodename) {
        free(nodename);
    }
    return;
}

static void recv_inputs(int status,
                        orte_rmcast_channel_t channel,
                        orte_rmcast_tag_t tag,
                        orte_process_name_t *sender,
                        struct iovec *msg, int count, void *cbdata)
{
    orcm_pnp_group_t *group, *grp;
    orcm_pnp_source_t *src, *leader;
    orcm_pnp_pending_request_t *request;
    int i, rc;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received input on channel %d from sender %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), channel,
                         ORTE_NAME_PRINT(sender)));
    
    /* protect against threading */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);
    
    /* the rmcast channel is associated with a group - get it */
    grp = NULL;
    for (i=0; i < groups.size; i++) {
        if (NULL == (group = (orcm_pnp_group_t*)opal_pointer_array_get_item(&groups, i))) {
            continue;
        }
        if (channel == group->channel) {
            grp = group;
            break;
        }
    }
    if (NULL == grp) {
        /* we don't know this channel yet - just ignore it */
        goto DEPART;
    }
    
    /* find the request object for this tag */
    if (NULL == (request = find_request(&grp->requests, tag))) {
        /* if there isn't one for this specific tag, is there one for the wildcard tag? */
        if (NULL == (request = find_request(&grp->requests, ORCM_PNP_TAG_WILDCARD))) {
            /* no matching requests */
            goto DEPART;
        }
    }

    if (NULL == (src = (orcm_pnp_source_t*)opal_pointer_array_get_item(&grp->members, sender->vpid))) {
        /* don't know this sender - add it */
        src = OBJ_NEW(orcm_pnp_source_t);
        src->name.jobid = sender->jobid;
        src->name.vpid = sender->vpid;
        opal_pointer_array_set_item(&group->members, src->name.vpid, src);
    }
    
    /* add the message to the src */
    
    /* get the current leader for this group */
    leader = orcm_leader.get_leader(grp);
    
    /* if the leader is wildcard, then deliver it */
    if (NULL == leader) {
        if (NULL != request->cbfunc) {
            /* release the thread prior to executing the callback
             * to avoid deadlock
             */
            OPAL_RELEASE_THREAD(&lock, &cond, &active);
            request->cbfunc(ORCM_SUCCESS, sender, tag, msg, count, cbdata);
            return;
        }
        /* if no callback provided, then just ignore the message */
        goto DEPART;
    }
    
#if 0
    /* see if the leader has failed */
    if (orcm_leader.has_leader_failed(grp, leader)) {
        /* leader has failed - get new one */
        if (ORCM_SUCCESS != (rc = orcm_leader.select_leader(grp))) {
            ORTE_ERROR_LOG(rc);
        }
    }
    
    /* if this data came from the leader, queue for delivery */
    if (NULL != grp->leader && src == grp->leader) {
        ORCM_PROCESS_PNP_IOVECS(&recvs, &recvlock, &recvcond, grp, src,
                                channel, tag, msg, count, cbdata);
    }
#endif
    
DEPART:
#if 0
    /* update the msg number */
    src->last_msg_num = seq_num;
#endif
    /* release the thread */
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
}

static void recv_input_buffers(int status,
                               orte_rmcast_channel_t channel,
                               orte_rmcast_tag_t tag,
                               orte_process_name_t *sender,
                               opal_buffer_t *buf, void *cbdata)
{
    orcm_pnp_group_t *group, *grp;
    orcm_pnp_source_t *src, *leader;
    orcm_pnp_pending_request_t *request;
    int i, rc;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received input buffer on channel %d from sender %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), channel,
                         ORTE_NAME_PRINT(sender)));
    
    /* protect against threading */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);
    
    /* the rmcast channel is associated with a group - get it */
    grp = NULL;
    for (i=0; i < groups.size; i++) {
        if (NULL == (group = (orcm_pnp_group_t*)opal_pointer_array_get_item(&groups, i))) {
            continue;
        }
        if (channel == group->channel) {
            grp = group;
            break;
        }
    }
    if (NULL == grp) {
        /* we don't know this channel yet - just ignore it */
        goto DEPART;
    }
    
    /* find the request object for this tag */
    if (NULL == (request = find_request(&grp->requests, tag))) {
        /* if there isn't one for this specific tag, is there one for the wildcard tag? */
        if (NULL == (request = find_request(&grp->requests, ORCM_PNP_TAG_WILDCARD))) {
            /* no matching requests */
            goto DEPART;
        }
    }
    
    if (NULL == (src = (orcm_pnp_source_t*)opal_pointer_array_get_item(&grp->members, sender->vpid))) {
        /* don't know this sender - add it */
        src = OBJ_NEW(orcm_pnp_source_t);
        src->name.jobid = sender->jobid;
        src->name.vpid = sender->vpid;
        opal_pointer_array_set_item(&group->members, src->name.vpid, src);
    }
    
    /* add the message to the src */
    
    /* get the current leader for this group */
    leader = orcm_leader.get_leader(grp);

    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received input buffer found sender %s in grp %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(sender), grp->app));
    
    /* if the leader is wildcard, then deliver it */
    if (NULL == leader) {        
        if (NULL != request->cbfunc_buf) {
            OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                                 "%s pnp:default:received input buffer wildcard leader - delivering msg",
                                 ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
            /* release the thread prior to executing the callback
             * to avoid deadlock
             */
            OPAL_RELEASE_THREAD(&lock, &cond, &active);
            request->cbfunc_buf(ORCM_SUCCESS, sender, tag, buf, cbdata);
            return;
        }
        /* if no callback provided, then just ignore the message */
        goto DEPART;
    }
    
#if 0
    /* see if the leader has failed */
    if (orcm_leader.has_leader_failed(grp, src, seq_num)) {
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:received input buffer leader failed",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
        
        /* leader has failed - get new one */
        if (ORCM_SUCCESS != (rc = orcm_leader.select_leader(grp))) {
            ORTE_ERROR_LOG(rc);
        }
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:received input buffer setting new leader to %s",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             ORTE_NAME_PRINT(&(grp->leader->name))));
    }
    
    /* if this data came from the leader, queue it for delivery */
    if (NULL != grp->leader && src == grp->leader) {
        ORCM_PROCESS_PNP_BUFFERS(&recvs, &recvlock, &recvcond, grp, src,
                                 channel, tag, buf, cbdata);
    }
#endif
    
DEPART:
#if 0
    /* update the msg number */
    src->last_msg_num = seq_num;
#endif
    /* release the thread */
    OPAL_RELEASE_THREAD(&lock, &cond, &active);
}

/* pack the common elements of an announcement message */
static int pack_announcement(opal_buffer_t *buf, orte_process_name_t *sender)
{
    int ret;
    
    /* if we haven't registered an app-triplet yet, then we can't announce */
    if (NULL == my_app) {
        return ORCM_ERR_NOT_AVAILABLE;
    }
    
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, ORTE_PROC_MY_NAME, 1, ORTE_NAME))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &my_app, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &my_version, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &my_release, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }
    /* pack my output channel */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &my_channel, 1, ORTE_RMCAST_CHANNEL_T))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }
    /* pack my node */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, &orte_process_info.nodename, 1, OPAL_STRING))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }
    
    /* tell everyone we are responding to an announcement
     * so they don't respond back
     */
    if (ORCM_SUCCESS != (ret = opal_dss.pack(buf, sender, 1, ORTE_NAME))) {
        ORTE_ERROR_LOG(ret);
        return ret;
    }        
    
    if (ORCM_PROC_IS_DAEMON || ORCM_PROC_IS_MASTER) {
        /* if we are a daemon or the master, include our system info */
        /* get our local resources */
        char *keys[] = {
            OPAL_SYSINFO_CPU_TYPE,
            OPAL_SYSINFO_CPU_MODEL,
            OPAL_SYSINFO_NUM_CPUS,
            OPAL_SYSINFO_MEM_SIZE,
            NULL
        };
        opal_list_t resources;
        opal_list_item_t *item;
        opal_sysinfo_value_t *info;
        int32_t num_values;
        
        /* include our node name */
        opal_dss.pack(buf, &orte_process_info.nodename, 1, OPAL_STRING);
        
        OBJ_CONSTRUCT(&resources, opal_list_t);
        opal_sysinfo.query(keys, &resources);
        /* add number of values to the buffer */
        num_values = opal_list_get_size(&resources);
        opal_dss.pack(buf, &num_values, 1, OPAL_INT32);
        /* add them to the buffer */
        while (NULL != (item = opal_list_remove_first(&resources))) {
            info = (opal_sysinfo_value_t*)item;
            opal_dss.pack(buf, &info->key, 1, OPAL_STRING);
            opal_dss.pack(buf, &info->type, 1, OPAL_DATA_TYPE_T);
            if (OPAL_INT64 == info->type) {
                opal_dss.pack(buf, &(info->data.i64), 1, OPAL_INT64);
            } else if (OPAL_STRING == info->type) {
                opal_dss.pack(buf, &(info->data.str), 1, OPAL_STRING);
            }
            OBJ_RELEASE(info);
        }
        OBJ_DESTRUCT(&resources);
    }

    return ORCM_SUCCESS;
}

/* ORTE callback functions so we can map them to our own */
static void rmcast_callback_buffer(int status,
                                   orte_rmcast_channel_t channel,
                                   orte_rmcast_tag_t tag,
                                   orte_process_name_t *sender,
                                   opal_buffer_t *buf, void* cbdata)
{
    orcm_pnp_send_t *send = (orcm_pnp_send_t*)cbdata;
    
    if (NULL != send->cbfunc_buf) {
        send->cbfunc_buf(status, ORTE_PROC_MY_NAME, send->tag, send->buffer, send->cbdata);
    }
    OBJ_RELEASE(send);
}

static void rmcast_callback(int status,
                            orte_rmcast_channel_t channel,
                            orte_rmcast_tag_t tag,
                            orte_process_name_t *sender,
                            struct iovec *msg, int count, void* cbdata)
{
    orcm_pnp_send_t *send = (orcm_pnp_send_t*)cbdata;
    
    if (NULL != send->cbfunc) {
        send->cbfunc(status, ORTE_PROC_MY_NAME, send->tag, send->msg, send->count, send->cbdata);
    }    
    OBJ_RELEASE(send);
}

static void rml_callback_buffer(int status,
                                orte_process_name_t* sender,
                                opal_buffer_t* buffer,
                                orte_rml_tag_t tag,
                                void* cbdata)
{
    orcm_pnp_send_t *send = (orcm_pnp_send_t*)cbdata;
    
    /* release the scratch buffer */
    OBJ_RELEASE(buffer);
    /* do any required callbacks */
    if (NULL != send->cbfunc_buf) {
        send->cbfunc_buf(status, ORTE_PROC_MY_NAME, send->tag, send->buffer, send->cbdata);
    }
    OBJ_RELEASE(send);
}

static void recv_direct_msgs(int status, orte_process_name_t* sender,
                             opal_buffer_t* buffer, orte_rml_tag_t tg,
                             void* cbdata)
{
    int rc;
    int32_t n, i, sz, iovec_count;
    int8_t flag;
    orcm_pnp_tag_t tag;
    struct iovec *iovec_array;
    orcm_pnp_group_t *group, *grp;
    orcm_pnp_source_t *source, *src;
    opal_list_item_t *item;
    orte_rmcast_channel_t channel;
    orcm_pnp_pending_request_t *request;

    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default recvd direct msg from %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         ORTE_NAME_PRINT(sender)));
    
    /* protect against threading */
    OPAL_ACQUIRE_THREAD(&lock, &cond, &active);
    
    /* unpack the channel */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buffer, &channel, &n, ORTE_RMCAST_CHANNEL_T))) {
        ORTE_ERROR_LOG(rc);
        goto CLEANUP;
    }
    
    /* locate this sender on our list */
    grp = NULL;
    for (i=0; i < groups.size; i++) {
        if (NULL == (group = (orcm_pnp_group_t*)opal_pointer_array_get_item(&groups, i))) {
            continue;
        }
        if (channel == group->channel) {
            grp = group;
            break;
        }
    }
    if (NULL == grp) {
        /* we don't know this channel yet - just ignore it */
        goto CLEANUP;
    }
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:received input buffer found grp",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME)));
    
    if (NULL == (src = (orcm_pnp_source_t*)opal_pointer_array_get_item(&grp->members, sender->vpid))) {
        /* don't know this sender - add it */
        src = OBJ_NEW(orcm_pnp_source_t);
        src->name.jobid = sender->jobid;
        src->name.vpid = sender->vpid;
        opal_pointer_array_set_item(&group->members, src->name.vpid, src);
    }
    
    /* unpack the flag */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buffer, &flag, &n, OPAL_INT8))) {
        goto CLEANUP;
    }
    
    /* unpack the intended tag for this message */
    n=1;
    if (ORCM_SUCCESS != (rc = opal_dss.unpack(buffer, &tag, &n, ORCM_PNP_TAG_T))) {
        ORTE_ERROR_LOG(rc);
        goto CLEANUP;
    }
    
    /* find the request object for this tag */
    if (NULL == (request = find_request(&grp->requests, tag))) {
        /* if there isn't one for this specific tag, is there one for the wildcard tag? */
        if (NULL == (request = find_request(&grp->requests, ORCM_PNP_TAG_WILDCARD))) {
            /* no matching requests */
            goto CLEANUP;
        }
    }
    
    if (1 == flag && NULL != request->cbfunc_buf) {
        /* release the thread prior to executing the callback
         * to avoid deadlock
         */
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        /* deliver the message */
        request->cbfunc_buf(ORCM_SUCCESS, sender, tag, buffer, NULL);
        goto DEPART;
    } else if (0 == flag && NULL != request->cbfunc) {
        /* iovecs included - get the number of iovecs in the buffer */
        n=1;
        if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &iovec_count, &n, OPAL_INT32))) {
            ORTE_ERROR_LOG(rc);
            goto CLEANUP;
        }
        /* malloc the required space */
        iovec_array = (struct iovec *)malloc(iovec_count * sizeof(struct iovec));
        /* unpack the iovecs */
        for (i=0; i < iovec_count; i++) {
            /* unpack the number of bytes in this iovec */
            n=1;
            if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &sz, &n, OPAL_INT32))) {
                ORTE_ERROR_LOG(rc);
                goto CLEANUP;
            }
            if (0 < sz) {
                /* allocate the space */
                iovec_array[i].iov_base = (uint8_t*)malloc(sz);
                iovec_array[i].iov_len = sz;
                /* unpack the data */
                n=sz;
                if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, iovec_array[i].iov_base, &n, OPAL_UINT8))) {
                    ORTE_ERROR_LOG(rc);
                    goto CLEANUP;
                }                    
            }
        }
        /* release the thread prior to executing the callback
         * to avoid deadlock
         */
        OPAL_RELEASE_THREAD(&lock, &cond, &active);
        /* deliver the message */
        request->cbfunc(ORCM_SUCCESS, sender, tag, iovec_array, iovec_count, NULL);
        goto DEPART;
    }
    
CLEANUP:
    /* release the thread */
    OPAL_RELEASE_THREAD(&lock, &cond, &active);

DEPART:
    /* reissue the recv */
    if (ORTE_SUCCESS != (rc = orte_rml.recv_buffer_nb(ORTE_NAME_WILDCARD,
                                                      ORTE_RML_TAG_MULTICAST_DIRECT,
                                                      ORTE_RML_NON_PERSISTENT,
                                                      recv_direct_msgs,
                                                      NULL))) {
        ORTE_ERROR_LOG(rc);
    }
    
}

static orcm_pnp_channel_tracker_t* get_channel(char *app,
                                               char *version,
                                               char *release)
{
    int i, j;
    orcm_pnp_channel_tracker_t *tracker;
    orcm_pnp_group_t *group;
    orcm_pnp_pending_request_t *request, *req;
    opal_list_item_t *itm2;
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:get_channel app %s version %s release %s",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         (NULL == app) ? "NULL" : app,
                         (NULL == version) ? "NULL" : version,
                         (NULL == release) ? "NULL" : release));
    
    for (i=0; i < channels.size; i++) {
        if (NULL == (tracker = (orcm_pnp_channel_tracker_t*)opal_pointer_array_get_item(&channels, i))) {
            continue;
        }
        if ((NULL == app && NULL != tracker->app) ||
            (NULL != app && NULL == tracker->app)) {
            continue;
        }
        if (NULL != app && 0 != strcasecmp(app, tracker->app)) {
            continue;
        }
        if ((NULL == version && NULL != tracker->version) ||
            (NULL != version && NULL == tracker->version)) {
            continue;
        }
        if (NULL != version && 0 != strcasecmp(version, tracker->version)) {
            continue;
        }
        if ((NULL == release && NULL != tracker->release) ||
            (NULL != release && NULL == tracker->release)) {
            continue;
        }
        if (NULL != release && 0 != strcasecmp(release, tracker->release)) {
            continue;
        }
        /* if we get here, then we have a match */
        OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                             "%s pnp:default:get_channel match found for tracker channel %d",
                             ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                             tracker->channel));
        return tracker;
    }
    
    /* if we get here, then this triplet doesn't exist - create it */
    tracker = OBJ_NEW(orcm_pnp_channel_tracker_t);
    if (NULL != app) {
        tracker->app = strdup(app);
    }
    if (NULL != version) {
        tracker->version = strdup(version);
    }
    if (NULL != release) {
        tracker->release = strdup(release);
    }
    tracker->channel = my_pnp_channels++;
    i = opal_pointer_array_add(&channels, tracker);
    
    OPAL_OUTPUT_VERBOSE((2, orcm_pnp_base.output,
                         "%s pnp:default:get_channel created tracker channel %d status %d",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                         tracker->channel, i));

    /* check all known groups to see who might belong to this new channel */
    for (i=0; i < groups.size; i++) {
        if (NULL == (group = (orcm_pnp_group_t*)opal_pointer_array_get_item(&groups, i))) {
            continue;
        }
        if (NULL != app && 0 != strcasecmp(app, group->app)) {
            continue;
        }
        if (NULL != version && 0 != strcasecmp(group->version, version)) {
            continue;
        }
        if (NULL != release && 0 != strcasecmp(group->release, release)) {
            continue;
        }
        /* have a match - add the group */
        OBJ_RETAIN(group);
        opal_pointer_array_add(&tracker->groups, group);
        /* add any pending requests associated with this channel */
        for (itm2 = opal_list_get_first(&group->requests);
             itm2 != opal_list_get_end(&group->requests);
             itm2 = opal_list_get_next(itm2)) {
            req = (orcm_pnp_pending_request_t*)itm2;
            if (NULL != find_request(&tracker->requests, req->tag)) {
                /* already assigned */
                continue;
            }
            /* add it */
            request = OBJ_NEW(orcm_pnp_pending_request_t);
            request->tag = req->tag;
            request->cbfunc = req->cbfunc;
            request->cbfunc_buf = req->cbfunc_buf;
            opal_list_append(&tracker->requests, &request->super);
        }
    }
    
    return tracker;
}

static orcm_pnp_channel_tracker_t* find_channel(orcm_pnp_channel_t channel)
{
    orcm_pnp_channel_tracker_t *tracker;
    int i, j, ret;
    
    /* bozo check */
    if (ORCM_PNP_INVALID_CHANNEL == channel) {
        /* throw bits on floor */
        return NULL;
    }
    
    for (i=0; i < channels.size; i++) {
        if (NULL == (tracker = (orcm_pnp_channel_tracker_t*)opal_pointer_array_get_item(&channels, i))) {
            continue;
        }
        if (channel == tracker->channel) {
            /* found it! */
            return tracker;
        }
    }
    
    /* get here if we don't have it */
    return NULL;
}

static void setup_recv_request(orcm_pnp_channel_tracker_t *tracker,
                               orcm_pnp_tag_t tag,
                               orcm_pnp_callback_fn_t cbfunc,
                               orcm_pnp_callback_buffer_fn_t cbfunc_buf)
{
    orcm_pnp_pending_request_t *request;
    orcm_pnp_group_t *group;
    int i;
    
    if (NULL == (request = find_request(&tracker->requests, tag))) {
        request = OBJ_NEW(orcm_pnp_pending_request_t);
        request->tag = tag;
        opal_list_append(&tracker->requests, &request->super);
    }
    
    /* setup the callback functions - since the request may
     * be pre-existing, we need to be careful that we only
     * update the functions that were provided
     */
    if (NULL != cbfunc) {
        request->cbfunc = cbfunc;
    }
    if (NULL != cbfunc_buf) {
        request->cbfunc_buf = cbfunc_buf;
    }
    
    /* set it up for the associated groups */
    for (i=0; i < tracker->groups.size; i++) {
        if (NULL == (group = (orcm_pnp_group_t*)opal_pointer_array_get_item(&tracker->groups, i))) {
            continue;
        }
        if (NULL != find_request(&group->requests, request->tag)) {
            /* already assigned */
            continue;
        }
        /* add it - since this is new, we can just use
         * whatever cbfuncs that were given
         */
        request = OBJ_NEW(orcm_pnp_pending_request_t);
        request->tag = tag;
        request->cbfunc = cbfunc;
        request->cbfunc_buf = cbfunc_buf;
        opal_list_append(&group->requests, &request->super);
    }
}

static orcm_pnp_pending_request_t* find_request(opal_list_t *list,
                                                orcm_pnp_tag_t tag)
{
    orcm_pnp_pending_request_t *req;
    opal_list_item_t *item;
    
    for (item = opal_list_get_first(list);
         item != opal_list_get_end(list);
         item = opal_list_get_next(item)) {
        req = (orcm_pnp_pending_request_t*)item;
        
        if (tag == req->tag) {
            return req;
        }
    }
    
    return NULL;
}
