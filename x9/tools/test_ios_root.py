"""Dependency closure for byte-preserved iOS bootstrap RAM roots."""
from pathlib import Path
import struct
import plistlib
import tempfile
import unittest
from build_ios_root import dependencies, safe_path, stage_root


def macho(deps=()):
    commands=[]
    for path,weak in deps:
        s=path.encode()+b'\0';n=(24+len(s)+7)&~7
        commands.append(struct.pack('<6I',0x80000018 if weak else 12,n,24,0,0,0)+s+bytes(n-24-len(s)))
    blob=b''.join(commands)
    return struct.pack('<8I',0xfeedfacf,0x100000c,0,2,len(commands),len(blob),0,0)+blob


class IOSRootTests(unittest.TestCase):
    def test_dependency_commands_preserve_weak_contracts_and_reject_bad_bounds(self):
        b=macho([('/usr/lib/libSystem.B.dylib',False),('/usr/lib/optional.dylib',True)])
        self.assertEqual(dependencies(b),[('/usr/lib/libSystem.B.dylib',False),('/usr/lib/optional.dylib',True)])
        for bad in [b[:20],b[:-1],b'not MachO']:
            with self.assertRaises(ValueError): dependencies(bad)
        with self.assertRaises(ValueError): dependencies(macho([('@rpath/a',False)]))
        for path in ['../host','/usr/../host','usr/lib/a']:
            with self.assertRaises(ValueError): safe_path(path)

    def test_closure_copies_original_system_binaries_and_required_restore_libraries(self):
        with tempfile.TemporaryDirectory(prefix='x9-ios-root-') as d:
            d=Path(d);system=d/'system';restore=d/'restore';target=d/'root'
            for rel in ['sbin/launchd','usr/lib/dyld','usr/libexec/xpcproxy']:
                p=system/rel;p.parent.mkdir(parents=True,exist_ok=True)
                p.write_bytes(macho([('/usr/lib/libSystem.B.dylib',False),('/usr/lib/optional.dylib',True)]))
            p=restore/'usr/lib/libSystem.B.dylib';p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(macho())
            (restore/'usr/lib/optional.dylib').write_bytes(macho())
            config=['private/etc/passwd','private/etc/group','private/etc/master.passwd','private/etc/hosts','private/etc/ttys','System/Library/CoreServices/SystemVersion.plist']
            for rel in config:
                p=system/rel;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(b'original system config')
            result=stage_root(system,restore,target)
            self.assertFalse(result['probe_included']);self.assertFalse(result['userland_modified'])
            self.assertEqual((target/'sbin/launchd').read_bytes(),(system/'sbin/launchd').read_bytes())
            self.assertEqual((target/'usr/lib/libSystem.B.dylib').read_bytes(),macho())
            self.assertFalse((target/'usr/lib/optional.dylib').exists())
            self.assertIn('/usr/lib/optional.dylib',result['optional_weak_dependencies_not_included'])
            self.assertEqual((target/'etc').readlink().as_posix(),'private/etc')
            self.assertFalse((target/'usr/appleinternal/sbin/launchd.x9probe').exists())
            self.assertTrue((target/'dev').is_dir())

    def test_selected_official_job_cache_only_references_supplied_services(self):
        with tempfile.TemporaryDirectory(prefix='x9-ios-jobs-') as d:
            d=Path(d);system=d/'system';restore=d/'restore'
            for rel in ['sbin/launchd','usr/lib/dyld','usr/libexec/xpcproxy','usr/sbin/notifyd']:
                p=system/rel;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(macho())
            for rel in ['private/etc/passwd','private/etc/group','private/etc/master.passwd',
                        'private/etc/hosts','private/etc/ttys','System/Library/CoreServices/SystemVersion.plist']:
                p=system/rel;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(b'original')
            path='/System/Library/LaunchDaemons/com.apple.notifyd.plist'
            job={'Label':'com.apple.notifyd','ProgramArguments':['/usr/sbin/notifyd']}
            p=system/path.lstrip('/');p.parent.mkdir(parents=True,exist_ok=True)
            original=plistlib.dumps(job);p.write_bytes(original)
            p=system/'System/Library/xpc/launchd.plist';p.parent.mkdir(parents=True)
            p.write_bytes(plistlib.dumps({'VersionNumber':7,'LaunchDaemons':{
                path:job,'/System/Library/LaunchDaemons/restore.plist':{'Program':'/usr/sbin/restore'}}}))
            target=d/'root';result=stage_root(system,restore,target,['notifyd'])
            selected=plistlib.loads((target/'System/Library/xpc/launchd.plist').read_bytes())
            self.assertEqual(selected['LaunchDaemons'],{path:job})
            self.assertEqual((target/path.lstrip('/')).read_bytes(),original)
            self.assertEqual(result['owners']['/usr/sbin/notifyd'],[0,0])
            self.assertEqual(result['services'],['notifyd'])
            self.assertFalse((target/'usr/sbin/restore').exists())

    def test_required_library_absence_is_fatal(self):
        with tempfile.TemporaryDirectory(prefix='x9-ios-missing-') as d:
            d=Path(d);p=d/'system/sbin/launchd';p.parent.mkdir(parents=True);p.write_bytes(macho([('/usr/lib/missing',False)]))
            # Initial dyld is mandatory too: either missing input must fail before image build.
            with self.assertRaises(FileNotFoundError): stage_root(d/'system',d/'restore',d/'root')


if __name__ == '__main__': unittest.main()
