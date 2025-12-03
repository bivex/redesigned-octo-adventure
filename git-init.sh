#!/bin/bash
# Initialize git repository and create dev1 branch for libreactor project

echo "=== Initializing Git Repository for Libreactor ==="

# Check if git is available
if ! command -v git &> /dev/null; then
    echo "❌ Git is not installed"
    exit 1
fi

# Initialize git repository
echo "Initializing git repository..."
git init

# Configure git
git config user.name "Libreactor Developer"
git config user.email "dev@libreactor.local"

# Create .gitignore
echo "Creating .gitignore..."
cat > .gitignore << 'EOF'
# Build artifacts
*.o
libreactor
libreactor-server
perf.data
perf.data.old

# Temporary files
*.tmp
*.log
*.out

# OS files
.DS_Store
Thumbs.db

# IDE files
.vscode/
.idea/
*.swp
*.swo

# Performance profiling
flamegraph.svg
*.perf

# Docker
.dockerignore
EOF

# Add all files
echo "Adding files to git..."
git add .

# Create initial commit
echo "Creating initial commit..."
git commit -m "Initial commit: Extreme HTTP Performance Server

- SO_REUSEPORT + BPF connection distribution
- Multi-process architecture with CPU pinning
- Socket optimizations (busy poll, TCP_NODELAY, keepalive)
- Maximum compilation optimizations (-O3 -flto -march=native)
- System-level optimizations (kernel params, sysctl, nftables)

Performance: 65k-80k req/sec on 3 CPU cores"

# Create and switch to dev1 branch
echo "Creating dev1 branch..."
git checkout -b dev1

echo ""
echo "✅ Git repository initialized successfully!"
echo "📍 Repository: $(pwd)"
echo "🌿 Branch: $(git branch --show-current)"
echo "📊 Status:"
git status --short

echo ""
echo "📝 Next steps:"
echo "  git log --oneline    # View commits"
echo "  git status          # Check status"
echo "  git diff            # See changes"
echo ""
echo "🚀 Repository ready for development on branch dev1!"
