#!/usr/bin/env python3
"""Local Linux device-number adapter; leaves the competition repository intact.

The repository collector uses st_dev directly. On this Ubuntu VM, st_dev is
the filesystem partition (253:1), while cgroup v2 io.stat accounts I/O against
its whole block device (253:0). Resolve that relationship through sysfs.
This adapter changes device selection only, never the counters or score.
"""
import importlib.util
import os
from pathlib import Path
import sys

SOURCE = Path('/mnt/mvdb-src/bench/run_contract_benchmark.py')
spec = importlib.util.spec_from_file_location('mvdb_contract_benchmark', SOURCE)
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


def cgroup_block_device(path):
    device = os.stat(path, follow_symlinks=False).st_dev
    number = f'{os.major(device)}:{os.minor(device)}'
    sysfs = (Path('/sys/dev/block') / number).resolve(strict=True)
    if (sysfs / 'partition').is_file():
        number = (sysfs.parent / 'dev').read_text().strip()
    major, minor = number.split(':')
    if not major.isdigit() or not minor.isdigit():
        raise ValueError(f'Invalid sysfs block device: {number}')
    return number


benchmark.data_device = cgroup_block_device
if __name__ == '__main__':
    sys.exit(benchmark.main())
