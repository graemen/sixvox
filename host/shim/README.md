# Host shims

Computer replacements for the Beads hardware headers, so that the VOX platform
code (`vox/common/hw`, `vox/common/ui`) and every algorithm's panel glue compile
unchanged for a computer (VCV Rack, host tests). This directory is put first on
the include path; each header keeps the path and the interface of the one it
replaces. The hardware is reached through plain globals in `namespace
vox_host` (one emulated module per loaded copy of the core library).
