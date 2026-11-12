# Security policy

## Supported versions

| Version | Supported |
| ------- | --------- |
| 2.0.x   | :white_check_mark: |
| < 2.0   | :x: |

The module is pinned to one NVIDIA driver release, the one named in
`kernel/gpu_unlock/recipes.h`. A card unlocked against any other driver
version is out of support: the writes it makes are read out of that exact
driver's own Booter and payload.

## Reporting a vulnerability

Report privately through the *Report a vulnerability* form on this
repository's Security tab. Do not open a public issue, and do not open a
public pull request with the fix.

This code runs in ring 0, before the NVIDIA driver binds, from the initramfs,
and it writes registers that the stock driver never exposes to a driver
module. A flaw in it is a full host compromise and, for the registers it
touches, a way to leave a card needing a chassis power cycle. A public report
hands that out before there is a fix.

Expect an acknowledgement within a week. The maintainer reads every report
that names a register, a device id and a driver version.
