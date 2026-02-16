#!/bin/bash
# Setup branch protection for the main branch
# Requires GITHUB_TOKEN environment variable with repo scope

set -e

# Configuration
OWNER="sslivins"
REPO="arctic-controller"
BRANCH="main"

# Check for GitHub token
if [ -z "$GITHUB_TOKEN" ]; then
    echo "Error: GITHUB_TOKEN environment variable not set"
    echo "Please set a GitHub Personal Access Token with 'repo' scope:"
    echo "  export GITHUB_TOKEN=your_token_here"
    exit 1
fi

echo "Configuring branch protection for $OWNER/$REPO ($BRANCH branch)..."

# GitHub API endpoint
API_URL="https://api.github.com/repos/$OWNER/$REPO/branches/$BRANCH/protection"

# Branch protection configuration
# This requires the following status checks to pass:
# - Build (production build workflow)
# - Device Tests / build (test build)
# - Device Tests / device-tests (integration tests on hardware)
CONFIG=$(cat <<EOF
{
  "required_status_checks": {
    "strict": true,
    "checks": [
      {
        "context": "Build"
      },
      {
        "context": "Device Tests / build"
      },
      {
        "context": "Device Tests / device-tests"
      }
    ]
  },
  "enforce_admins": false,
  "required_pull_request_reviews": null,
  "restrictions": null,
  "required_linear_history": false,
  "allow_force_pushes": false,
  "allow_deletions": false,
  "block_creations": false,
  "required_conversation_resolution": false,
  "lock_branch": false,
  "allow_fork_syncing": false
}
EOF
)

# Apply branch protection
echo "Applying branch protection rules..."
HTTP_CODE=$(curl -s -o /tmp/gh_response.json -w "%{http_code}" \
  -X PUT \
  -H "Accept: application/vnd.github+json" \
  -H "Authorization: Bearer $GITHUB_TOKEN" \
  -H "X-GitHub-Api-Version: 2022-11-28" \
  "$API_URL" \
  -d "$CONFIG")

if [ "$HTTP_CODE" = "200" ]; then
    echo "✅ Branch protection configured successfully!"
    echo ""
    echo "Required status checks:"
    echo "  - Build"
    echo "  - Device Tests / build"
    echo "  - Device Tests / device-tests"
    echo ""
    echo "Settings:"
    echo "  - Require branches to be up to date: Yes"
    echo "  - Enforce for administrators: No"
    echo "  - Required PR reviews: None (solo developer workflow)"
elif [ "$HTTP_CODE" = "401" ]; then
    echo "❌ Authentication failed. Check your GITHUB_TOKEN."
    exit 1
elif [ "$HTTP_CODE" = "403" ]; then
    echo "❌ Permission denied. Token needs 'repo' scope."
    exit 1
elif [ "$HTTP_CODE" = "404" ]; then
    echo "❌ Repository or branch not found."
    exit 1
else
    echo "❌ Failed with HTTP $HTTP_CODE"
    echo "Response:"
    cat /tmp/gh_response.json
    exit 1
fi

echo ""
echo "Verification:"
echo "Visit https://github.com/$OWNER/$REPO/settings/branches to confirm settings"
