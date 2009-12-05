/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

/**
 * @fie
 * Open Cluster Manager support tool
 *
 */

/* add the openrcm definitions */
#include "../runtime/runtime.h"

#include "orte_config.h"
#include "orte/constants.h"

#include <stdio.h>
#include <errno.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif  /*  HAVE_STDLIB_H */
#ifdef HAVE_SIGNAL_H
#include <signal.h>
#endif  /*  HAVE_SIGNAL_H */
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif  /* HAVE_SYS_STAT_H */
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif  /* HAVE_SYS_TYPES_H */
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif  /* HAVE_SYS_WAIT_H */
#ifdef HAVE_STRING_H
#include <string.h>
#endif  /* HAVE_STRING_H */
#ifdef HAVE_DIRENT_H
#include <dirent.h>
#endif  /* HAVE_DIRENT_H */

#include "opal/event/event.h"
#include "opal/util/cmd_line.h"
#include "opal/util/argv.h"
#include "opal/util/opal_environ.h"
#include "opal/util/path.h"
#include "opal/mca/base/base.h"
#include "opal/mca/base/mca_base_param.h"
#include "opal/runtime/opal.h"
#include "opal/mca/installdirs/installdirs.h"

#include "orte/runtime/runtime.h"
#include "orte/util/show_help.h"
#include "orte/util/parse_options.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/runtime/orte_globals.h"
#include "orte/mca/rml/rml.h"
#include "orte/util/name_fns.h"

/*****************************************
 * Global Vars for Command line Arguments
 *****************************************/
static struct {
    bool help;
    char *config_file;
    int num_procs;
    char *hosts;
    bool constrained;
} my_globals;

opal_cmd_line_init_t cmd_line_opts[] = {
    { NULL, NULL, NULL, 'h', NULL, "help", 0,
      &my_globals.help, OPAL_CMD_LINE_TYPE_BOOL,
      "Print help message" },

    { NULL, NULL, NULL, '\0', "boot", "boot", 1,
      &my_globals.config_file, OPAL_CMD_LINE_TYPE_STRING,
      "Name of file containing a configuration to be started" },
    
    { NULL, NULL, NULL, 'n', "np", "np", 1,
      &my_globals.num_procs, OPAL_CMD_LINE_TYPE_INT,
      "Number of instances to start" },

    { NULL, NULL, NULL, 'H', "host", "host", 1,
      &my_globals.hosts, OPAL_CMD_LINE_TYPE_STRING,
      "Comma-delimited list of hosts upon which procs are to be initially started" },

    { NULL, NULL, NULL, '\0', "constrain", "constrain", 0,
      &my_globals.constrained, OPAL_CMD_LINE_TYPE_BOOL,
      "Constrain processes to run solely on the specified hosts, even upon restart from failure" },

/* End of list */
    { NULL, NULL, NULL, 
      '\0', NULL, NULL, 
      0,
      NULL, OPAL_CMD_LINE_TYPE_NULL,
      NULL }
};

/*
 * Local variables & functions
 */
static void abort_exit_callback(int fd, short flags, void *arg);
static struct opal_event term_handler;
static struct opal_event int_handler;
static opal_event_t *orted_exit_event;

#define CM_MAX_LINE_LENGTH  1024

static char *cm_getline(FILE *fp)
{
    char *ret, *buff;
    char input[CM_MAX_LINE_LENGTH];
    
retry:
    ret = fgets(input, CM_MAX_LINE_LENGTH, fp);
    if (NULL != ret) {
        if ('#' == input[0]) {
            /* ignore this line - it is a comment */
            goto retry;
        }
        input[strlen(input)-1] = '\0';  /* remove newline */
        buff = strdup(input);
        return buff;
    }
    
    return NULL;
}


