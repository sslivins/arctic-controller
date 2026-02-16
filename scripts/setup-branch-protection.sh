#!/bin/bash

# Branch Protection Setup Script
# This script configures branch protection for the main branch using GitHub CLI

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

REPO="sslivins/arctic-controller"
BRANCH="main"

echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BLUE}  Arctic Controller - Branch Protection Setup${NC}"
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""

# Check if gh is installed
if ! command -v gh &> /dev/null; then
    echo -e "${RED}Error: GitHub CLI (gh) is not installed${NC}"
    echo "Please install it from: https://cli.github.com/"
    exit 1
fi

# Check if authenticated
if ! gh auth status &> /dev/null; then
    echo -e "${RED}Error: Not authenticated with GitHub CLI${NC}"
    echo "Please run: gh auth login"
    exit 1
fi

echo -e "${YELLOW}Current repository:${NC} $REPO"
echo -e "${YELLOW}Branch to protect:${NC} $BRANCH"
echo ""

# Check current protection status
echo -e "${BLUE}Checking current protection status...${NC}"
if gh api "repos/$REPO/branches/$BRANCH/protection" 2>/dev/null | jq . > /dev/null 2>&1; then
    echo -e "${YELLOW}⚠️  Branch protection is already configured${NC}"
    echo ""
    read -p "Do you want to update the configuration? (y/n) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        echo -e "${BLUE}Operation cancelled${NC}"
        exit 0
    fi
else
    echo -e "${GREEN}✓ No protection currently configured${NC}"
fi

echo ""
echo -e "${BLUE}Configuring branch protection...${NC}"
echo ""
echo "The following settings will be applied:"
echo -e "  ${GREEN}✓${NC} Require pull request before merging (0 approvals needed)"
echo -e "  ${GREEN}✓${NC} Require status checks: Device Tests workflow (build + tests)"
echo -e "  ${GREEN}✓${NC} Require branches to be up-to-date before merging"
echo -e "  ${GREEN}✓${NC} Dismiss stale pull request approvals on new commits"
echo -e "  ${GREEN}✓${NC} Require conversation resolution before merging"
echo -e "  ${GREEN}✓${NC} No force pushes allowed"
echo -e "  ${GREEN}✓${NC} No deletions allowed"
echo ""
read -p "Continue? (y/n) " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo -e "${BLUE}Operation cancelled${NC}"
    exit 0
fi

# Apply branch protection
echo ""
echo -e "${BLUE}Applying configuration...${NC}"

if gh api "repos/$REPO/branches/$BRANCH/protection" \
  -X PUT \
  -H "Accept: application/vnd.github+json" \
  --silent \
  --input - << 'EOF'
{
  "required_status_checks": {
    "strict": true,
    "contexts": ["Device Tests / build", "Device Tests / device-tests"]
  },
  "enforce_admins": false,
  "required_pull_request_reviews": {
    "dismiss_stale_reviews": true,
    "require_code_owner_reviews": false,
    "required_approving_review_count": 0
  },
  "restrictions": null,
  "required_linear_history": false,
  "allow_force_pushes": false,
  "allow_deletions": false,
  "required_conversation_resolution": true
}
EOF
then
    echo -e "${GREEN}✓ Branch protection configured successfully!${NC}"
    echo ""
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${GREEN}  Success!${NC}"
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo ""
    echo "Branch protection is now active on '$BRANCH'."
    echo ""
    echo "Next steps:"
    echo "  1. All changes must now go through pull requests"
    echo "  2. The 'build' workflow must pass before merging"
    echo "  3. Branches must be up-to-date with main"
    echo ""
    echo "To make changes:"
    echo "  git checkout -b feature/my-feature"
    echo "  # make changes"
    echo "  git push origin feature/my-feature"
    echo "  # Create PR on GitHub, wait for build to pass, then merge"
    echo ""
    
    # Display current configuration
    echo -e "${BLUE}Current protection rules:${NC}"
    gh api "repos/$REPO/branches/$BRANCH/protection" | jq '{
      required_status_checks: .required_status_checks.contexts,
      require_pr: (.required_pull_request_reviews != null),
      required_approvals: .required_pull_request_reviews.required_approving_review_count,
      enforce_admins: .enforce_admins.enabled,
      allow_force_pushes: .allow_force_pushes.enabled,
      allow_deletions: .allow_deletions.enabled
    }'
else
    echo -e "${RED}✗ Failed to configure branch protection${NC}"
    echo ""
    echo "Possible reasons:"
    echo "  - You may not have admin access to the repository"
    echo "  - The 'build' workflow may not have run yet on the main branch"
    echo "  - Network or API issues"
    echo ""
    echo "Manual setup instructions: See .github/BRANCH_PROTECTION.md"
    exit 1
fi
