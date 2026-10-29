"""Encode a sparse resident RAMDisk using existing SWEETXNU v1 segments.
Zero gaps become segment BSS; the existing loader reconstructs them in RAM.
No filesystem or executable bytes are rewritten by this transport encoding.
"""
import struct

PAGE = 0x4000
TABLE = 0x2a8


def append_ramdisk(container, image, add_to_adt):
    if not image:
        raise ValueError('empty resident RAMDisk')
    magic,version,count=struct.unpack_from('<8sII',container)
    if magic!=b'SWEETXNU' or version!=1 or not 0<count<128:
        raise ValueError('invalid base payload')
    virt,entry,adt_off,adt_size,end,phys,mem=struct.unpack_from('<7Q',container,0x10)
    rd_size=(len(image)+PAGE-1)&~(PAGE-1)
    if end-virt+rd_size+adt_size+2*PAGE>mem:
        raise ValueError('resident root leaves no room for ADT and boot_args')
    pieces=[]
    for i in range(count):
        va,sz,off,size=struct.unpack_from('<4Q',container,TABLE+32*i)
        if off+size>adt_off:
            raise ValueError('base segment exceeds payload')
        pieces.append((va,sz,container[off:off+size]))
    pages=[i for i in range((len(image)+PAGE-1)//PAGE)
           if image[i*PAGE:(i+1)*PAGE].strip(b'\0')]
    # Adaptively merge small gaps if the existing v1 segment limit needs it.
    gap=64
    while True:
        runs=[]
        for index in pages:
            if runs and index-runs[-1][1]<=gap:
                runs[-1][1]=index
            else:runs.append([index,index])
        needed=len(runs)+(1 if not runs or runs[0][0] else 0)
        if count+needed>128 and len(runs)==1 and runs[0][0]:
            runs[0][0]=0;needed=1
        if count+needed<=128:break
        gap*=2
    if not runs:
        pieces.append((end,rd_size,b''))
    else:
        if runs[0][0]:pieces.append((end,runs[0][0]*PAGE,b''))
        for i,(first,last) in enumerate(runs):
            start=first*PAGE
            stop=runs[i+1][0]*PAGE if i+1<len(runs) else rd_size
            pieces.append((end+start,stop-start,image[start:(last+1)*PAGE]))
    root_pa=phys+end-virt
    tree=add_to_adt(container[adt_off:adt_off+adt_size],root_pa,rd_size)
    output=bytearray(TABLE+32*len(pieces));output[:TABLE]=container[:TABLE]
    struct.pack_into('<I',output,12,len(pieces))
    for i,(va,sz,data) in enumerate(pieces):
        offset=len(output)
        output.extend(data);output.extend(bytes(-len(output)&15))
        struct.pack_into('<4Q',output,TABLE+32*i,va,sz,offset,len(data))
    adt_off=len(output);output.extend(tree);output.extend(bytes(-len(output)&15))
    struct.pack_into('<7Q',output,0x10,virt,entry,adt_off,len(tree),end+rd_size,phys,mem)
    return bytes(output),{'ramdisk':(hex(root_pa),hex(rd_size)),
                          'kernel_end':hex(end+rd_size),'size':len(output),
                          'ramdisk_transport':'sparse-zero-segments-v1',
                          'ramdisk_file_bytes':len(image),
                          'ramdisk_stored_bytes':sum(len(data) for _,_,data in pieces[count:]),
                          'ramdisk_segments':len(pieces)-count}
