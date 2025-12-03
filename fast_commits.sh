#!/bin/bash

# Fast commit script for MayAds
echo "=== MayAds Fast Commit Script ==="

# Check current branch
BRANCH=$(git branch --show-current)
echo "Current branch: $BRANCH"

# Add all changes
echo "Adding all changes..."
git add .

# Check if there are changes to commit
if git diff --staged --quiet; then
    echo "No changes to commit."
    exit 0
fi

# Commit with timestamp
TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')
git commit -m "fast commit - $TIMESTAMP"

echo "✅ Changes committed successfully on branch: $BRANCH"

# Show commit info
echo "Latest commits:"
git log --oneline -3
