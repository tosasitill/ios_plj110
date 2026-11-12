#!/usr/bin/env python3
"""Build complete iOS OS + cryptex contents as a raw HFSX root, offline.
Read-only source mounts. Source executables are copied byte-for-byte.
Does not flash, reboot, or contact a device. No restore jobs are imported.
"""
import argparse
import hashlib
import json
import mmap
import os
from pathlib import Path
import plistlib
import stat
import struct
import subprocess
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent))


def run(args):
    print('RUN',*map(str,args),flush=True)
    p=subprocess.run(list(map(str,args)),capture_output=True)
    if p.returncode:
        print(p.stderr.decode(errors='replace'),flush=True)
        p.check_returncode()
    return p.stdout


def inventory(root):
    result={};errors=[]
    for d,dirs,names in os.walk(root,followlinks=False,onerror=lambda e:errors.append(str(e))):
        for p in [Path(d)]+[Path(d)/n for n in dirs+names]:
            s=p.lstat();rel='/'+p.relative_to(root).as_posix() if p!=root else '/'
            result[rel]={'uid':s.st_uid,'gid':s.st_gid,'mode':s.st_mode&0xffff}
    return result,errors


def mount(image,path,owners):
    data=run(['hdiutil','attach','-readonly','-owners',owners,'-nobrowse','-mountpoint',path,image,'-plist'])
    p=plistlib.loads(data)
    return next(e['dev-entry'] for e in p['system-entities'] if e.get('content-hint')=='GUID_partition_scheme' or (e.get('dev-entry') and not e.get('mount-point') and e['dev-entry'].rsplit('disk',1)[-1].isdigit()))


def patch_catalog(path,metadata):
    with path.open('r+b') as f,mmap.mmap(f.fileno(),0) as image:
        if image[1024:1026] not in (b'H+',b'HX'):raise ValueError('raw HFS image required')
        block=struct.unpack_from('>I',image,1064)[0];total=struct.unpack_from('>I',image,1068)[0]*block
        if not 0<total<=len(image):raise ValueError('invalid HFS geometry')
        size=struct.unpack_from('>Q',image,1296)[0];ext=[struct.unpack_from('>II',image,1312+i*8) for i in range(8)]
        if size>sum(n*block for _,n in ext):raise ValueError('catalog overflow unsupported; increase initial catalog size')
        cat=bytearray()
        for start,n in ext:
            if (start+n)*block>total:raise ValueError('catalog outside volume')
            cat.extend(image[start*block:(start+n)*block])
        del cat[size:];node_size=struct.unpack_from('>H',cat,32)[0];leaf=struct.unpack_from('>I',cat,24)[0]
        records={};visited=set()
        while leaf:
            if leaf in visited:raise ValueError('catalog cycle')
            visited.add(leaf);base=leaf*node_size
            if node_size<256 or base+node_size>len(cat) or cat[base+8]!=255:raise ValueError('invalid catalog node')
            count=struct.unpack_from('>H',cat,base+10)[0]
            for i in range(count):
                off=struct.unpack_from('>H',cat,base+node_size-2*(i+1))[0];end=struct.unpack_from('>H',cat,base+node_size-2*(i+2))[0]
                if not 14<=off<end<=node_size-2*(count+1):raise ValueError('catalog bounds')
                pos=base+off;key,parent,chars=struct.unpack_from('>HIH',cat,pos);body=pos+2+key
                if key!=6+chars*2 or body+2>base+end:raise ValueError('catalog key')
                name=cat[pos+8:body].decode('utf-16be');kind=struct.unpack_from('>H',cat,body)[0]
                if kind not in (1,2):continue
                cnid=struct.unpack_from('>I',cat,body+8)[0];records[cnid]=(parent,name,body)
            leaf=struct.unpack_from('>I',cat,base)[0]
        paths={2:'/'}
        def path_for(n):
            if n in paths:return paths[n]
            parent,name,_=records[n];paths[n]=path_for(parent).rstrip('/')+'/'+name;return paths[n]
        exact=0
        for cnid,(_,_,body) in records.items():
            rel=path_for(cnid);m=metadata.get(rel)
            if m:
                struct.pack_into('>II',cat,body+32,m['uid'],m['gid']);struct.pack_into('>H',cat,body+42,m['mode']);exact+=1
            elif rel.startswith('/.HFS+') or rel in ['/.journal','/.journal_info_block']:pass
            else:struct.pack_into('>II',cat,body+32,0,0)
        copied=0
        for start,n in ext:
            take=min(n*block,size-copied)
            if take<=0:break
            image[start*block:start*block+take]=cat[copied:copied+take];copied+=take
        image.flush()
    with path.open('r+b') as f:f.truncate(total)
    return {'records':len(records),'exact_metadata_records':exact,'volume_bytes':total,'catalog_bytes':size}


