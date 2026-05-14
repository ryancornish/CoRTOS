set -e

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
if [ "$PWD" != "$SCRIPT_DIR" ]; then
   echo "Error: Script must be run from the same directory it resides in."
   exit 1
fi

rm -rf ./out/

cortos-builder build -p build_tree/profile.toml

mkdir ./out/bin/

g++-15 test_singlecore_waitables.cpp \
   -std=gnu++26 -O0 -g3 \
   -Iout/gcc-basic/include/ \
   -Lout/gcc-basic/lib/ \
   -lcortos \
   -lboost_context \
   -lgtest \
   -o out/bin/test_singlecore_waitables

echo "Created ./out/bin/test_singlecore_waitables"
echo "Executing test:"
./out/bin/test_singlecore_waitables
