#!/usr/bin/env bash

APPNAME="voter"

# find voltdb binaries in either installation or distribution directory.
if [ -n "$(which voltdb 2> /dev/null)" ]; then
    VOLTDB_BIN=$(dirname "$(which voltdb)")
else
    VOLTDB_BIN="$(dirname $(dirname $(pwd)))/bin"
    echo "The VoltDB scripts are not in your PATH."
    echo "For ease of use, add the VoltDB bin directory: "
    echo
    echo $VOLTDB_BIN
    echo
    echo "to your PATH."
    echo
fi
# installation layout has all libraries in $VOLTDB_ROOT/lib/voltdb
if [ -d "$VOLTDB_BIN/../lib/voltdb" ]; then
    VOLTDB_BASE=$(dirname "$VOLTDB_BIN")
    VOLTDB_LIB="$VOLTDB_BASE/lib/voltdb"
    VOLTDB_VOLTDB="$VOLTDB_LIB"
# distribution layout has libraries in separate lib and voltdb directories
else
    VOLTDB_BASE=$(dirname "$VOLTDB_BIN")
    VOLTDB_LIB="$VOLTDB_BASE/lib"
    VOLTDB_VOLTDB="$VOLTDB_BASE/voltdb"
fi

VOLTPKG=../../tools/voltpkg

