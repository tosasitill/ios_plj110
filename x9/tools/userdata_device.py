"""Resolve Android userdata without depending on the boot's UFS sdX order.

All commands are read-only. A private root manifest additionally binds the
live GPT partition UUID and geometry; dm-default-key must route to that same
physical partition with zero offsets. Never print the dm table's key field.
"""
import re
import shlex
import uuid


def resolve_userdata(read, manifest=None):
    device = read('readlink -f /dev/block/by-name/userdata').strip()
    if not re.fullmatch(r'/dev/block/[A-Za-z0-9_]+', device):
        raise ValueError('cannot resolve physical userdata by-name link')
    name = device.rsplit('/', 1)[1]
    sysfs = '/sys/class/block/' + name
    fields = dict(line.split('=', 1) for line in
                  read('cat ' + shlex.quote(sysfs + '/uevent')).splitlines()
                  if '=' in line)
    if (fields.get('DEVNAME') != name or
            fields.get('DEVTYPE') != 'partition' or
            fields.get('PARTNAME') != 'userdata'):
        raise ValueError('userdata link does not identify the userdata partition')
    dev = read('cat ' + shlex.quote(sysfs + '/dev')).strip()
    if (not re.fullmatch(r'\d+:\d+', dev) or
            dev != fields.get('MAJOR', '') + ':' + fields.get('MINOR', '')):
        raise ValueError('userdata device identity changed')
    if manifest is not None:
        try:
            guid = uuid.UUID(fields['PARTUUID']).bytes_le.hex()
        except (KeyError, ValueError) as exc:
            raise ValueError('userdata GPT UUID unavailable') from exc
        if guid != manifest['partition_guid_hex'].lower():
            raise ValueError('userdata GPT UUID changed')
        start = int(read('cat ' + shlex.quote(sysfs + '/start')).strip())
        size = int(read('cat ' + shlex.quote(sysfs + '/size')).strip())
        if (start != manifest['partition_first'] * 8 or
                size != manifest['partition_blocks'] * 8):
            raise ValueError('userdata geometry changed')
    # Keep the dm key only in memory; errors never contain the raw table.
    tokens = read('dmctl table userdata').split()
    try:
        i = next(i for i, value in enumerate(tokens)
                 if value.rstrip(',') == 'default-key')
    except StopIteration as exc:
        raise ValueError('unexpected userdata dm target') from exc
    if (len(tokens) <= i + 5 or tokens[i + 1] != 'aes-xts-plain64' or
            tokens[i + 3] != '0' or tokens[i + 4] != dev or
            tokens[i + 5] != '0'):
        raise ValueError('default-key route or offsets changed')
    return device
