# Branch Protection Setup

This repository requires specific status checks to pass before PRs can be merged to the `main` branch.

## Required Status Checks

The following GitHub Actions workflows must pass before merging:

1. **Build** - Compiles the firmware for ESP32-P4
2. **Device Tests / build** - Builds firmware with test endpoints enabled
3. **Device Tests / device-tests** - Runs integration tests on physical hardware

## Setup Instructions

### Automatic Setup (Recommended)

Run the branch protection setup script:

```bash
./scripts/setup-branch-protection.sh
```

This requires:
- GitHub Personal Access Token with `repo` scope
- Set as environment variable: `GITHUB_TOKEN=your_token_here`

### Manual Setup

1. Go to repository Settings → Branches
2. Add rule for `main` branch
3. Enable "Require status checks to pass before merging"
4. Search for and select these required checks:
   - `Build`
   - `Device Tests / build`
   - `Device Tests / device-tests`
5. Enable "Require branches to be up to date before merging"
6. Save changes

## Required Status Checks Explained

- **Build**: Ensures the firmware compiles successfully with production settings (test endpoints disabled)
- **Device Tests / build**: Ensures the firmware compiles with test endpoints enabled
- **Device Tests / device-tests**: Ensures all device integration tests pass on actual hardware

## Verification

The repository includes a workflow that verifies branch protection is configured correctly:
- `.github/workflows/verify-branch-protection.yml`

This runs daily and on workflow dispatch to ensure protection rules remain in place.

## Updating Requirements

To modify required status checks:

1. Update this documentation
2. Update `scripts/setup-branch-protection.sh`
3. Update `.github/workflows/verify-branch-protection.yml`
4. Run the setup script to apply changes
