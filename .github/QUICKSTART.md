# Quick Start: Branch Protection Setup

This guide will help you protect your `main` branch in under 5 minutes.

## Why Branch Protection?

Branch protection ensures:
- ✅ All changes go through pull requests
- ✅ The build must pass before merging
- ✅ No accidental direct pushes to main
- ✅ Code quality is maintained

## Quick Setup (Choose One Method)

### Method 1: Automated Script (Recommended)

```bash
# Run the automated setup script
./scripts/setup-branch-protection.sh
```

This requires:
- GitHub CLI (`gh`) installed
- Authentication: `gh auth login`

### Method 2: GitHub Web UI (Manual, 2 minutes)

1. Go to: `https://github.com/sslivins/arctic-controller/settings/branches`

2. Click **Add rule** or **Add branch protection rule**

3. Enter branch name: `main`

4. Enable these settings:
   - ✅ **Require a pull request before merging**
     - Set required approvals to `0`
   - ✅ **Require status checks to pass before merging**
     - ✅ **Require branches to be up to date**
     - Search and select: `Device Tests / build` and `Device Tests / device-tests`
   - ✅ **Require conversation resolution before merging**

5. Click **Create** or **Save changes**

**Done!** 🎉

## Verify It's Working

Run the verification workflow:

```bash
gh workflow run verify-branch-protection.yml
```

Or go to: `https://github.com/sslivins/arctic-controller/actions/workflows/verify-branch-protection.yml`

## New Workflow for Making Changes

Now that protection is active, here's your new process:

```bash
# 1. Create a feature branch
git checkout -b feature/my-awesome-feature

# 2. Make your changes
# ... edit files ...

# 3. Commit and push
git add .
git commit -m "Add awesome feature"
git push origin feature/my-awesome-feature

# 4. Create a pull request on GitHub
# Go to: https://github.com/sslivins/arctic-controller/pulls
# Click "New pull request"
# Select your branch and click "Create pull request"

# 5. Wait for the build to pass (green checkmark)

# 6. Merge the PR
# Click "Merge pull request" on GitHub

# 7. Update your local main branch
git checkout main
git pull origin main

# 8. Delete the feature branch (optional)
git branch -d feature/my-awesome-feature
```

## What Gets Checked?

The following must pass before merging:

| Check | Description | Defined In |
|-------|-------------|------------|
| `Device Tests / build` | Compiles firmware for ESP32-P4 | `.github/workflows/device-tests.yml` |
| `Device Tests / device-tests` | Runs tests on actual hardware | `.github/workflows/device-tests.yml` |

**Note:** The standalone `Build` workflow is redundant since Device Tests already builds the firmware.

## Troubleshooting

### Required checks not appearing

The workflows must run at least once on main for GitHub to recognize them.

**Solution:**
- The Device Tests workflow needs to run on main first
- Then the checks will be available for future PRs
- You can trigger it manually or wait for it to run on the next PR

### Can't push to main anymore

This is expected! Direct pushes are now blocked.

**Solution:**
- Always create a branch for changes
- Push the branch and create a PR
- Merge the PR after the build passes

### Need to make an urgent fix

You have two options:

1. **Quick PR workflow** (recommended):
   ```bash
   git checkout -b hotfix/urgent-fix
   # make changes
   git commit -am "Urgent fix"
   git push origin hotfix/urgent-fix
   # Create PR on GitHub, wait for build, merge
   ```

2. **Temporarily disable protection**:
   - Go to branch protection settings
   - Click "Delete" or "Edit"
   - Make your changes
   - Re-enable protection

## Need Help?

For detailed information, see:
- [Full Branch Protection Guide](.github/BRANCH_PROTECTION.md)
- [GitHub Documentation](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-protected-branches)

## Summary

- ✅ Branch protection prevents direct pushes to `main`
- ✅ All changes must go through pull requests
- ✅ The `build` workflow must pass before merging
- ✅ No approvals required (perfect for solo development)
- ✅ You can still merge your own PRs after builds pass
