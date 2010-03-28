/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef PNP_BASE_PUBLIC_H
#define PNP_BASE_PUBLIC_H
#include "openrcm_config_private.h"

#include "mca/pnp/pnp.h"

BEGIN_C_DECLS

ORCM_DECLSPEC int orcm_pnp_base_open(void);
ORCM_DECLSPEC int orcm_pnp_base_select(void);
ORCM_DECLSPEC int orcm_pnp_base_close(void);

/* heartbeat support */
ORCM_DECLSPEC void orcm_pnp_base_start_heart(char *app, char *version, char *release);
ORCM_DECLSPEC void orcm_pnp_base_stop_heart(void);

ORCM_DECLSPEC extern const mca_base_component_t *orcm_pnp_base_components[];

END_C_DECLS

#endif
