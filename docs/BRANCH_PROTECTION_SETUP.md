# Setting Up Branch Protection for Device Tests

This guide explains how to configure GitHub branch protection to require the "Device Tests" workflow to pass before merging PRs to main.

## Overview

The Device Tests workflow has two jobs that both need to pass:
1. **Device Tests / build** - Builds firmware with test endpoints enabled
2. **Device Tests / device-tests** - Runs integration tests on physical hardware

Additionally, the Build workflow must also pass for production firmware validation.

## Quick Setup

### Step 1: Generate GitHub Personal Access Token

1. Go to https://github.com/settings/tokens
2. Click "Generate new token" → "Generate new token (classic)"
3. Give it a descriptive name like "Branch Protection Setup"
4. Select the `repo` scope (Full control of private repositories)
5. Click "Generate token"
6. **Copy the token** (you won't see it again!)

### Step 2: Run the Setup Script

```bash
export GITHUB_TOKEN=your_token_here
./scripts/setup-branch-protection.sh
```

The script will:
- Configure branch protection for the `main` branch
- Require these status checks to pass:
  - Build
  - Device Tests / build
  - Device Tests / device-tests
- Require branches to be up to date before merging
- Allow solo developer workflow (no required reviews)

### Step 3: Verify

Visit https://github.com/sslivins/arctic-controller/settings/branches to confirm the settings.

Or run the verification workflow manually:
1. Go to Actions → Verify Branch Protection
2. Click "Run workflow"

## What This Means for Your Workflow

### Before Branch Protection
- You could merge PRs even if tests failed
- Risk of broken code in main branch

### After Branch Protection
- ✅ All required workflows must pass before merging
- ✅ Ensures firmware builds successfully
- ✅ Ensures all device tests pass on hardware
- ✅ Maintains code quality in main branch

### For This PR (Notification Tests)
Once branch protection is configured:
1. Your notification tests will run automatically on the device
2. The PR can only merge after both:
   - Build workflow passes ✅
   - Device Tests workflow passes ✅
3. This ensures your new tests work before merging

## Manual Configuration (Alternative)

If you prefer to configure manually via GitHub UI:

1. Go to https://github.com/sslivins/arctic-controller/settings/branches
2. Click "Add rule" or edit existing rule for `main`
3. Check ☑ "Require status checks to pass before merging"
4. Check ☑ "Require branches to be up to date before merging"
5. In the search box, type and select:
   - `Build`
   - `Device Tests / build`
   - `Device Tests / device-tests`
6. Scroll down and click "Create" or "Save changes"

## Troubleshooting

### Status Checks Not Appearing

The status checks won't appear in the search until they've run at least once. Solutions:

1. **Create a test PR first** - This will trigger the workflows
2. **Wait for first run** - After the workflows complete, the checks will be available
3. **Then configure branch protection** - Now you can select the checks

### Script Fails with 401 Error

Your token doesn't have the right permissions:
- Make sure you selected the `repo` scope when creating the token
- Try regenerating the token with full `repo` access

### Script Fails with 403 Error

Your token can't modify repository settings:
- Ensure you're the repository owner or have admin access
- Check that the token hasn't expired

## Automated Verification

The repository includes a workflow that runs daily to verify branch protection:
- `.github/workflows/verify-branch-protection.yml`
- Checks that all required status checks are configured
- Alerts if configuration drifts

## Updating Requirements

To add or remove required status checks:

1. Edit `scripts/setup-branch-protection.sh`
2. Update the `checks` array in the JSON configuration
3. Run the script again: `./scripts/setup-branch-protection.sh`
4. Update `.github/BRANCH_PROTECTION.md` documentation

## Learn More

- [GitHub Branch Protection Rules](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-protected-branches/about-protected-branches)
- [Required Status Checks](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-protected-branches/about-protected-branches#require-status-checks-before-merging)
