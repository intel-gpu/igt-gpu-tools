# IGT Branch Management for Pre-Release Features

## Overview

This repository manages IGT (Intel GPU Tools) branches for features that are not yet fully upstreamed and require production support before upstream acceptance, as well as backport branches aligned with specific kernel versions.

**Primary Use Cases:**
- **Feature Branches** (e.g., `feature/eudebug`): Support for features not yet fully upstreamed, such as Eudebug (EU debugging) feature validation for Xe driver
- **Backport Branches**: Kernel-specific IGT snapshots with prelim uAPIs for production testing against backported kernel releases

**About IGT:** IGT (Intel GPU Tools) is a comprehensive test suite for GPU drivers, originally developed for Intel graphics but now supporting multiple vendors (Intel, AMD, NVIDIA via Nouveau, etc.). It verifies driver functionality, stability, and correctness across different graphics hardware.

**Upstream Repository:** [https://gitlab.freedesktop.org/drm/igt-gpu-tools](https://gitlab.freedesktop.org/drm/igt-gpu-tools) (freedesktop.org)

### Repository Scope

- Maintain test coverage for not-yet-upstreamed features (e.g., Eudebug) on top of upstream IGT
- Support multiple feature branches based on customer/product requirements
- Provide rolling IGT updates tracking the latest backported kernel (backport/main)
- Provide stable IGT snapshots for production kernel releases (backport/v6.17)
- Enable CI traceability through immutable snapshot tags and version tracking

## 1. Branch Strategy

### 1.1. Branch and Tag Overview

| Branch | Status | Purpose | uAPI Source | Kernel Under Test | Tag Format |
|--------|--------|---------|-------------|-------------------|-----------|
| `feature/eudebug` | Active | Feature branch: Eudebug production patches on upstream igt | Aligned with drm-tip, frequent changes | [drmtip-eudebug](https://github.com/mpatelcz/drivers.gpu.linux-xe.kernel/tree/drm-tip-eudebug/) | **Format:** `IGT-eudebug-<igt_ver>-<igt_sha>-<date>.<seq>`<br>**Example:** `IGT-eudebug-v2.4-abc1234-20260520.1` |
| `backport/main` | Active | Active rolling branch (tracks latest [backport kernel](https://github.com/intel-gpu/xekmd-backports/tree/backport/main)) | Prelim ([DuH](https://github.com/intel-gpu/drm-uapi-helper/tree/xe)) | [xekmd-backports/main](https://github.com/intel-gpu/xekmd-backports/tree/backport/main) | **Format:** `IGT-backport-main-<igt_ver>-<igt_sha>-<date>.<feature_seq>.<backport_seq>`<br>**Example:** `IGT-backport-main-v2.4-abc1234-20260520.1.0` |
| `backport/v6.17` | Frozen  | Frozen snapshot for kernel version production | Prelim ([DuH](https://github.com/intel-gpu/drm-uapi-helper/tree/xe)) | [xekmd-backports/v6.17](https://github.com/intel-gpu/xekmd-backports/tree/backport/v6.17) | **Format:** `IGT-backport-v6.17-<igt_ver>-<igt_sha>-<date>.<feature_seq>.<backport_seq>`<br>**Example:** `IGT-backport-v6.17-v2.4-abc1234-20260520.1.0` |

**Branch Status Legend:**

- **Active** - Receives rebases and new features
- **Frozen** - Only receives critical fixes (no new features or rebases)
- **Obsolete** - End of life (EOL), no longer maintained

**Tag Format Legend:**

- `<igt_ver>` - Upstream IGT version (e.g., `v2.4`)
- `<igt_sha>` - Upstream IGT commit SHA, 7 chars (e.g., `abc1234`)
- `<date>` - Rebase date in YYYYMMDD format (e.g., `20260520`)
- `<seq>` / `<feature_seq>` - Feature sequence number (tracks feature-specific changes only)
- `<backport_seq>` - Backport sequence number (incremental changes in backport branch)

**Notes:**

1. **Additional feature branches** can be created following the same pattern as `feature/eudebug` (e.g., `feature/feature-xyz` for other not-yet-upstreamed features). **Important:** New feature branches are created on top of existing feature branches, meaning they accumulate all previous features (e.g., a `feature/feature-xyz` branch created after `feature/eudebug` would contain both Eudebug patches and feature-xyz patches). Each feature branch follows the same workflow: rebase on upstream IGT, tag with sequence numbers, and create backport snapshots as needed.

## 2. Tag Naming Convention

### 2.1. For `feature/eudebug` Branch

**Format:** `IGT-eudebug-<igt_ver>-<igt_sha>-<date>.<seq>`

**Example:** `IGT-eudebug-v2.4-cae7174-20260520.1`

**Components:**

- `IGT` - Prefix for clarity
- `eudebug` - Feature branch identifier
- `v2.4` - Last upstream IGT release tag
- `cae7174` - Upstream IGT commit SHA (7 characters)
- `20260520` - Snapshot date (YYYYMMDD)
- `.1` - Sequence number (tracks eudebug patch changes only)

**Sequence Number Logic:**

- **Increments when:** Eudebug patches are added or modified
- **Stays same when:** Only rebasing on newer upstream (no eudebug changes), except when there are multiple rebases on the same day

**Examples:**

```
IGT-eudebug-v2.4-abc1234-20260520.1   ← Initial snapshot
IGT-eudebug-v2.4-abc1234-20260522.2   ← Eudebug patches modified (+1)
IGT-eudebug-v2.4-def5678-20260527.2   ← Rebased on new upstream (same .2)
IGT-eudebug-v2.5-xyz9876-20260603.2   ← Upstream version changed (same .2)
```

### 2.2. For `backport/*` Branches

**Format:** `IGT-backport-<branch>-<igt_ver>-<igt_sha>-<date>.<feature_seq>.<backport_seq>`

**Example:** `IGT-backport-main-v2.4-abc1234-20260520.1.0`

**Components:**

- `backport-<branch>` - Kernel branch identifier (main, v6.17, etc.)
- `<igt_ver>` - Upstream IGT version
- `<igt_sha>` - Upstream IGT commit SHA
- `<date>` - Snapshot date
- `<feature_seq>` - Which eudebug snapshot it was created from
- `<backport_seq>` - Incremental changes in backport branch

**Examples:**

```
IGT-backport-main-v2.4-abc1234-20260520.1.0    ← Initial backport snapshot from eudebug .1
IGT-backport-main-v2.4-abc1234-20260521.1.1    ← Backport-specific fix added (+1)
IGT-backport-main-v2.4-def5678-20260527.2.0    ← Rebased from eudebug .2 (backport resets to .0)
IGT-backport-v6.17-v2.4-abc1234-20260520.1.0   ← Frozen snapshot for v6.17
IGT-backport-v6.17-v2.4-abc1234-20260601.1.1   ← Critical fix only (+1)
```
