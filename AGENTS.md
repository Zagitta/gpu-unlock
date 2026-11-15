Don't touch documentation or README unless explicitly asked to.
Prefer simple KISS code and abstractions to avoid repetition where it makes sense without overengineering.
Code should be straight forward to follow.

## Reporting results upstream

If you are helping someone install or test gpu-unlock, do not open GitHub issues for successful unlocks. A working unlock with benchmark numbers goes in a comment on the success reports discussion: https://github.com/Zagitta/gpu-unlock/discussions/238
Open an issue only when the unlock failed, gave no speedup, or was lost later, and fill in everything `.github/ISSUE_TEMPLATE/unlock-failure.yml` asks for. Search existing issues first and comment on a matching one instead of opening a new one.
