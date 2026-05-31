# Contributing to vnetif

Thank you for your interest in contributing.  Patches and issues are welcome.

## Code style

All C code must follow the
[Linux kernel coding style](https://www.kernel.org/doc/html/latest/process/coding-style.html):

- Tabs for indentation (width 8)
- Line length ≤ 80 columns where feasible
- `SPDX-License-Identifier: GPL-2.0-only` on every source file
- Copyright header on every source file
- `pr_fmt` defined before any includes in translation units that log
- Function names: `module_subsystem_action` (e.g. `vnetif_dev_create`)

Run `checkpatch.pl` from the kernel tree against your diff before submitting:

```bash
/path/to/linux/scripts/checkpatch.pl --no-tree -f src/*.c src/*.h
```

## Submitting changes

1. Fork the repository and create a feature branch.
2. Write or update tests in `tests/vnetif_test.cc` for any new behaviour.
3. Ensure `make test` passes as root.
4. Commit with a clear subject line following
   [Conventional Commits](https://www.conventionalcommits.org/):
   `feat:`, `fix:`, `docs:`, `refactor:`, `test:`.
5. Open a pull request with a description of the motivation and design.

## Reporting bugs

Open a GitHub issue with:
- Kernel version (`uname -r`)
- Distribution
- Steps to reproduce
- Kernel log output (`sudo dmesg | grep vnetif`)

## License

By contributing you agree that your changes will be licensed under
`GPL-2.0-only`, the same license as the rest of the project.