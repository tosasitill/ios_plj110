"""Set HFS+ catalog ownership in a detached build input without host root.
Only BSD owner/group fields change; executable data forks remain untouched.
"""
import struct


def set_catalog_owners(image, owners=None):
    owners = owners or {}
    data = bytearray(image)
    if data[1024:1026] not in (b'H+', b'HX'):
        raise ValueError('expected a raw HFS+ volume')
    block = struct.unpack_from('>I', data, 1064)[0]
    length = struct.unpack_from('>Q', data, 1024+272)[0]
    extents = [struct.unpack_from('>II', data, 1024+288+i*8) for i in range(8)]
    if not block or length > sum(n*block for _,n in extents):
        raise ValueError('catalog overflow extents require explicit support')
    catalog = bytearray()
    for start,count in extents:
        if (start+count)*block > len(data):
            raise ValueError('catalog extent exceeds volume')
        catalog.extend(data[start*block:(start+count)*block])
    catalog = catalog[:length]
    if len(catalog)<34:
        raise ValueError('truncated catalog header')
    node_size = struct.unpack_from('>H', catalog, 32)[0]
    leaf = struct.unpack_from('>I', catalog, 24)[0]
    records,visited={},set()
    while leaf:
        if leaf in visited:
            raise ValueError('catalog leaf cycle')
        visited.add(leaf)
        base=leaf*node_size
        if node_size<256 or base+node_size>len(catalog) or catalog[base+8]!=255:
            raise ValueError('invalid catalog leaf')
        count=struct.unpack_from('>H',catalog,base+10)[0]
        if count>(node_size-14)//2-1:
            raise ValueError('invalid record count')
        for i in range(count):
            offset=struct.unpack_from('>H',catalog,base+node_size-2*(i+1))[0]
            end=struct.unpack_from('>H',catalog,base+node_size-2*(i+2))[0]
            if not 14<=offset<end<=node_size-2*(count+1):
                raise ValueError('invalid catalog record bounds')
            pos=base+offset
            keylen,parent,chars=struct.unpack_from('>HIH',catalog,pos)
            body=pos+2+keylen
            if keylen!=6+2*chars or body+2>base+end:
                raise ValueError('invalid catalog key')
            name=catalog[pos+8:body].decode('utf-16be')
            kind=struct.unpack_from('>H',catalog,body)[0]
            if kind not in (1,2):
                continue
            if body+48>base+end:
                raise ValueError('truncated permissions record')
            cnid=struct.unpack_from('>I',catalog,body+8)[0]
            if cnid in records:
                raise ValueError('duplicate catalog ID')
            records[cnid]=(parent,name,body)
        leaf=struct.unpack_from('>I',catalog,base)[0]
    paths={2:'/'}
    def path_for(cnid,ancestors=()):
        if cnid in paths:return paths[cnid]
        if cnid in ancestors or cnid not in records:
            raise ValueError('missing or cyclic catalog parent')
        parent,name,_=records[cnid]
        path=path_for(parent,ancestors+(cnid,)).rstrip('/')+'/'+name
        paths[cnid]=path
        return path
    changes=[]
    for cnid,(_,_,body) in records.items():
        path=path_for(cnid)
        owner=owners.get(path)
        if owner is None:
            owner=(501,501) if path=='/private/var/mobile' or path.startswith('/private/var/mobile/') else (0,0)
        uid,gid=owner
        old=struct.unpack_from('>II',catalog,body+32)
        struct.pack_into('>II',catalog,body+32,uid,gid)
        changes.append({'path':path,'old_uid':old[0],'old_gid':old[1],'uid':uid,'gid':gid})
    copied=0
    for start,count in extents:
        size=min(count*block,length-copied)
        if size<=0:break
        data[start*block:start*block+size]=catalog[copied:copied+size]
        copied+=size
    return bytes(data),changes
