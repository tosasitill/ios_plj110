#!/usr/bin/env python3
"""Host-only exercise of the actual diagnostic temp preflight with vnode stubs."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class DiagnosticTemp(unittest.TestCase):
    def test_preflight_and_scoping(self):
        source = (ROOT / 'x9/kernel/src/bsd/kern/x9_bluetooth_root.inc').read_text()
        header = (ROOT / 'x9/bluetooth/settings_header.py').read_text()
        self.assertIn('#define X9_BT_ROOT_SETTINGS_ONLY 1', header)
        func = source.split('static int x9_btr_diag_temp_absent(void)', 1)[1].split('\n#endif', 1)[0]
        self.assertIn('#if !X9_BT_ROOT_SETTINGS_ONLY\n for(unsigned i=0;i<count;i++){char tmp[320];', source)
        self.assertIn('int e=0; /* O_EXCL below:', source)
        harness = r'''
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#define NULLVP NULL
#define VNODE_LOOKUP_NOFOLLOW 1
typedef void *vnode_t;
struct file {const char *path; int orig_bytes, prev_count;};
static struct file x9_btr_files[]={{"/usr/lib/X9SettingsBTDiag.dylib",0,0}};
static int lookup_result, put_count;
static int x9_btr_name(char *o,size_t n,const char *p,const char *s) {return snprintf(o,n,"%s%s",p,s)>=(int)n?E2BIG:0;}
static void *vfs_context_kernel(void) {return NULL;}
static int vnode_lookup(const char *p,int flags,vnode_t *v,void *ctx) {
 (void)flags;(void)ctx;
 if(strcmp(p,"/usr/lib/X9SettingsBTDiag.dylib.x9-bt-temp")) return EINVAL;
 *v=(void *)1;return lookup_result;
}
static void vnode_put(vnode_t v) {(void)v;put_count++;}
static int x9_btr_diag_temp_absent(void)''' + func + r'''
int main(void) {
 lookup_result=ENOENT;if(x9_btr_diag_temp_absent()!=0)return 1;
 lookup_result=0;if(x9_btr_diag_temp_absent()!=EEXIST||put_count!=1)return 2;
 lookup_result=EACCES;if(x9_btr_diag_temp_absent()!=EACCES)return 3;
 x9_btr_files[0].prev_count=1;if(x9_btr_diag_temp_absent()!=EINVAL)return 4;
 x9_btr_files[0].prev_count=0;x9_btr_files[0].path="/wrong";
 if(x9_btr_diag_temp_absent()!=EINVAL)return 5;
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as d:
            c = pathlib.Path(d) / 'test.c'
            c.write_text(harness)
            exe = pathlib.Path(d) / 'test'
            subprocess.run(['cc', '-Wall', '-Werror', str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
