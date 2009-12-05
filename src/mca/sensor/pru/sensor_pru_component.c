/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
 *
  * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "openrcm_config.h"
#include "constants.h"

#include "opal/mca/base/base.h"
#include "opal/util/output.h"
#include "opal/mca/base/mca_base_param.h"
#include "opal/class/opal_pointer_array.h"

#include "orte/util/proc_info.h"
#include "orte/util/show_help.h"

#include "mca/sensor/base/public.h"
#include "sensor_pru.h"

/*
 * Local functions
 */

static int orcm_sensor_pru_open(void);
static int orcm_sensor_pru_close(void);
static int orcm_sensor_pru_query(mca_base_module_t **module, int *priority);

orcm_sensor_pru_component_t mca_sensor_pru_component = {
    {
        {
            ORCM_SENSOR_BASE_VERSION_1_0_0,
            
            "pru", /* MCA component name */
            OPENRCM_MAJOR_VERSION,  /* MCA component major version */
            OPENRCM_MINOR_VERSION,  /* MCA component minor version */
            OPENRCM_RELEASE_VERSION,  /* MCA component release version */
            orcm_sensor_pru_open,  /* component open  */
            orcm_sensor_pru_close, /* component close */
            orcm_sensor_pru_query  /* component query */
        },
        {
            /* The component is checkpoint ready */
            MCA_BASE_METADATA_PARAM_CHECKPOINT
        }
    }
};


/**
  * component open/close/init function
  */
static int orcm_sensor_pru_open(void)
{
    mca_base_component_t *c = &mca_sensor_pru_component.super.base_version;
    int tmp;

    /* lookup parameters */
    mca_base_param_reg_int(c, "sample_rate",
                           "Sample rate in seconds (default=10)",
                           false, false, 10,  &mca_sensor_pru_component.sample_rate);
    
    mca_base_param_reg_int(c, "memory_limit",
                           "Max virtual memory size in GBytes (default=10)",
                           false, false, 10,  &tmp);
    if (tmp < 0) {
        opal_output(0, "Illegal value %d - must be > 0", tmp);
        return ORCM_ERR_FATAL;
    }
    mca_sensor_pru_component.memory_limit = tmp;
    
    return ORCM_SUCCESS;
}


static int orcm_sensor_pru_query(mca_base_module_t **module, int *priority)
{    
    *priority = 0;  /* select only if specified */
    *module = (mca_base_module_t *)&orcm_sensor_pru_module;
    
    return ORCM_SUCCESS;
}

/**
 *  Close all subsystems.
 */

static int orcm_sensor_pru_close(void)
{
    return ORCM_SUCCESS;
}
