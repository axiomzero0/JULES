#!/usr/bin/env bash
# JULES compiler build (sandbox driver; CMakeLists.txt is provided for
# environments with cmake). Deterministic source list from sources.cmake.
set -euo pipefail
cd "$(dirname "$0")"

CXX=${CXX:-g++}
STD=""
for candidate in c++26 c++2c c++23; do
    if echo 'int main(){return 0;}' | $CXX -std=$candidate -x c++ -fsyntax-only - >/dev/null 2>&1; then
        STD="-std=$candidate"
        break
    fi
done
if [ -z "$STD" ]; then
    echo "error: no C++23/26 mode available on this toolchain" >&2
    exit 1
fi
echo "building julesc with $CXX $STD"

FLAGS="$STD -fno-exceptions -fno-rtti -O2 -Wall -Wextra -Isrc"
mkdir -p build/obj

mapfile -t SOURCES < <(grep -o 'src/[^ ]*\.cpp' sources.cmake | sort)

for f in "${SOURCES[@]}"; do
    obj="build/obj/$(echo "$f" | tr '/' '_').o"
    if [ "$obj" -nt "$f" ] && [ "$obj" -ot "build/julesc" ]; then :; fi
    echo "  CXX $f"
    $CXX $FLAGS -c "$f" -o "$obj"
done

OBJ=()
for f in "${SOURCES[@]}"; do
    OBJ+=("build/obj/$(echo "$f" | tr '/' '_').o")
done
$CXX "${OBJ[@]}" -o build/julesc
echo "built build/julesc"
