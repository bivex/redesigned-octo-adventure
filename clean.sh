#!/bin/bash

# Clean build artifacts using Makefile
echo "Cleaning build artifacts..."
make clean

# Remove generated log files
echo "Removing log files..."
rm -f *.log

# Remove profiling outputs
echo "Removing profiling outputs..."
rm -f *.out *.out.* *.data *.data.old

# Remove flamegraph images
echo "Removing flamegraph images..."
rm -f *.svg

# Remove preprocessed source files
echo "Removing preprocessed source files..."
rm -f *.i

# Remove other generated files
echo "Removing other generated files..."
rm -f strace_out pvs-full-report.txt

# Remove profiling and report directories
echo "Removing generated directories..."
rm -rf perf-profiles amd-profiles leak-profiles pvs-report.html

echo "Cleanup complete."
