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

#include "runtime/runtime.h"

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
    int tmp;

    /* whether we are just testing the interface */
    mca_base_param_reg_int(c, "test_mode",
                           "Set test mode - just output received commands",
                           false, false, 0, &tmp);
    mca_orcm_cfgi_confd_component.test_mode = OPAL_INT_TO_BOOL(tmp);

    return ORCM_SUCCESS;
}

int orcm_cfgi_confd_component_close(void)
{
    return ORCM_SUCCESS;
}

int orcm_cfgi_confd_component_query(mca_base_module_t **module, int *priority)
{
    char *comp, **values;
    bool use;
    int i;

    if (ORCM_PROC_IS_DAEMON) {
        /* if not specifically requested, don't use this module */
        if (NULL == (comp = getenv("OMPI_MCA_orcm_cfgi"))) {
            goto donotuse;
        }
        values = opal_argv_split(comp, ',');
        use = false;
        for (i=0; NULL != values[i]; i++) {
            if (0 == strcmp("confd", values[i])) {
                use = true;
                break;
            }
        }
        if (!use) {
            goto donotuse;
        }
        *module = (mca_base_module_t*)&orcm_cfgi_confd_module;
        *priority = 100;
        return ORCM_SUCCESS;
    }

 donotuse:
    /* otherwise, cannot use this module */
    *priority = 0;
    *module = NULL;
    return ORCM_ERROR;
}

int orcm_cfgi_confd_component_register(void)
{
    return ORCM_SUCCESS;
}

