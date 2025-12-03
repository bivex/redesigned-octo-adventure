#!/bin/bash
# Drop all changes to last commit script for Debian shell
echo "Dropping all changes to last commit..."
git reset --hard HEAD
echo "All changes have been dropped!"
