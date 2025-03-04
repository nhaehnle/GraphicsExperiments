
#define AS_GROUP_SIZE 32
#define MAX_LOD_COUNT 5

struct SceneProperties {
    float3      EyePosition;
    float4x4    CameraVP;
    uint        InstanceCount;
    uint        MeshletCount;
    uint        __pad0;
    float       MaxLODDistance;
    uint        Meshlet_LOD_Offsets[5];
    uint        Meshlet_LOD_Counts[5];
    uint        __pad1[3];
    float3      MeshBoundsMin;
    float3      MeshBoundsMax;
};

ConstantBuffer<SceneProperties> Scene : register(b0);

struct Vertex {
    float3 Position;
};

struct Meshlet {
	uint VertexOffset;
	uint TriangleOffset;
	uint VertexCount;
	uint TriangleCount;
};

struct Instance
{
    float4x4 M;
};

StructuredBuffer<Vertex>   Vertices        : register(t1);
StructuredBuffer<Meshlet>  Meshlets        : register(t2);
StructuredBuffer<float4>   MeshletBounds   : register(t3);
StructuredBuffer<uint>     VertexIndices   : register(t4);
StructuredBuffer<uint>     TriangleIndices : register(t5);
StructuredBuffer<Instance> Instances       : register(t6);

struct MeshOutput {
    float4 Position : SV_POSITION;
    float3 Color    : COLOR;
};

struct Payload {
    uint InstanceIndices[AS_GROUP_SIZE];
    uint MeshletIndices[AS_GROUP_SIZE];
};

groupshared Payload sPayload;

// -------------------------------------------------------------------------------------------------
// Amplification Shader
// -------------------------------------------------------------------------------------------------
[numthreads(AS_GROUP_SIZE, 1, 1)]
void asmain(
    uint gtid : SV_GroupThreadID,
    uint dtid : SV_DispatchThreadID,
    uint gid  : SV_GroupID
)
{
    bool visible = false;

    uint instanceIndex = dtid / Scene.MeshletCount;
    uint meshletIndex  = dtid % Scene.MeshletCount;

    // Make sure instance index is within bounds
    if (instanceIndex < Scene.InstanceCount) {
        // Instance's model transform matrix
        float4x4 M  = Instances[instanceIndex].M;

        // Get center of transformed bounding box to use in LOD distance calculation
        float4 instanceBoundsMinWS = mul(M, float4(Scene.MeshBoundsMin, 1.0));
        float4 instanceBoundsMaxWS = mul(M, float4(Scene.MeshBoundsMax, 1.0));
        float4 instanceCenter = (instanceBoundsMinWS + instanceBoundsMaxWS) / 2.0;

        // Distance between transformed bounding box and camera eye position
        float dist = distance(instanceCenter.xyz, Scene.EyePosition);

        // Normalize distance using MaxLODDistance
        float ndist = clamp(dist / Scene.MaxLODDistance, 0.0, 1.0);

        // Calculate LOD using normalized distance
        uint lod = (uint)(pow(ndist, 0.65) * (MAX_LOD_COUNT - 1));

        // Get meshlet count for the LOD
        uint lodMeshletCount = Scene.Meshlet_LOD_Counts[lod];

        // Make sure meshlet index is within bounds of current LOD's meshlet count
        if (meshletIndex < lodMeshletCount) {
            meshletIndex += Scene.Meshlet_LOD_Offsets[lod];
            
            // Assuming visibile, no culling here
            visible = 1;
        }
    }

    if (visible) {
        uint index = WavePrefixCountBits(visible);
        sPayload.InstanceIndices[index] = instanceIndex;
        sPayload.MeshletIndices[index]  = meshletIndex;
    }
    
    uint visibleCount = WaveActiveCountBits(visible);    
    DispatchMesh(visibleCount, 1, 1, sPayload); 
}

// -------------------------------------------------------------------------------------------------
// Mesh Shader
// -------------------------------------------------------------------------------------------------
[outputtopology("triangle")]
[numthreads(128, 1, 1)]
void msmain(
                 uint       gtid : SV_GroupThreadID, 
                 uint       gid  : SV_GroupID, 
     in payload  Payload    payload, 
    out indices  uint3      triangles[128],
    out vertices MeshOutput vertices[64]) 
{
    uint instanceIndex = payload.InstanceIndices[gid];
    uint meshletIndex  = payload.MeshletIndices[gid];

    Meshlet m = Meshlets[meshletIndex];
    SetMeshOutputCounts(m.VertexCount, m.TriangleCount);
       
    if (gtid < m.TriangleCount) {
        //
        // meshopt stores the triangle offset in bytes since it stores the
        // triangle indices as 3 consecutive bytes. 
        //
        // Since we repacked those 3 bytes to a 32-bit uint, our offset is now
        // aligned to 4 and we can easily grab it as a uint without any 
        // additional offset math.
        //
        uint packed = TriangleIndices[m.TriangleOffset + gtid];
        uint vIdx0  = (packed >>  0) & 0xFF;
        uint vIdx1  = (packed >>  8) & 0xFF;
        uint vIdx2  = (packed >> 16) & 0xFF;
        triangles[gtid] = uint3(vIdx0, vIdx1, vIdx2);
    }

    if (gtid < m.VertexCount) {
        uint vertexIndex = m.VertexOffset + gtid;        
        vertexIndex = VertexIndices[vertexIndex];

        float4x4 MVP = mul(Scene.CameraVP, Instances[instanceIndex].M);

        vertices[gtid].Position = mul(MVP, float4(Vertices[vertexIndex].Position, 1.0));
        
        float3 color = float3(
            float(meshletIndex & 1),
            float(meshletIndex & 3) / 4,
            float(meshletIndex & 7) / 8);
        vertices[gtid].Color = color;
    }    
}

// -------------------------------------------------------------------------------------------------
// Pixel Shader
// -------------------------------------------------------------------------------------------------
float4 psmain(MeshOutput input) : SV_TARGET
{
    return float4(input.Color, 1);
}
