#!/bin/env bash

cd `dirname $0`/..
echo `pwd`
repo_name=`basename $(pwd)`

lib_version=`grep -Po '(?<=LA_AVDECC_VERSION\s)[0-9.]*' CMakeLists.txt`
branch_name=`git branch --show-current`
commit_hash=`git log --pretty=format:'%h' -n 1`
file_name="avdecc-${lib_version}-${branch_name}-${commit_hash}"

cd ..

tar cf "/tmp/${file_name}.tar" \
  --exclude-vcs \
  --exclude "${repo_name}/externals/3rdparty/json/doc/*" \
  --exclude "${repo_name}/externals/3rdparty/json/test/*" \
  --exclude "${repo_name}/externals/3rdparty/fmtlib/test/*" \
  --exclude "${repo_name}/externals/3rdparty/fmtlib/doc/*" \
  --exclude "${repo_name}/externals/3rdparty/json/benchmarks/*" \
  --exclude "${repo_name}/externals/3rdparty/gtest/googletest/test/*" \
  --exclude "${repo_name}/externals/3rdparty/gtest/googlemock/test/*" \
  --exclude "${repo_name}/externals/nih/externals/3rdparty/gtest/*" \
  ${repo_name}

tar xf "/tmp/${file_name}.tar" -C /tmp

cd /tmp
zip -r -9 -q "/tmp/${file_name}.zip" "${repo_name}"
rm -rf "/tmp/${repo_name}"

echo "/tmp/${file_name}.zip"
