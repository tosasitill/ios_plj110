"""Program-only updates to the observed 20H392 launchd binary service cache.

Reuse Python's plistlib binary parser/writer, not a second plist parser. Keep
the entire original object region and its object IDs/offsets byte-for-byte;
append four replacement containers and two Program strings, then a new offset
table/trailer. This preserves the existing kernel Siri string-object guards.
The caller must bind/backup the exact original bytes and compare the live file.
"""
import io
import plistlib
import struct

CACHE_PATH = '/System/Library/xpc/launchd.plist'
CACHE_NAME = 'launchd.plist'
CACHE_LIMIT = 1024 * 1024
ORIGINAL_SIZE = 796447
ORIGINAL_GEOMETRY = {'offset_size': 4, 'ref_size': 2, 'num_objects': 22269,
                     'top_object': 0, 'table_offset': 707339}
CANDIDATE_SIZE = 798777
CANDIDATE_GEOMETRY = {'offset_size': 4, 'ref_size': 2, 'num_objects': 22275,
                      'top_object': 22269, 'table_offset': 709645}
ROOT_KEYS = {'AppExtensions', 'LaunchDaemons', 'AppRemovalServices',
             'SystemLibraryTreeState', 'Symlinks', 'VersionNumber'}


def geometry(data):
    if not isinstance(data, bytes) or not 40 <= len(data) <= CACHE_LIMIT or data[:8] != b'bplist00':
        raise ValueError('expected bounded binary launchd cache')
    values = struct.unpack('>6xBBQQQ', data[-32:])
    result = dict(zip(ORIGINAL_GEOMETRY, values))
    width, refs, count, root, table = values
    if (data[-32:-26] != b'\0' * 6 or width != 4 or refs != 2 or
            count < 1 or root >= count or table < 8 or
            table + count * width + 32 != len(data)):
        raise ValueError('unsupported launchd cache binary geometry')
    return result


def derive(original, jobs):
    """Return the deterministic append-only candidate and both exact geometries."""
    old_geometry = geometry(original)
    if len(original) != ORIGINAL_SIZE or old_geometry != ORIGINAL_GEOMETRY:
        raise ValueError('launchd cache does not match the observed original 20H392 geometry')
    parser = plistlib._BinaryPlistParser(dict)
    old = parser.parse(io.BytesIO(original))
    if (not isinstance(old, dict) or set(old) != ROOT_KEYS or
            type(old['VersionNumber']) is not int or old['VersionNumber'] != 7 or
            any(not isinstance(old[key], dict) for key in ROOT_KEYS - {'VersionNumber'}) or
            len(old['LaunchDaemons']) != 533):
        raise ValueError('unexpected original version-7 launchd cache shape')
    count = old_geometry['num_objects']
    table = old_geometry['table_offset']
    offsets = parser._object_offsets
    if (len(offsets) != count or len(set(offsets)) != count or
            any(offset < 8 or offset >= table for offset in offsets)):
        raise ValueError('invalid original object offsets')
    # Keep even unreachable original objects. plistlib performs all decoding.
    for ref in range(count):
        parser._read_object(ref)
    daemons = dict(old['LaunchDaemons'])
    if len(jobs) != 2:
        raise ValueError('only the two native Bluetooth launch jobs are permitted')
    for label, program, args, service, replacement in jobs:
        path = '/System/Library/LaunchDaemons/' + label + '.plist'
        job = daemons.get(path)
        if (not isinstance(job, dict) or job.get('Label') != label or
                job.get('UserName') != 'mobile' or
                job.get('ProgramArguments') != [program, *args] or 'Program' in job or
                not isinstance(job.get('MachServices'), dict) or service not in job['MachServices']):
            raise ValueError('unexpected original cached job identity: ' + label)
        daemons[path] = dict(job, Program=replacement)
    new = dict(old, LaunchDaemons=daemons)
    stream = io.BytesIO()
    stream.write(original[:table])
    writer = plistlib._BinaryPlistWriter(stream, sort_keys=False, skipkeys=False)
    writer._objlist = list(parser._objects)
    writer._objtable = {}
    writer._objidtable = {}
    for ref, obj in enumerate(writer._objlist):
        if isinstance(obj, plistlib._scalars):
            writer._objtable[(type(obj), obj)] = ref
        else:
            writer._objidtable[id(obj)] = ref
    writer._flatten(new)
    # Root + LaunchDaemons + exactly two jobs + exactly two Program values.
    appended = writer._objlist[count:]
    if (len(appended) != 6 or sum(isinstance(obj, dict) for obj in appended) != 4 or
            [obj for obj in appended if isinstance(obj, str)] != [job[4] for job in jobs]):
        raise ValueError('unexpected appended objects; refusing broader cache mutation')
    total = len(writer._objlist)
    if plistlib._count_to_size(total) != old_geometry['ref_size']:
        raise ValueError('cache append would change original reference width')
    writer._ref_size = old_geometry['ref_size']
    writer._ref_format = plistlib._BINARY_FORMAT[writer._ref_size]
    writer._object_offsets = list(offsets) + [0] * len(appended)
    for obj in appended:
        writer._write_object(obj)
    new_table = stream.tell()
    if plistlib._count_to_size(new_table) != old_geometry['offset_size']:
        raise ValueError('cache append would change original offset width')
    stream.write(struct.pack('>' + 'I' * total, *writer._object_offsets))
    stream.write(struct.pack('>6xBBQQQ', 4, 2, total, writer._getrefnum(new), new_table))
    candidate = stream.getvalue()
    if (len(candidate) != CANDIDATE_SIZE or geometry(candidate) != CANDIDATE_GEOMETRY or
            candidate[:table] != original[:table] or
            tuple(writer._object_offsets[:count]) != offsets or
            plistlib.loads(candidate) != new):
        raise ValueError('append-only byte/offset/semantic preservation failed')
    return candidate, old_geometry, geometry(candidate)


def validate_pair(original, candidate, jobs):
    expected, old_geometry, new_geometry = derive(original, jobs)
    if candidate != expected:
        raise ValueError('cache candidate is not the exact two-Program append-only update')
    return {'path': CACHE_PATH, 'method': 'append-only-stdlib-plistlib',
            'original_geometry': old_geometry, 'candidate_geometry': new_geometry}
