# Quick Start: Require Device Tests Before Merge

## TL;DR - 3 Steps

```bash
# 1. Get a GitHub token from: https://github.com/settings/tokens
#    - Select "repo" scope
#    - Copy the token

# 2. Run the setup script
export GITHUB_TOKEN=your_token_here
./scripts/setup-branch-protection.sh

# 3. Verify it worked
# Visit: https://github.com/sslivins/arctic-controller/settings/branches
```

## What This Does

✅ Requires all these to pass before merging ANY PR to main:
- Build workflow
- Device Tests / build job  
- Device Tests / device-tests job

✅ Ensures your new notification tests run on hardware before merge

✅ Prevents broken code from entering main branch

## That's It!

Once configured, every PR (including this one with notification tests) will automatically require all device tests to pass before the merge button becomes available.

See `docs/BRANCH_PROTECTION_SETUP.md` for detailed guide and troubleshooting.
