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

#include "opal/util/output.h"

#include "mca/cfgi/cfgi.h"
#include "mca/cfgi/confd/cfgi_confd.h"

orcm_cfgi_confd_component_t mca_orcm_cfgi_confd_component = {
    {
        /* First, the mca_base_component_t struct containing meta
         information about the component itself */

        {
            ORCM_CFGI_BASE_VERSION_2_0_0,

            "confd",
            OPENRCM_MAJOR_VERSION,
            OPENRCM_MINOR_VERSION,
            OPENRCM_RELEASE_VERSION,
            orcm_cfgi_confd_component_open,
            orcm_cfgi_confd_component_close,
            orcm_cfgi_confd_component_query,
            orcm_cfgi_confd_component_register
        },
        {
            /* The component is checkpoint ready */
            MCA_BASE_METADATA_PARAM_CHECKPOINT
        }
    }
};

int orcm_cfgi_confd_component_open(void)
{
    mca_base_component_t *c = &mca_orcm_cfgi_confd_component.super.cfgic_version;

    /* check for confd name */
    mca_base_param_reg_string(c, "db_name",
                              "Name of the confd database to which we are to connect",
                              false, false, NULL, &mca_orcm_cfgi_confd_component.db);
    
    /* check for logfile name */
    mca_base_param_reg_string(c, "logfile",
                              "File in which to log transactions (default: NULL)",
                              false, false, NULL, &mca_orcm_cfgi_confd_component.logfile);
    
    /* check for mode */
    mca_base_param_reg_string(c, "mode",
                              "Operating mode (SILENT | DEBUG | TRACE [default])",
                              false, false, "TRACE", &mca_orcm_cfgi_confd_component.mode);

    /* set namespace */
    mca_base_param_reg_string(c, "namespace",
                              "Set namespace",
                              false, false, "confd-example", &mca_orcm_cfgi_confd_component.ns);

    return ORCM_SUCCESS;
}

int orcm_cfgi_confd_component_close(void)
{
    return ORCM_SUCCESS;
}

int orcm_cfgi_confd_component_query(mca_base_module_t **module, int *priority)
{
    if (NULL != mca_orcm_cfgi_confd_component.confd) {
        *module = (mca_base_module_t*)&orcm_cfgi_confd_module;
        *priority = 100;
        return ORCM_SUCCESS;
    }
    
    *module = NULL;
    *priority = 0;
    return ORCM_ERROR;
}

int orcm_cfgi_confd_component_register(void)
{
    return ORCM_SUCCESS;
}

