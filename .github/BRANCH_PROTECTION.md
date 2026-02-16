# Branch Protection Setup Guide

This guide explains how to protect the `main` branch to ensure all tests pass before merging.

## Overview

Branch protection ensures code quality by requiring specific checks to pass before code can be merged into the `main` branch.

## Prerequisites

- Repository admin access
- Build workflow must be configured (already done: `.github/workflows/build.yml`)

## Setup Instructions

### Option 1: Using GitHub Web UI (Recommended)

1. Navigate to your repository on GitHub: `https://github.com/sslivins/arctic-controller`

2. Click on **Settings** (gear icon in the top menu)

3. In the left sidebar, click **Branches** under "Code and automation"

4. Under "Branch protection rules", click **Add rule** (or **Add branch protection rule**)

5. Configure the following settings:

   **Branch name pattern:**
   ```
   main
   ```

   **Protect matching branches - Enable these options:**
   
   - ✅ **Require a pull request before merging**
     - ✅ **Require approvals**: Set to `0` (since you're the only developer)
     - ✅ **Dismiss stale pull request approvals when new commits are pushed**
     - ⬜ **Require review from Code Owners** (optional, not needed for single developer)
     
   - ✅ **Require status checks to pass before merging**
     - ✅ **Require branches to be up to date before merging**
     - In the search box that appears, type `build` and select:
       - ✅ **build** (this is the job name from `.github/workflows/build.yml`)
   
   - ✅ **Require conversation resolution before merging** (optional, recommended)
   
   - ✅ **Do not allow bypassing the above settings** (optional, for strict enforcement)

6. Click **Create** (or **Save changes**)

### Option 2: Using GitHub CLI

If you have the GitHub CLI (`gh`) installed and authenticated:

```bash
gh api repos/sslivins/arctic-controller/branches/main/protection \
  -X PUT \
  -H "Accept: application/vnd.github+json" \
  --input - << 'EOF'
{
  "required_status_checks": {
    "strict": true,
    "contexts": ["build"]
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
```

### Option 3: Using the Provided Script

A convenience script is provided to set up branch protection:

```bash
./scripts/setup-branch-protection.sh
```

## What This Protects Against

Once configured, the following protections are in place:

1. **No direct pushes to main** - All changes must go through a pull request
2. **Build must succeed** - The `build` workflow must pass before merging
3. **Up-to-date branches** - Your branch must be up-to-date with main before merging

## Workflow for Making Changes

After protection is enabled:

1. Create a new branch:
   ```bash
   git checkout -b feature/my-feature
   ```

2. Make your changes and commit:
   ```bash
   git add .
   git commit -m "Add new feature"
   ```

3. Push to GitHub:
   ```bash
   git push origin feature/my-feature
   ```

4. Create a Pull Request:
   - Go to GitHub repository
   - Click "Compare & pull request"
   - Review changes
   - Click "Create pull request"

5. Wait for checks to pass:
   - The `build` workflow will run automatically
   - Once it passes (green checkmark), you can merge

6. Merge the PR:
   - Click "Merge pull request"
   - Click "Confirm merge"

7. Delete the feature branch (optional):
   - Click "Delete branch" on GitHub
   - Or locally: `git branch -d feature/my-feature`

## Current Required Checks

The following workflow checks are required to pass:

- **build** - Compiles the firmware for ESP32-P4 target
  - Checks out code with submodules
  - Fetches dependencies
  - Builds with ESP-IDF v5.4.3
  - Creates firmware artifacts

## Verifying Protection is Active

To verify branch protection is working:

```bash
# This should fail if protection is active:
git checkout main
echo "test" >> README.md
git commit -am "Direct push test"
git push origin main
```

You should see an error message like:
```
remote: error: GH006: Protected branch update failed for refs/heads/main.
```

## Troubleshooting

### Build workflow not appearing in status checks

- Make sure the workflow has run at least once on the main branch
- The workflow must have a job named `build` (already configured)
- Wait a few minutes after the first workflow run for GitHub to register it

### Still able to push directly to main

- Verify you don't have admin bypass enabled
- Check that the branch name pattern exactly matches `main`
- Ensure you've saved the branch protection rule

### Accidentally locked yourself out

As the repository owner/admin, you can:
- Temporarily disable branch protection
- Update the settings
- Re-enable protection

## Additional Resources

- [GitHub Docs: About protected branches](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-protected-branches/about-protected-branches)
- [GitHub Docs: Managing a branch protection rule](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-protected-branches/managing-a-branch-protection-rule)
