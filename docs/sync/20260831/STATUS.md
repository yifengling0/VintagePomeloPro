# Integration status

## Current

- T0: isolated branch created, source SHAs verified, 69 commits classified. Baseline host tests passed.
- Phone connected, API26; temporary screen running lock verified, original timeout saved outside Git.
- Installed phone package reports 1.3.2; screenshot captured. This is not yet proof of the exact main source baseline.
- T1: default-policy naming adapted without changing resolver behavior; infrastructure processes filtered only from running cards; managed payload sync extracted and unused SmokeRunner removed. Existing model tests updated for current desktop-only and v6 keyboard contracts.
- T1 gates: `make test`, `make test-model`, `make NATIVE_ARCH=arm64-v8a check-native`, `git diff --check` passed. HAP/device validation remains pending.
- Build checks now have `check-native` and `test-model` Makefile targets; both run in the existing SDK Docker container. Signing material stays ignored (`signs/` added alongside `sign/`).
- T2 in progress; T3-T8 pending. Intermediate commits are not release-qualified. No full integration or phone-regression completion claim.

## Resume

Read PLAN.md and commits.json, inspect current Git status before acting, then execute the next unfinished task. Preserve the full requested scope.
