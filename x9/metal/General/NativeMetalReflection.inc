// SPDX-License-Identifier: MIT
// UniXNU/AdrenoMetal 7183e588, actual AIR-derived binding metadata.
// ---- pipeline reflection ----
// The `reflection:` of a pipeline's creation, from the runtime's JSON (nmb_*_pipeline_reflection): subclasses of Metal's own reflection
// classes (MTLArgument, which also answers MTLBinding's selectors, MTLStructType, MTLStructMember, MTLArrayType, MTLPointerType,
// MTLTextureReferenceType and the pipeline reflections), so a caller's class checks pass and a getter not answered here is the base
// class's. GFXBench's NGL engine (Aztec Ruins) reads every uniform's offset and every texture's index from it: without it, it binds
// nothing and every draw reads zeros.
@class NBStructType,NBArrayType,NBPointerType,NBTextureReferenceType;
static NSUInteger reflectionValue(NSDictionary *json,NSString *key) {return [json[key] unsignedIntegerValue];}
static NBStructType *reflectionStruct(NSDictionary *json);
static NBArrayType *reflectionArray(NSDictionary *json);
static NBPointerType *reflectionPointer(NSDictionary *json);
static NBTextureReferenceType *reflectionTexture(NSDictionary *json);
@interface NBTextureReferenceType : MTLTextureReferenceType
@property(nonatomic,strong) NSDictionary *json;
@end
@implementation NBTextureReferenceType
- (MTLDataType)dataType {return MTLDataTypeTexture;}
- (MTLDataType)textureDataType {return (MTLDataType)reflectionValue(_json,@"dataType");}
- (MTLTextureType)textureType {return (MTLTextureType)reflectionValue(_json,@"textureType");}
- (MTLBindingAccess)access {return (MTLBindingAccess)reflectionValue(_json,@"access");}
- (BOOL)isDepthTexture {return [_json[@"depth"] boolValue];}
@end
@interface NBPointerType : MTLPointerType
@property(nonatomic,strong) NSDictionary *json;
@end
@implementation NBPointerType
- (MTLDataType)dataType {return MTLDataTypePointer;}
- (MTLDataType)elementType {return (MTLDataType)reflectionValue(_json,@"elementType");}
- (MTLBindingAccess)access {return (MTLBindingAccess)reflectionValue(_json,@"access");}
- (NSUInteger)alignment {return reflectionValue(_json,@"alignment");}
- (NSUInteger)dataSize {return reflectionValue(_json,@"size");}
- (BOOL)elementIsArgumentBuffer {return [_json[@"argumentBuffer"] boolValue];}
- (MTLStructType *)elementStructType {return (MTLStructType *)reflectionStruct(_json[@"struct"]);}
- (MTLArrayType *)elementArrayType {return (MTLArrayType *)reflectionArray(_json[@"array"]);}
@end
@interface NBArrayType : MTLArrayType
@property(nonatomic,strong) NSDictionary *json;
@end
@implementation NBArrayType
- (MTLDataType)dataType {return MTLDataTypeArray;}
- (MTLDataType)elementType {return (MTLDataType)reflectionValue(_json,@"elementType");}
- (NSUInteger)arrayLength {return reflectionValue(_json,@"length");}
- (NSUInteger)stride {return reflectionValue(_json,@"stride");}
- (NSUInteger)argumentIndexStride {return reflectionValue(_json,@"argumentIndexStride");}
- (MTLStructType *)elementStructType {return (MTLStructType *)reflectionStruct(_json[@"struct"]);}
- (MTLArrayType *)elementArrayType {return (MTLArrayType *)reflectionArray(_json[@"array"]);}
- (MTLPointerType *)elementPointerType {return (MTLPointerType *)reflectionPointer(_json[@"pointer"]);}
- (MTLTextureReferenceType *)elementTextureReferenceType {return (MTLTextureReferenceType *)reflectionTexture(_json[@"texture"]);}
@end
@interface NBStructMember : MTLStructMember
@property(nonatomic,strong) NSDictionary *json;
@end
@implementation NBStructMember
- (NSString *)name {return _json[@"name"];}
- (NSUInteger)offset {return reflectionValue(_json,@"offset");}
- (MTLDataType)dataType {return (MTLDataType)reflectionValue(_json,@"dataType");}
- (NSUInteger)argumentIndex {return reflectionValue(_json,@"argumentIndex");}
- (MTLStructType *)structType {return (MTLStructType *)reflectionStruct(_json[@"struct"]);}
- (MTLArrayType *)arrayType {return (MTLArrayType *)reflectionArray(_json[@"array"]);}
- (MTLPointerType *)pointerType {return (MTLPointerType *)reflectionPointer(_json[@"pointer"]);}
- (MTLTextureReferenceType *)textureReferenceType {return (MTLTextureReferenceType *)reflectionTexture(_json[@"texture"]);}
@end
@interface NBStructType : MTLStructType
@property(nonatomic,strong) NSArray *memberList;
@end
@implementation NBStructType
- (MTLDataType)dataType {return MTLDataTypeStruct;}
- (NSArray<MTLStructMember *> *)members {return _memberList;}
- (MTLStructMember *)memberByName:(NSString *)name {
 for(NBStructMember *member in _memberList)if([member.name isEqualToString:name])return member;
 return nil;
}
@end
static NBStructType *reflectionStruct(NSDictionary *json) {
 if(![json isKindOfClass:[NSDictionary class]])return nil;
 NSMutableArray *members=[NSMutableArray array];
 for(NSDictionary *m in json[@"members"]){NBStructMember *member=[NBStructMember new];member.json=m;[members addObject:member];}
 NBStructType *type=[NBStructType new];type.memberList=members;return type;
}
static NBArrayType *reflectionArray(NSDictionary *json) {
 if(![json isKindOfClass:[NSDictionary class]])return nil;
 NBArrayType *type=[NBArrayType new];type.json=json;return type;
}
static NBPointerType *reflectionPointer(NSDictionary *json) {
 if(![json isKindOfClass:[NSDictionary class]])return nil;
 NBPointerType *type=[NBPointerType new];type.json=json;return type;
}
static NBTextureReferenceType *reflectionTexture(NSDictionary *json) {
 if(![json isKindOfClass:[NSDictionary class]])return nil;
 NBTextureReferenceType *type=[NBTextureReferenceType new];type.json=json;return type;
}
// One argument: MTLArgument's getters, and MTLBinding's (with MTLBufferBinding's, MTLTextureBinding's and MTLThreadgroupBinding's),
// whose `type` values are MTLArgumentType's.
@interface NBArgument : MTLArgument
@property(nonatomic,strong) NSDictionary *json;
@property(nonatomic,strong) NBStructType *madeStruct;   // made once: callers walk its members by identity
@end
@implementation NBArgument
- (NSString *)name {return _json[@"name"];}
- (MTLArgumentType)type {return (MTLArgumentType)reflectionValue(_json,@"type");}
- (MTLBindingAccess)access {return (MTLBindingAccess)reflectionValue(_json,@"access");}
- (NSUInteger)index {return reflectionValue(_json,@"index");}
- (BOOL)isActive {return [_json[@"used"] boolValue];}
- (BOOL)isUsed {return self.isActive;}
- (BOOL)isArgument {return [_json[@"argument"] boolValue];}
- (NSUInteger)bufferAlignment {return reflectionValue(_json[@"buffer"],@"alignment");}
- (NSUInteger)bufferDataSize {return reflectionValue(_json[@"buffer"],@"size");}
- (MTLDataType)bufferDataType {return (MTLDataType)reflectionValue(_json[@"buffer"],@"dataType");}
- (MTLStructType *)bufferStructType {
 if(!_madeStruct)_madeStruct=reflectionStruct(_json[@"buffer"][@"struct"]);
 return _madeStruct;
}
- (MTLPointerType *)bufferPointerType {return (MTLPointerType *)reflectionPointer(_json[@"buffer"][@"pointer"]);}
- (NSUInteger)threadgroupMemoryAlignment {return reflectionValue(_json[@"threadgroup"],@"alignment");}
- (NSUInteger)threadgroupMemoryDataSize {return reflectionValue(_json[@"threadgroup"],@"size");}
- (MTLTextureType)textureType {return (MTLTextureType)reflectionValue(_json[@"texture"],@"textureType");}
- (MTLDataType)textureDataType {return (MTLDataType)reflectionValue(_json[@"texture"],@"dataType");}
- (BOOL)isDepthTexture {return [_json[@"texture"][@"depth"] boolValue];}
- (NSUInteger)arrayLength {return _json[@"texture"]?reflectionValue(_json[@"texture"],@"arrayLength"):1;}
- (NSString *)description {return [NSString stringWithFormat:@"<NBArgument %@ type %lu index %lu>",self.name,(unsigned long)self.type,(unsigned long)self.index];}
@end
@interface NBRenderPipelineReflection : MTLRenderPipelineReflection
@property(nonatomic,strong) NSArray *vertexList,*fragmentList;
@end
@implementation NBRenderPipelineReflection
- (NSArray *)vertexArguments {return _vertexList;}
- (NSArray *)fragmentArguments {return _fragmentList;}
- (NSArray *)tileArguments {return @[];}
- (NSArray *)vertexBindings {return _vertexList;}
- (NSArray *)fragmentBindings {return _fragmentList;}
- (NSArray *)tileBindings {return @[];}
- (NSArray *)objectBindings {return @[];}
- (NSArray *)meshBindings {return @[];}
@end
@interface NBComputePipelineReflection : MTLComputePipelineReflection
@property(nonatomic,strong) NSArray *argumentList;
@end
@implementation NBComputePipelineReflection
- (NSArray *)arguments {return _argumentList;}
- (NSArray *)bindings {return _argumentList;}
@end
// The runtime's JSON as objects (nil, with the reason logged, for a refusal).
static NSDictionary *reflectionJSON(NmbReflection *reflection,const char *what) {
 if(!reflection){NBLOG(@"native-metal: %s reflection refused: %@",what,lastError());return nil;}
 const char *text=nmb_reflection_json(reflection);
 NSData *data=text?[NSData dataWithBytes:text length:strlen(text)]:nil;
 NSDictionary *json=data?[NSJSONSerialization JSONObjectWithData:data options:0 error:nil]:nil;
 nmb_reflection_release(reflection);
 if(![json isKindOfClass:[NSDictionary class]]){NBLOG(@"native-metal: %s reflection is not JSON",what);return nil;}
 return json;
}
static NSArray *reflectionArguments(NSArray *bindings) {
 NSMutableArray *arguments=[NSMutableArray array];
 for(NSDictionary *b in bindings)if([b isKindOfClass:[NSDictionary class]]){NBArgument *argument=[NBArgument new];argument.json=b;[arguments addObject:argument];}
 return arguments;
}
// Metal answers a reflection where the options ask for argument or buffer type information.
static bool wantsReflection(MTLPipelineOption options) {return options&(MTLPipelineOptionArgumentInfo|MTLPipelineOptionBufferTypeInfo);}
static id renderReflection(const NmbRenderPipelineDescriptor *p) {
 NSDictionary *json=reflectionJSON(nmb_render_pipeline_reflection(p),"render pipeline");
 if(!json)return nil;
 NBRenderPipelineReflection *reflection=[NBRenderPipelineReflection new];
 reflection.vertexList=reflectionArguments(json[@"vertex"]);reflection.fragmentList=reflectionArguments(json[@"fragment"]);
 return reflection;
}
static id computeReflection(const NmbComputePipelineDescriptor *p) {
 NSDictionary *json=reflectionJSON(nmb_compute_pipeline_reflection(p),"compute pipeline");
 if(!json)return nil;
 NBComputePipelineReflection *reflection=[NBComputePipelineReflection new];reflection.argumentList=reflectionArguments(json[@"bindings"]);
 return reflection;
}

