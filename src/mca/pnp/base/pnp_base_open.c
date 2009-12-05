/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "openrcm_config.h"
#include "include/constants.h"

#include "opal/class/opal_value_array.h"
#include "opal/util/output.h"
#include "opal/mca/mca.h"
#include "opal/mca/base/base.h"

#include "orte/mca/rmcast/rmcast_types.h"

#include "mca/pnp/pnp.h"
#include "mca/pnp/base/public.h"
#include "mca/pnp/base/private.h"
#include "mca/pnp/base/components.h"

const mca_base_component_t *orcm_pnp_base_components[] = {
    &mca_pnp_default_component.pnpc_version,
    NULL
};

/* instantiate the module */
orcm_pnp_base_module_t orcm_pnp = {
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

/* instantiate the globals */
orcm_pnp_base_t orcm_pnp_base;

int orcm_pnp_base_open(void)
{
    /* Debugging / verbose output.  Always have stream open, with
     verbose set by the mca open system... */
    orcm_pnp_base.output = opal_output_open(NULL);
    
    /* Open up all available components */
    if (ORCM_SUCCESS != 
        mca_base_components_open("pnp", orcm_pnp_base.output,
                                 orcm_pnp_base_components, 
                                 &orcm_pnp_base.opened, true)) {
            return ORCM_ERROR;
        }
    
    /* All done */
    return ORCM_SUCCESS;
}

/****    INSTANTIATE CLASSES    ****/
static void source_constructor(orcm_pnp_source_t *ptr)
{
    ptr->name.jobid = ORTE_JOBID_INVALID;
    ptr->name.vpid = ORTE_VPID_INVALID;
    ptr->failed = false;
    memset(ptr->msgs, 0, ORCM_PNP_MAX_MSGS*sizeof(opal_buffer_t*));
    ptr->start = 0;
    ptr->end = 0;
    ptr->last_msg_num = ORTE_RMCAST_SEQ_INVALID;
}
static void source_destructor(orcm_pnp_source_t *ptr)
{
    int i;
    
    for (i=0; i < ORCM_PNP_MAX_MSGS; i++) {
        if (NULL != ptr->msgs[i]) {
            OBJ_RELEASE(ptr->msgs[i]);
        }
    }
}
OBJ_CLASS_INSTANCE(orcm_pnp_source_t,
                   opal_list_item_t,
                   source_constructor,
                   source_destructor);

static void group_constructor(orcm_pnp_group_t *ptr)
{
    ptr->app = NULL;
    ptr->version = NULL;
    ptr->release = NULL;
    OBJ_CONSTRUCT(&ptr->channels, opal_value_array_t);
    opal_value_array_init(&ptr->channels, sizeof(orte_rmcast_channel_t));
    OBJ_CONSTRUCT(&ptr->members, opal_list_t);
    OBJ_CONSTRUCT(&ptr->requests, opal_list_t);
    ptr->leader = NULL;
}
static void group_destructor(orcm_pnp_group_t *ptr)
{
    opal_list_item_t *item;
    
    if (NULL != ptr->app) {
        free(ptr->app);
    }
    if (NULL != ptr->version) {
        free(ptr->version);
    }
    if (NULL != ptr->release) {
        free(ptr->release);
    }
    OBJ_DESTRUCT(&ptr->channels);
    while (NULL != (item = opal_list_remove_first(&ptr->members))) {
        OBJ_RELEASE(item);
    }
    OBJ_DESTRUCT(&ptr->members);
    while (NULL != (item = opal_list_remove_first(&ptr->requests))) {
        OBJ_RELEASE(item);
    }
    OBJ_DESTRUCT(&ptr->requests);
}
OBJ_CLASS_INSTANCE(orcm_pnp_group_t,
                   opal_list_item_t,
                   group_constructor,
                   group_destructor);

static void request_constructor(orcm_pnp_pending_request_t *ptr)
{
    ptr->tag = ORCM_PNP_TAG_WILDCARD;
    ptr->cbfunc = NULL;
    ptr->cbfunc_buf = NULL;
}
/* no destruct required here */
OBJ_CLASS_INSTANCE(orcm_pnp_pending_request_t,
                   opal_list_item_t,
                   request_constructor, NULL);

static void send_constructor(orcm_pnp_send_t *ptr)
{
    ptr->cbfunc = NULL;
    ptr->cbfunc_buf = NULL;
    ptr->cbdata = NULL;
}
/* no destruct required here */
OBJ_CLASS_INSTANCE(orcm_pnp_send_t,
                   opal_object_t,
                   send_constructor, NULL);