APPCLASSPATH=$CLASSPATH:$({ \
    \ls -1 "$VOLTDB_VOLTDB"/voltdb-*.jar; \
    \ls -1 "$VOLTDB_LIB"/*.jar; \
    \ls -1 "$VOLTDB_LIB"/extension/*.jar; \
} 2> /dev/null | paste -sd ':' - )
CLIENTCLASSPATH=$CLASSPATH:$({ \
    \ls -1 "$VOLTDB_VOLTDB"/voltdbclient-*.jar; \
    \ls -1 "$VOLTDB_LIB"/commons-cli-1.2.jar; \
} 2> /dev/null | paste -sd ':' - )
VOLTDB="$VOLTDB_BIN/voltdb"
LOG4J="$VOLTDB_VOLTDB/log4j.xml"
LICENSE="$VOLTDB_VOLTDB/license.xml"
HOST="localhost"

# remove build artifacts
function clean() {
    rm -rf obj debugoutput $APPNAME.jar voltdbroot voltdbroot
}

# compile the source code for procedures and the client
function srccompile() {
    mkdir -p obj
    javac -target 1.7 -source 1.7 -classpath $APPCLASSPATH -d obj \
        src/voter/*.java \
        src/voter/procedures/*.java
    # stop if compilation fails
    if [ $? != 0 ]; then exit; fi
}

# build an application catalog
function catalog() {
    srccompile
    echo "Compiling the voter application catalog."
    echo "To perform this action manually, use the command line: "
    echo
    echo "voltdb compile --classpath obj -o $APPNAME.jar ddl.sql"
    echo
    $VOLTDB compile --classpath obj -o $APPNAME.jar ddl.sql
    # stop if compilation fails
    if [ $? != 0 ]; then exit; fi
}

# run the voltdb server locally
function server() {
    # if a catalog doesn't exist, build one
    if [ ! -f $APPNAME.jar ]; then catalog; fi
    # run the server
    echo "Starting the VoltDB server."
    echo "To perform this action manually, use the command line: "
    echo
    echo "$VOLTDB create -d deployment.xml -l $LICENSE -H $HOST $APPNAME.jar"
    echo
    $VOLTDB create -d deployment.xml -l $LICENSE -H $HOST $APPNAME.jar
}

# run the voltdb server locally as a daemon process
function daemon() {
    # if a catalog doesn't exist, build one
    if [ ! -f $APPNAME.jar ]; then catalog; fi
    # run the server
    echo "Starting the VoltDB server."
    echo "To perform this action manually, use the command line: "
    echo
    echo "$VOLTDB create -d deployment.xml -l $LICENSE -H $HOST $APPNAME.jar"
    echo
    $VOLTDB create -B -d deployment.xml -l $LICENSE -H $HOST $APPNAME.jar
}

# run the voltdb server locally
function rejoin() {
    # if a catalog doesn't exist, build one
    if [ ! -f $APPNAME.jar ]; then catalog; fi
    # run the server
    $VOLTDB rejoin -H $HOST -d deployment.xml -l $LICENSE
}

# run the client that drives the example
function client() {
    async-benchmark
}

# Asynchronous benchmark sample
# Use this target for argument help
function async-benchmark-help() {
    test -f obj/$APPNAME/AsyncBenchmark.class || srccompile
    java -classpath obj:$CLIENTCLASSPATH:obj voter.AsyncBenchmark --help
}

# latencyreport: default is OFF
# ratelimit: must be a reasonable value if lantencyreport is ON
# Disable the comments to get latency report
function async-benchmark() {
    test -f obj/$APPNAME/AsyncBenchmark.class || srccompile
    java -classpath obj:$CLIENTCLASSPATH:obj -Dlog4j.configuration=file://$LOG4J \
        voter.AsyncBenchmark \
        --displayinterval=5 \
        --warmup=5 \
        --duration=120 \
        --servers=localhost:21212 \
        --contestants=6 \
        --maxvotes=2
#        --latencyreport=true \
#        --ratelimit=100000
}

function simple-benchmark() {
    test -f obj/$APPNAME/SimpleBenchmark.class || srccompile
    java -classpath obj:$CLIENTCLASSPATH:obj -Dlog4j.configuration=file://$LOG4J \
        voter.SimpleBenchmark localhost
}

# Multi-threaded synchronous benchmark sample
# Use this target for argument help
function sync-benchmark-help() {
    test -f obj/$APPNAME/SyncBenchmark.class || srccompile
    java -classpath obj:$CLIENTCLASSPATH:obj voter.SyncBenchmark --help
}

function sync-benchmark() {
    test -f obj/$APPNAME/SyncBenchmark.class || srccompile
    java -classpath obj:$CLIENTCLASSPATH:obj -Dlog4j.configuration=file://$LOG4J \
        voter.SyncBenchmark \
        --displayinterval=5 \
        --warmup=5 \
        --duration=120 \
        --servers=localhost:21212 \
        --contestants=6 \
        --maxvotes=2 \
        --threads=40
}

# JDBC benchmark sample
# Use this target for argument help
function jdbc-benchmark-help() {
    test -f obj/$APPNAME/JDBCBenchmark.class || srccompile
    java -classpath obj:$CLIENTCLASSPATH:obj voter.JDBCBenchmark --help
}

function jdbc-benchmark() {
    test -f obj/$APPNAME/JDBCBenchmark.class || srccompile
    java -classpath obj:$CLIENTCLASSPATH:obj -Dlog4j.configuration=file://$LOG4J \
        voter.JDBCBenchmark \
        --displayinterval=5 \
        --duration=120 \
        --maxvotes=2 \
        --servers=localhost:21212 \
        --contestants=6 \
        --threads=40
}

### Docker commands

function docker-build() {
    test -f Dockerfile || docker-generate
    docker build -q --rm -t $APPNAME .
}

function docker-run() {
    docker run -p 127.0.0.1:41212:21212 -t $APPNAME
}

function docker-rebuild() {
    test -f Dockerfile || docker-generate
    docker build -q --no-cache --rm -t $APPNAME .
}

function docker-clean() {
    for C in $(docker ps -a -q); do
        docker stop $C
        docker rm $C
    done
    for I in $(docker images -a -q); do
        docker rmi $I
    done
}

function docker-clean-all() {
    docker-clean
    \rm -rf dist Dockerfile
}

function docker-show() {
    docker images | awk "NR==1||\$1~/^$APPNAME/{print}"
}

function docker-generate() {
    $VOLTPKG docker -O
}

function docker-client() {
    test -f obj/$APPNAME/AsyncBenchmark.class || srccompile
    echo java -classpath obj:$CLIENTCLASSPATH:obj -Dlog4j.configuration=file://$LOG4J \
        voter.AsyncBenchmark \
        --displayinterval=5 \
        --warmup=5 \
        --duration=120 \
        --servers=127.0.0.1:41212 \
        --contestants=6 \
        --maxvotes=2
}
function help() {
    echo "Usage: ./run.sh {clean|catalog|server|async-benchmark|aysnc-benchmark-help|...}"
    echo "       {...|sync-benchmark|sync-benchmark-help|jdbc-benchmark|jdbc-benchmark-help|...}"
    echo "       {...|docker-build|docker-run|docker-rebuild|docker-clean|docker-clean-all|...}"
    echo "       {...|docker-client|docker-show|docker-generate}"
}

# Run the target passed as the first arg on the command line
# If no first arg, run server
if [ $# -gt 1 ]; then help; exit; fi
if [ $# = 1 ]; then $1; else server; fi
