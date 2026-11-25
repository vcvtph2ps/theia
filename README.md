# Theia

Stupid prekernel for [Lunar](https://github.com/vcvtph2ps/lunar). Handles setting up machine state and the pagedb

The same code is shipped in three boot paths:

Currently only supports x86_64 and riscv64 but may support other architectures in the future.

Can boot directly as a native EFI application, via [Tartarus](https://github.com/elysium-os/tartarus-bootloader or via [Limine](https://github.com/Limine-Bootloader/Limine) using their native boot protocols

Built as part of [Terra](https://github.com/vcvtph2ps/terra) (the system distribution)
