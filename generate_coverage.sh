#!/bin/bash
# generate_coverage.sh - Build and generate coverage report

set -e

# Create build directory
BUILD_DIR="build_coverage"
rm -rf "$BUILD_DIR"
mkdir "$BUILD_DIR"
cd "$BUILD_DIR"

echo "=== Configuring with coverage ==="
cmake .. \
   -DCMAKE_BUILD_TYPE=None \
   -DENABLE_COVERAGE=ON \
   -DCORTOS_TIME_DRIVER=simulation

echo ""
echo "=== Building ==="
cmake --build . -j$(nproc)

echo ""
echo "=== Generating coverage ==="
make coverage

echo ""
echo "=== Opening report ==="
REPORT="$(pwd)/coverage_html/index.html"

if [ -f "$REPORT" ]; then
   echo -e "Report:\n$REPORT"
else
   echo "ERROR: Report not found at $REPORT"
   exit 1
fi
