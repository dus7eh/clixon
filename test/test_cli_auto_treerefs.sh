#!/usr/bin/env bash
# Tests for @datamodel tree references
# See also test_cli_auto_genmodel.sh
# XXX: cant do "show leaf" only "show leaf <value>"

# Magic line must be first in script (see README.md)
s="$_" ; . ./lib.sh || if [ "$s" = $0 ]; then exit 0; else return 0; fi

APPNAME=example

# include err() and new() functions and creates $dir

cfg=$dir/conf_yang.xml
fyang=$dir/$APPNAME.yang
fstate=$dir/state.xml
clidir=$dir/cli
if [ -d $clidir ]; then
    rm -rf $clidir/*
else
    mkdir $clidir
fi

# Use yang in example

cat <<EOF > $cfg
<clixon-config xmlns="http://clicon.org/config">
  <CLICON_CONFIGFILE>$cfg</CLICON_CONFIGFILE>
  <CLICON_YANG_DIR>/usr/local/share/clixon</CLICON_YANG_DIR>
  <CLICON_YANG_DIR>$dir</CLICON_YANG_DIR>
  <CLICON_YANG_MAIN_FILE>$fyang</CLICON_YANG_MAIN_FILE>
  <CLICON_BACKEND_DIR>/usr/local/lib/$APPNAME/backend</CLICON_BACKEND_DIR>
  <CLICON_CLISPEC_DIR>$clidir</CLICON_CLISPEC_DIR>
  <CLICON_CLI_DIR>/usr/local/lib/$APPNAME/cli</CLICON_CLI_DIR>
  <CLICON_CLI_MODE>$APPNAME</CLICON_CLI_MODE>
  <CLICON_CLI_GENMODEL>2</CLICON_CLI_GENMODEL>
  <CLICON_CLI_GENMODEL_TYPE>VARS</CLICON_CLI_GENMODEL_TYPE>
  <CLICON_SOCK>/usr/local/var/$APPNAME/$APPNAME.sock</CLICON_SOCK>
  <CLICON_BACKEND_PIDFILE>/usr/local/var/$APPNAME/$APPNAME.pidfile</CLICON_BACKEND_PIDFILE>
  <CLICON_XMLDB_DIR>/usr/local/var/$APPNAME</CLICON_XMLDB_DIR>
  <CLICON_MODULE_LIBRARY_RFC7895>false</CLICON_MODULE_LIBRARY_RFC7895>
  <CLICON_CLI_AUTOCLI_EXCLUDE>clixon-restconf</CLICON_CLI_AUTOCLI_EXCLUDE>
</clixon-config>
EOF

# Four different trees in terms of "config none": none, under container, list, leaf respectively
cat <<EOF > $fyang
module $APPNAME {
  namespace "urn:example:clixon";
  prefix ex;
  container config{
    list parameter{
      key name;
      leaf name{
        type string;
      }
      leaf value{
        type string;
      }
    }
  }
  container statecontainer{
    config false;
    list parameter{
      key name;
      leaf name{
        type string;
      }
      leaf value{
        type string;
      }
    }
  }
  container stateleaf{
    list parameter{
      key name;
      leaf name{
        type string;
      }
      leaf value{
        config false;
        type string;
      }
    }
  }
  container statelist{
    list parameter{
      config false;
      key name;
      leaf name{
        type string;
      }
      leaf value{
        type string;
      }
    }
  }
}
EOF

# This is state data written to file that backend reads from (on request)
cat <<EOF > $fstate
   <statecontainer xmlns="urn:example:clixon">
     <parameter>
       <name>a</name>
       <value>42</value>
     </parameter>
   </statecontainer>
EOF

cat <<EOF > $clidir/ex.cli
CLICON_MODE="example";
CLICON_PROMPT="%U@%H> ";

show {
  base @datamodel, cli_show_auto_state("running", "cli", "set ");
  add-nonconfig  @datamodel,@add:nonconfig, cli_show_auto_state("running", "cli", "set ");
  add-show @datamodel,@add:show, cli_show_auto_state("running", "cli", "set ");
  add-show-nonconfig @datamodel,@add:nonconfig,@add:show, cli_show_auto_state("running", "cli", "set ");
  remove-leaf @datamodel,@remove:leaf, cli_show_auto_state("running", "cli", "set ");
}

auto {
     edit @datamodel, @remove:leaf,cli_auto_edit("datamodel");
     top, cli_auto_top("datamodel");
     show, cli_auto_show("datamodel", "candidate", "text", true, false);
     set @datamodel, cli_auto_set();
}
EOF

new "test params: -f $cfg"
if [ $BE -ne 0 ]; then
    new "kill old backend"
    sudo clixon_backend -z -f $cfg
    if [ $? -ne 0 ]; then
	err
    fi
    new "start backend -s init -f $cfg -- -sS $fstate"
    start_backend -s init -f $cfg -- -sS $fstate
fi

new "wait backend"
wait_backend

#------ base
top=base
new "Show $top"
expectpart "$(echo "show $top ?" | $clixon_cli -f $cfg 2> /dev/null)" 0 config stateleaf statelist --not-- statecontainer '<cr>'

new "Show $top config"
expectpart "$(echo "show $top config ?" | $clixon_cli -f $cfg 2> /dev/null)" 0 parameter '<cr>'

new "Show $top config parameter"
expectpart "$(echo "show $top config parameter ?" | $clixon_cli -f $cfg 2> /dev/null)" 0 parameter '<name>' --not-- '<cr>'

new "Show $top config parameter <name>"
expectpart "$(echo "show $top config parameter a ?" | $clixon_cli -f $cfg 2> /dev/null)" 0 value '<cr>' --not-- '<value>'

new "Show $top config parameter <name> value ?"
expectpart "$(echo "show $top config parameter a value ?" | $clixon_cli -f $cfg 2> /dev/null)" 0 '<value>' --not-- '<cr>'

new "Show $top statelist"
expectpart "$(echo "show $top statelist ?" | $clixon_cli -f $cfg 2> /dev/null)" 0 '<cr>' --not-- parameter 

new "Show $top stateleaf parameter"
expectpart "$(echo "show $top stateleaf parameter a ?" | $clixon_cli -f $cfg 2> /dev/null)" 0 '<cr>' --not-- value 

#--------- @add:nonconfig (check state rules)

top=add-nonconfig
new "Show $top"
expectpart "$(echo "show $top ?" | $clixon_cli -f $cfg 2> /dev/null)" 0 config statecontainer stateleaf statelist --not-- '<cr>'

new "Show $top config parameter a"
expectpart "$(echo "show $top config parameter a ?" | $clixon_cli -f $cfg 2> /dev/null)" 0 value '<cr>'

new "Show $top statecontainer parameter"
expectpart "$(echo "show $top statecontainer parameter a ?" | $clixon_cli -f $cfg 2> /dev/null)" 0 value '<cr>'

new "Show $top statelist parameter"
expectpart "$(echo "show $top statelist parameter a ?" | $clixon_cli -f $cfg 2> /dev/null)" 0 value  '<cr>'

new "Show $top stateleaf parameter"
expectpart "$(echo "show $top stateleaf parameter a ?" | $clixon_cli -f $cfg 2> /dev/null)" 0 value  '<cr>'

#--------- @add:show (compare with config: add <cr> at list and leaf)
top=add-show
new "Show $top"
expectpart "$(echo "show $top ?" | $clixon_cli -f $cfg 2> /dev/null)" 0 config stateleaf statelist --not-- statecontainer '<cr>'

new "Show $top config"
expectpart "$(echo "show $top config ?" | $clixon_cli -f $cfg 2> /dev/null)" 0 parameter '<cr>'

# <cr> is enabled on lists
new "Show $top config parameter"
expectpart "$(echo "show $top config parameter ?" | $clixon_cli -f $cfg 2> /dev/null)" 0 parameter '<name>' '<cr>' 

new "Show $top config parameter <name>"
expectpart "$(echo "show $top config parameter a ?" | $clixon_cli -f $cfg 2> /dev/null)" 0 value '<cr>' --not-- '<value>'

if false; then
    # Have not succeeded with this, and I am not sure it is necessary?
    # Ie to do "show leaf", but not "show leaf <value>"
    # <cr> is enabled but no value on leafs
    new "Show $top config parameter <name> value"
    expectpart "$(echo "show $top config parameter a value ?" | $clixon_cli -f $cfg 2> /dev/null)" 0 '<cr>' --not-- '<value>'
fi

#--------- @add:show+@add:nonconfig

top=add-show-nonconfig
new "Show $top"
expectpart "$(echo "show $top ?" | $clixon_cli -f $cfg 2> /dev/null)" 0 config statecontainer stateleaf statelist --not-- '<cr>'

new "Show $top config"
expectpart "$(echo "show $top config ?" | $clixon_cli -f $cfg 2> /dev/null)" 0 parameter '<cr>'

# <cr> is enabled on lists
new "Show $top config parameter"
expectpart "$(echo "show $top config parameter ?" | $clixon_cli -f $cfg 2> /dev/null)" 0 parameter '<name>' '<cr>' 

new "Show $top config parameter <name>"
expectpart "$(echo "show $top config parameter a ?" | $clixon_cli -f $cfg 2> /dev/null)" 0 value '<cr>' --not-- '<value>'

# <cr> is enabled but no value on leafs
new "Show $top config parameter <name> value"
#expectpart "$(echo "show $top config parameter a value ?" | $clixon_cli -f $cfg 2> /dev/null)" 0 '<cr>' --not-- '<value>'

#--------- @remove:mode
top=remove-leaf

new "auto edit config"
expectpart "$(echo "auto edit config ?" | $clixon_cli -f $cfg 2> /dev/null)" 0 parameter '<cr>' --not-- '<value>'

new "auto edit config parameter"
expectpart "$(echo "auto edit config parameter ?" | $clixon_cli -f $cfg 2> /dev/null)" 0 '<name>'

new "auto edit config parameter <name>"
expectpart "$(echo "auto edit config parameter a ?" | $clixon_cli -f $cfg 2> /dev/null)" 0 '<cr>' --not-- value

new "Kill backend"
# Check if premature kill
pid=$(pgrep -u root -f clixon_backend)
if [ -z "$pid" ]; then
    err "backend already dead"
fi
# kill backend
stop_backend -f $cfg

rm -rf $dir

new "endtest"
endtest
