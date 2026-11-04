#!/bin/bash

set -e
FullExecPath=$PWD
pushd `dirname $0` > /dev/null
FullScriptPath=`pwd`
popd > /dev/null


cd $FullScriptPath/../docker/centos_env
poetry install
docker_arguments=()
if [ "${FOXMES_DOCKER_NO_CACHE:-0}" = 1 ]; then
    docker_arguments+=(--no-cache)
fi
poetry run gen_dockerfile | DOCKER_BUILDKIT=1 docker build "${docker_arguments[@]}" -t tdesktop:centos_env -
cd $FullExecPath
