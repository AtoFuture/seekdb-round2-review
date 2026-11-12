#!/usr/bin/env python3
"""Resolve a Linux filesystem partition to its cgroup-accounted block device.

The unchanged collector still rejects missing I/O counters. This wrapper only
resolves the kernel's partition/whole-device identity through sysfs.
"""
import os
from pathlib import Path
import sys
import json
import run_contract_benchmark as benchmark


def cgroup_block_device(path):
    device = os.stat(path, follow_symlinks=False).st_dev
    number = f'{os.major(device)}:{os.minor(device)}'
    sysfs = (Path('/sys/dev/block') / number).resolve(strict=True)
    if (sysfs / 'partition').is_file():
        number = (sysfs.parent / 'dev').read_text().strip()
    major, minor = number.split(':')
    if not major.isdigit() or not minor.isdigit():
        raise ValueError('Invalid sysfs block device: ' + number)
    return number


benchmark.data_device = cgroup_block_device
# Shared CI hosts can publish cgroup writeback counters later than file fsync.
# Freeze a 30-second ceiling for BOTH versions before the run. Keep the same
# clean-and-stable condition and account every final byte; never sample early.
# The timed SQL phase has already ended before this resource drain begins.
original_drain = benchmark.drain


def shared_host_drain(resources, root, timeout=30.0, interval=0.1):
    try:
        return original_drain(resources, root, timeout=timeout, interval=interval)
    except TimeoutError as error:
        print('RESOURCE_DRAIN_FAILURE ' + json.dumps(resources.sample()), flush=True)
        raise TimeoutError('data writeback did not drain within 30 seconds') from error


benchmark.drain = shared_host_drain
if __name__ == '__main__':
    sys.exit(benchmark.main())
