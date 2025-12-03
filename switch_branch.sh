#!/bin/bash

# Branch switching script for MayAds
echo "=== MayAds Branch Switcher ==="
echo "Available branches:"

# List all branches with current branch marked
git branch -v

echo ""
echo "Usage: $0 <branch_name>"
echo "Example: $0 main"
echo "Example: $0 feature/copy-writer-optimization"
echo ""

# If branch name provided, switch to it
if [ $# -eq 1 ]; then
    BRANCH=$1
    echo "Switching to branch: $BRANCH"

    if git show-ref --verify --quiet refs/heads/$BRANCH; then
        git checkout $BRANCH
        echo "✅ Switched to branch: $BRANCH"

        # Show latest commits on this branch
        echo "Latest commits on $BRANCH:"
        git log --oneline -3
    else
        echo "❌ Branch '$BRANCH' does not exist"
        echo "Available branches:"
        git branch
    fi
else
    echo "Please specify a branch name"
    echo "Current branch: $(git branch --show-current)"
fi