int main(int argc, char *argv[])
{
    int32_t ret, i, num_apps;
    opal_cmd_line_t cmd_line;
    FILE *fp;
    char *cmd;
    char **inpt, **xfer;
    opal_buffer_t buf;
    int count;
    char cwd[OPAL_PATH_MAX];
    char *app;
    orcm_tool_cmd_t flag = OPENRCM_TOOL_START_CMD;
    int8_t constrain;
    
    /***************
     * Initialize
     ***************/

    /*
     * Make sure to init util before parse_args
     * to ensure installdirs is setup properly
     * before calling mca_base_open();
     */
    if( ORTE_SUCCESS != (ret = orcm_init_util()) ) {
        return ret;
    }
    
    /* initialize the globals */
    my_globals.help = false;
    my_globals.config_file = NULL;
    my_globals.num_procs = 0;
    my_globals.hosts = NULL;
    my_globals.constrained = false;
    
    /* Parse the command line options */
    opal_cmd_line_create(&cmd_line, cmd_line_opts);
    
    mca_base_open();
    mca_base_cmd_line_setup(&cmd_line);
    ret = opal_cmd_line_parse(&cmd_line, true, argc, argv);
    
    /* extract the MCA/GMCA params */
    mca_base_cmd_line_process_args(&cmd_line, &environ, &environ);
    
    /**
     * Now start parsing our specific arguments
     */
    if (OPAL_SUCCESS != ret || my_globals.help) {
        char *args = NULL;
        args = opal_cmd_line_get_usage_msg(&cmd_line);
        orte_show_help("help-orcm-start.txt", "usage", true, args);
        free(args);
        return ORTE_ERROR;
    }
    
    /* get the current working directory */
    if (OPAL_SUCCESS != opal_getcwd(cwd, sizeof(cwd))) {
        fprintf(stderr, "failed to get cwd\n");
        return ORTE_ERR_NOT_FOUND;
    }
    
    /* setup the exit triggers */
    OBJ_CONSTRUCT(&orte_exit, orte_trigger_event_t);
    OBJ_CONSTRUCT(&orteds_exit, orte_trigger_event_t);

    /***************************
     * We need all of OPAL and ORTE - this will
     * automatically connect us to the CM
     ***************************/
    if (ORTE_SUCCESS != orcm_init(OPENRCM_TOOL)) {
        orte_finalize();
        return 1;
    }

    /* setup the buffer to send our cmd */
    OBJ_CONSTRUCT(&buf, opal_buffer_t);

    /* load the start cmd */
    opal_dss.pack(&buf, &flag, 1, OPENRCM_TOOL_CMD_T);
    
    /* if we have a config file, read it */
    if (NULL != my_globals.config_file) {
        fp = fopen(my_globals.config_file, "r");
        if (NULL == fp) {
            /* didn't find it or can't read it */
            ret = 1;
            goto cleanup;
        }
        num_apps = 0;
        while (NULL != (cmd = cm_getline(fp))) {
            inpt = opal_argv_split(cmd, ' ');
            free(cmd);
            /* get the absolute path */
            if (NULL == (app = opal_find_absolute_path(inpt[0]))) {
                fprintf(stderr, "App %s could not be found - try changing path\n", inpt[0]);
                continue;
            }
            xfer = NULL;
            opal_argv_append_nosize(&xfer, app);
            for (i=1; NULL != inpt[i]; i++) {
                opal_argv_append_nosize(&xfer, inpt[i]);
            }
            opal_argv_free(inpt);
            cmd = opal_argv_join(xfer, ' ');
            opal_argv_free(xfer);
            opal_dss.pack(&buf, &cmd, 1, OPAL_STRING);
            free(cmd);
            ++num_apps;
        }
        if (0 == num_apps) {
            goto cleanup;
        }
    } else {
        /* pack the number of instances to start */
        num_apps = my_globals.num_procs;
        opal_dss.pack(&buf, &num_apps, 1, OPAL_INT32);
        
        /* pack the starting hosts - okay to pack a NULL string */
        opal_dss.pack(&buf, &my_globals.hosts, 1, OPAL_STRING);
        
        /* pack the constraining flag */
        if (my_globals.constrained) {
            constrain = 1;
        } else {
            constrain = 0;
        }
        opal_dss.pack(&buf, &constrain, 1, OPAL_INT8);
        
        /* get the things to start */
        inpt = NULL;
        opal_cmd_line_get_tail(&cmd_line, &count, &inpt);
        
        /* get the absolute path */
        if (NULL == (app = opal_find_absolute_path(inpt[0]))) {
            fprintf(stderr, "App %s could not be found - try changing path\n", inpt[0]);
            goto cleanup;
        }
        xfer = NULL;
        opal_argv_append_nosize(&xfer, app);
        for (i=1; NULL != inpt[i]; i++) {
            opal_argv_append_nosize(&xfer, inpt[i]);
        }
        opal_argv_free(inpt);
        cmd = opal_argv_join(xfer, ' ');
        opal_argv_free(xfer);
        opal_dss.pack(&buf, &cmd, 1, OPAL_STRING);
        free(cmd);
    }
    
    if (0 > (ret = orte_rml.send_buffer(ORTE_PROC_MY_HNP, &buf, ORTE_RML_TAG_TOOL, 0))) {
        ORTE_ERROR_LOG(ret);
    }
    OBJ_DESTRUCT(&buf);
    
    /* now wait for ack */
    OBJ_CONSTRUCT(&buf, opal_buffer_t);
    if (0 > (ret = orte_rml.recv_buffer(ORTE_NAME_WILDCARD, &buf, ORTE_RML_TAG_TOOL, 0))) {
        ORTE_ERROR_LOG(ret);
        goto cleanup;
    }
    i=1;
    opal_dss.unpack(&buf, &ret, &i, OPAL_INT32);
    if (ORTE_SUCCESS != ret) {
        ORTE_ERROR_LOG(ret);
    }
    OBJ_DESTRUCT(&buf);
    
    /***************
     * Cleanup
     ***************/
 cleanup:
    /* Remove the TERM and INT signal handlers */
    opal_signal_del(&term_handler);
    opal_signal_del(&int_handler);
    OBJ_DESTRUCT(&orte_exit);

    /* cleanup orte */
    
    orcm_finalize();

    return ret;
}

static void abort_exit_callback(int fd, short ign, void *arg)
{
    opal_list_item_t *item;
    int ret;
    
    /* Remove the TERM and INT signal handlers */
    opal_signal_del(&term_handler);
    opal_signal_del(&int_handler);
    OBJ_DESTRUCT(&orte_exit);

    orcm_finalize();
    exit(1);
}
