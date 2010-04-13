/*
 * Copyright (c) 2009-2010 Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef CFGI_TOOL_H
#define CFGI_TOOL_H

#include "openrcm.h"

/* Functions in the cfgi tool component */

int orcm_cfgi_tool_component_open(void);
int orcm_cfgi_tool_component_close(void);
int orcm_cfgi_tool_component_query(mca_base_module_2_0_0_t **module, int *priority);
int orcm_cfgi_tool_component_register(void);

typedef struct {
    orcm_cfgi_base_component_t super;
    char *tool;
} orcm_cfgi_tool_component_t;

extern orcm_cfgi_tool_component_t mca_cfgi_tool_component;
extern orcm_cfgi_base_module_t orcm_cfgi_tool_module;

#endif /* CFGI_TOOL_H */
