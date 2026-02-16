# Scripts

Utility scripts for repository management and development.

## Branch Protection

### `setup-branch-protection.sh`

Configures branch protection for the `main` branch using GitHub CLI.

**Prerequisites:**
- GitHub CLI (`gh`) installed and authenticated
- Admin access to the repository

**Usage:**
```bash
./scripts/setup-branch-protection.sh
```

**What it does:**
- Requires pull requests before merging to main
- Requires the 'build' workflow to pass
- Requires branches to be up-to-date
- Prevents force pushes and deletions
- No review approvals required (suitable for solo development)

For detailed information about branch protection, see [../.github/BRANCH_PROTECTION.md](../.github/BRANCH_PROTECTION.md)

## Installation

All scripts in this directory should be executable. If you need to make them executable:

```bash
chmod +x scripts/*.sh
```