def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--out',type=Path,required=True)
    args=ap.parse_args();out=args.out.resolve();sources=Path(__file__).resolve().parents[2]/'.local/apple/ios16-20H392'
    output=out/'ios16-full.hfs';target=out/'root-mount';target.mkdir(exist_ok=True)
    if output.exists():raise ValueError('output exists')
    metadata=json.loads((out/'os-original-metadata.json').read_text());mounts={
        'OS':sources/'mnt-OS','SystemOS':out/'mounts/SystemOS','AppOS':out/'mounts/AppOS'}
    # OS source ownership was recorded using owners=on, then remounted off.
    for name,dev in [('SystemOS','/dev/disk6'),('AppOS','/dev/disk8')]:
        saved=out/(name+'-original-metadata.json')
        if saved.exists():
            prefix='/private/preboot/Cryptexes/'+('OS' if name=='SystemOS' else 'App')
            for rel,value in json.loads(saved.read_text())['metadata'].items():metadata[prefix+(rel if rel!='/' else '')]=value
            continue
        run(['hdiutil','detach',dev]);actual=mount(sources/(name+'.dmg'),mounts[name],'on')
        meta,errors=inventory(mounts[name]);(out/(name+'-original-metadata.json')).write_text(json.dumps({'metadata':meta,'errors':errors},separators=(',',':'))+'\n')
        prefix='/private/preboot/Cryptexes/'+('OS' if name=='SystemOS' else 'App')
        for rel,value in meta.items():metadata[prefix+(rel if rel!='/' else '')]=value
        run(['hdiutil','detach',actual]);mount(sources/(name+'.dmg'),mounts[name],'off')
    # Resolve only inaccessible skeleton metadata with explicit parent-owner
    # normalization. Original executable contents are never changed.
    normalized=[]
    osroot=mounts['OS']
    for d,dirs,names in os.walk(osroot,followlinks=False):
        for p in [Path(d)]+[Path(d)/n for n in dirs+names]:
            rel='/'+p.relative_to(osroot).as_posix() if p!=osroot else '/'
            if rel in metadata:continue
            ancestor=str(Path(rel).parent)
            while ancestor not in metadata and ancestor!='/':ancestor=str(Path(ancestor).parent)
            owner=metadata.get(ancestor,{'uid':0,'gid':0});s=p.lstat()
            metadata[rel]={'uid':owner['uid'],'gid':owner['gid'],'mode':s.st_mode&0xffff};normalized.append(rel)
    (out/'full-original-metadata.json').write_text(json.dumps(metadata,separators=(',',':'))+'\n')
    (out/'normalized-skeleton-metadata.json').write_text(json.dumps(sorted(set(normalized)),indent=2)+'\n')
    container=out/'ios16-full-container.dmg'
    if not container.exists():run(['hdiutil','create','-size','16g','-fs','Case-sensitive HFS+','-fsargs','-b 4096 -I c=65536,a=16384 -c c=65536','-layout','NONE','-type','UDIF','-volname','X9iOS16Full','-nospotlight',container])
    data=run(['hdiutil','attach','-owners','off','-nobrowse','-mountpoint',target,container,'-plist']);mounted=plistlib.loads(data)
    device=next(e['dev-entry'] for e in mounted['system-entities'] if e.get('mount-point'))
    try:
        for name,prefix in [('OS',''),('SystemOS','private/preboot/Cryptexes/OS'),('AppOS','private/preboot/Cryptexes/App')]:
            dest=target/prefix;dest.mkdir(parents=True,exist_ok=True)
            print('COPY',name,flush=True)
            # noacl avoids importing host ACL principals; modes restored from
            # source metadata after detach. No stripping or binary edits.
            if name=='OS' and (out/'resume-os-copy.json').exists():
                # The first ditto completed except an unreadable empty marker.
                # Verify its size, create its empty data fork, then full content
                # verification below decides whether the copy is complete.
                source=mounts[name]/'.file';marker=dest/'.file'
                if not source.is_file() or source.stat().st_size:raise ValueError('unexpected unreadable marker')
                if not marker.exists():marker.touch(mode=0o600)
            else:
                run(['/usr/bin/ditto','--noacl','--rsrc','--extattr',mounts[name],dest])
        # Hash every original regular file while checking the copied bytes.
        # This is a source-content copy check, not firmware unpack/revalidation.
        checked=0;source_bytes=0;hlog=out/'source-content.jsonl'
        with hlog.open('x') as stream:
            for name,prefix in [('OS',''),('SystemOS','private/preboot/Cryptexes/OS'),('AppOS','private/preboot/Cryptexes/App')]:
                root=mounts[name]
                for d,dirs,names in os.walk(root,followlinks=False):
                    for filename in names:
                        src=Path(d)/filename;rel=src.relative_to(root);dst=target/prefix/rel
                        if src.is_symlink():
                            if not dst.is_symlink() or os.readlink(src)!=os.readlink(dst):raise ValueError('symlink differs '+str(rel))
                            continue
                        if not src.is_file():continue
                        if src.stat().st_size==0:a=hashlib.sha256(b'').hexdigest()
                        else:
                            with src.open('rb') as stream_in:a=hashlib.file_digest(stream_in,'sha256').hexdigest()
                        with dst.open('rb') as stream_in:b=hashlib.file_digest(stream_in,'sha256').hexdigest()
                        if a!=b:raise ValueError('content differs '+str(rel))
                        count=src.stat().st_size;source_bytes+=count;checked+=1
                        stream.write(json.dumps({'source':name,'path':rel.as_posix(),'bytes':count,'sha256':a})+'\n')
                        if checked%10000==0:print('VERIFIED SOURCE FILES',checked,'bytes',source_bytes,flush=True)
    finally:run(['hdiutil','detach',device])
    container.rename(output)
    result=patch_catalog(output,metadata)
    print('HASH RAW FULL ROOT',flush=True)
    with output.open('rb') as stream:digest=hashlib.file_digest(stream,'sha256').hexdigest()
    result.update({'image':str(output),'sha256':digest,'source_files_verified':checked,'source_file_bytes':source_bytes,
                   'inputs':['OS.dmg','SystemOS.dmg','AppOS.dmg'],'version':'16.7.16','build':'20H392','format':'raw HFSX; full original file contents; cryptexes populated at original symlink targets',
                   'original_executable_bytes_modified':False,'normalized_skeleton_metadata':sorted(set(normalized)),
                   'fits_legacy512_ram':False,'runtime_requirement':'native XNU UFS extent-backed block device; not full-image RAM load'})
    (out/'full-root.json').write_text(json.dumps(result,indent=2)+'\n');print('FULL ROOT COMPLETE',json.dumps(result),flush=True)


if __name__=='__main__':main()
