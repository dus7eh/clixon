#!/usr/bin/env bash
# Tests cpp compatibility with clixon

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

cfile=$dir/c++.cpp

cat<<EOF > $cfile
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/param.h>

#include <cligen/cligen.h>
#include <clixon/clixon.h>
#include <clixon/clixon_netconf.h>

/*! Plugin start
 * Called once everything has been initialized, right before
 * the main event loop is entered.
 */

clixon_plugin_api * clixon_plugin_init(clicon_handle h);

int plugin_start(clicon_handle h)
{
    return 0;
}

int plugin_exit(clicon_handle h)
{
    return 0;
}

class netconf_test
{
private:
    struct clixon_plugin_api api;
    plginit2_t       *ca_init;
    plgstart_t       *ca_start;
    plgexit_t        *ca_exit;

public:
    netconf_test(plginit2_t* init, plgstart_t* start, plgexit_t* exit, const char* str = "c++ netconf test") : api()
    {
        strcpy(api.ca_name, str);
        api.ca_init = clixon_plugin_init;
        api.ca_start = plugin_start;
        api.ca_exit = plugin_exit;
    }

    clixon_plugin_api* get_api(void)
    {
        return &api;
    }
};

static netconf_test api(clixon_plugin_init, plugin_start, plugin_exit);

/*! Local example netconf rpc callback
 */
int netconf_client_rpc(clicon_handle h,
		   cxobj        *xe,
		   cbuf         *cbret,
		   void         *arg,
		   void         *regarg)
{
    int    retval = -1;
    cxobj *x = NULL;
    char  *ns;

    /* get namespace from rpc name, return back in each output parameter */
    if ((ns = xml_find_type_value(xe, NULL, "xmlns", CX_ATTR)) == NULL)
    {
	      clicon_err(OE_XML, ENOENT, "No namespace given in rpc %s", xml_name(xe));
	      goto done;
    }
    cprintf(cbret, "<rpc-reply>");
    if (!xml_child_nr_type(xe, CX_ELMNT))
	      cprintf(cbret, "<ok/>");
    else
        while ((x = xml_child_each(xe, x, CX_ELMNT)) != NULL)
        {
            if (xmlns_set(x, NULL, ns) < 0)
                goto done;
            if (clicon_xml2cbuf(cbret, x, 0, 0, -1) < 0)
                goto done;
        }
    cprintf(cbret, "</rpc-reply>");
    retval = 0;
    done:
    return retval;
    return 0;
}

/*! Netconf plugin initialization
 * @param[in]  h    Clixon handle
 * @retval     NULL Error with clicon_err set
 * @retval     api  Pointer to API struct
 */
clixon_plugin_api* clixon_plugin_init(clicon_handle h)
{
    clicon_debug(1, "%s netconf", __FUNCTION__);
    /* Register local netconf rpc client (note not backend rpc client) */
    if (rpc_callback_register(h, netconf_client_rpc, NULL, "urn:example:clixon", "client-rpc") < 0)
	      return NULL;

    return api.get_api();
}

EOF

new "C++ compile"
expectpart "$($CXX -g -Wall -rdynamic -fPIC -shared $cfile -o c++.o)" 0 ""

rm -f c++.o
rm -rf $dir

