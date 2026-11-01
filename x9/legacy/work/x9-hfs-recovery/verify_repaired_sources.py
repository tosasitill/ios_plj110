import pathlib,json,hashlib,sys
inventory=pathlib.Path(sys.argv[1]);mount=pathlib.Path(sys.argv[2]);out=pathlib.Path(sys.argv[3]);errors=[];count=0;total=0
prefix={"OS":"","SystemOS":"private/preboot/Cryptexes/OS","AppOS":"private/preboot/Cryptexes/App"}
for line in inventory.open():
 rec=json.loads(line);p=mount/prefix[rec["source"]]/rec["path"]
 try:
  if p.stat().st_size!=rec["bytes"]:raise ValueError("length differs")
  with p.open("rb") as f:digest=hashlib.file_digest(f,"sha256").hexdigest()
  if digest!=rec["sha256"]:raise ValueError("digest differs")
  count+=1;total+=rec["bytes"]
 except Exception as e:errors.append({"source":rec["source"],"path":rec["path"],"error":str(e)})
 if (count+len(errors))%20000==0:print("checked",count+len(errors),"errors",len(errors),flush=True)
out.write_text(json.dumps({"verified":count,"bytes":total,"errors":errors},indent=2)+"\n");sys.exit(bool(errors))
