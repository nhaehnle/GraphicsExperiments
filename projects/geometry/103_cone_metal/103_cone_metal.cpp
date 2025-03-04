#include "window.h"

#include "mtl_renderer.h"
#include "tri_mesh.h"

#include <glm/glm.hpp>
#include <glm/matrix.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/transform.hpp>
using namespace glm;

#define CHECK_CALL(FN)                                                               \
    {                                                                                \
        NS::Error* pError = FN;                                                      \
        if (pError != nullptr) {                                                     \
            std::stringstream ss;                                                    \
            ss << "\n";                                                              \
            ss << "*** FUNCTION CALL FAILED *** \n";                                 \
            ss << "FUNCTION: " << #FN << "\n";                                       \
            ss << "Error: " << pError->localizedDescription()->utf8String() << "\n"; \
            ss << "\n";                                                              \
            GREX_LOG_ERROR(ss.str().c_str());                                        \
            assert(false);                                                           \
        }                                                                            \
    }

// =============================================================================
// Shader code
// =============================================================================
const char* gShaders = R"(
#include <metal_stdlib>
using namespace metal;

struct Camera {
	float4x4 MVP;
};

struct VSOutput {
	float4 PositionCS [[position]];
	float3 Color;
};

struct VertexData {
	float3 PositionOS [[attribute(0)]];
	float3 Color [[attribute(1)]];
};

VSOutput vertex vertexMain(
	VertexData vertexData [[stage_in]],
	constant Camera &Cam [[buffer(2)]])
{
	VSOutput output;
	float3 position = vertexData.PositionOS;
	output.PositionCS = Cam.MVP * float4(position, 1.0f);
	output.Color = vertexData.Color;
	return output;
}

float4 fragment fragmentMain( VSOutput in [[stage_in]] )
{
	return float4(in.Color, 1.0);
}
)";

// =============================================================================
// Globals
// =============================================================================
static uint32_t gWindowWidth  = 1280;
static uint32_t gWindowHeight = 720;
static bool     gEnableDebug  = true;

void CreateGeometryBuffers(
    MetalRenderer* pRenderer,
    MetalBuffer*   pIndexBuffer,
    MetalBuffer*   pPositionBuffer,
    MetalBuffer*   pVertexColorBuffer,
    uint32_t*      pNumIndices,
    MetalBuffer*   pTBNVertexBuffer,
    uint32_t*      pNumTBNVertices);

// =============================================================================
// main()
// =============================================================================
int main(int argc, char** argv)
{
    std::unique_ptr<MetalRenderer> renderer = std::make_unique<MetalRenderer>();

    if (!InitMetal(renderer.get(), gEnableDebug)) {
        return EXIT_FAILURE;
    }

    // *************************************************************************
    // Compile shaders
    // *************************************************************************
    MetalShader vsShader;
    MetalShader fsShader;
    NS::Error*  pError  = nullptr;
    auto        library = NS::TransferPtr(renderer->Device->newLibrary(
        NS::String::string(gShaders, NS::UTF8StringEncoding),
        nullptr,
        &pError));

    if (library.get() == nullptr) {
        std::stringstream ss;
        ss << "\n"
           << "Shader compiler error (VS): " << pError->localizedDescription()->utf8String() << "\n";
        GREX_LOG_ERROR(ss.str().c_str());
        assert(false);
        return EXIT_FAILURE;
    }

    vsShader.Function = NS::TransferPtr(library->newFunction(NS::String::string("vertexMain", NS::UTF8StringEncoding)));
    if (vsShader.Function.get() == nullptr) {
        assert(false && "VS Shader MTL::Library::newFunction() failed");
        return EXIT_FAILURE;
    }

    fsShader.Function = NS::TransferPtr(library->newFunction(NS::String::string("fragmentMain", NS::UTF8StringEncoding)));
    if (fsShader.Function.get() == nullptr) {
        assert(false && "FS Shader MTL::Library::newFunction() failed");
        return EXIT_FAILURE;
    }

    // *************************************************************************
    // Graphics pipeline state object
    // *************************************************************************
    MetalPipelineRenderState trianglePipelineState;
    MetalDepthStencilState   triangleDepthStencilState;
    CHECK_CALL(CreateDrawVertexColorPipeline(
        renderer.get(),
        &vsShader,
        &fsShader,
        GREX_DEFAULT_RTV_FORMAT,
        GREX_DEFAULT_DSV_FORMAT,
        &trianglePipelineState,
        &triangleDepthStencilState));

    MetalPipelineRenderState tbnDebugPipelineState;
    MetalDepthStencilState   tbnDebugDepthStencilState;
    CHECK_CALL(CreateDrawVertexColorPipeline(
        renderer.get(),
        &vsShader,
        &fsShader,
        GREX_DEFAULT_RTV_FORMAT,
        GREX_DEFAULT_DSV_FORMAT,
        &tbnDebugPipelineState,
        &tbnDebugDepthStencilState,
        MTL::PrimitiveTopologyClassLine,
        METAL_PIPELINE_FLAGS_INTERLEAVED_ATTRS));

    // *************************************************************************
    // Geometry data
    // *************************************************************************
    MetalBuffer indexBuffer;
    MetalBuffer positionBuffer;
    MetalBuffer vertexColorBuffer;
    uint32_t    numIndices = 0;
    MetalBuffer tbnDebugVertexBuffer;
    uint32_t    tbnDebugNumVertices = 0;
    CreateGeometryBuffers(
        renderer.get(),
        &indexBuffer,
        &positionBuffer,
        &vertexColorBuffer,
        &numIndices,
        &tbnDebugVertexBuffer,
        &tbnDebugNumVertices);

    // *************************************************************************
    // Window
    // *************************************************************************
    auto window = Window::Create(gWindowWidth, gWindowHeight, "103_cone_metal");
    if (!window) {
        assert(false && "Window::Create failed");
        return EXIT_FAILURE;
    }

    // *************************************************************************
    // Render Pass Description
    // *************************************************************************
    MTL::RenderPassDescriptor* pRenderPassDescriptor = MTL::RenderPassDescriptor::renderPassDescriptor();

    // *************************************************************************
    // Swapchain
    // *************************************************************************
    if (!InitSwapchain(renderer.get(), window->GetNativeWindow(), window->GetWidth(), window->GetHeight(), 2, MTL::PixelFormatDepth32Float)) {
        assert(false && "InitSwapchain failed");
        return EXIT_FAILURE;
    }

    // *************************************************************************
    // Main loop
    // *************************************************************************
    MTL::ClearColor clearColor(0.23f, 0.23f, 0.31f, 0);
    uint32_t        frameIndex = 0;

    while (window->PollEvents()) {
        CA::MetalDrawable* pDrawable = renderer->pSwapchain->nextDrawable();
        assert(pDrawable != nullptr);

        uint32_t swapchainIndex = (frameIndex % renderer->SwapchainBufferCount);

        auto colorTargetDesc = NS::TransferPtr(MTL::RenderPassColorAttachmentDescriptor::alloc()->init());
        colorTargetDesc->setClearColor(clearColor);
        colorTargetDesc->setTexture(pDrawable->texture());
        colorTargetDesc->setLoadAction(MTL::LoadActionClear);
        colorTargetDesc->setStoreAction(MTL::StoreActionStore);
        pRenderPassDescriptor->colorAttachments()->setObject(colorTargetDesc.get(), 0);

        auto depthTargetDesc = NS::TransferPtr(MTL::RenderPassDepthAttachmentDescriptor::alloc()->init());
        depthTargetDesc->setClearDepth(1);
        depthTargetDesc->setTexture(renderer->SwapchainDSVBuffers[swapchainIndex].get());
        depthTargetDesc->setLoadAction(MTL::LoadActionClear);
        depthTargetDesc->setStoreAction(MTL::StoreActionDontCare);
        pRenderPassDescriptor->setDepthAttachment(depthTargetDesc.get());

        MTL::CommandBuffer*        pCommandBuffer = renderer->Queue->commandBuffer();
        MTL::RenderCommandEncoder* pRenderEncoder = pCommandBuffer->renderCommandEncoder(pRenderPassDescriptor);

        pRenderEncoder->setRenderPipelineState(trianglePipelineState.State.get());
        pRenderEncoder->setDepthStencilState(triangleDepthStencilState.State.get());

        float t        = static_cast<float>(glfwGetTime());
        mat4  modelMat = glm::rotate(t, vec3(1, 0, 0));
        mat4  viewMat  = lookAt(vec3(0, 1, 2), vec3(0, 0, 0), vec3(0, 1, 0));
        mat4  projMat  = perspective(radians(60.0f), gWindowWidth / static_cast<float>(gWindowHeight), 0.1f, 10000.0f);

        mat4 mvpMat = projMat * viewMat * modelMat;

        pRenderEncoder->setVertexBytes(&mvpMat, sizeof(glm::mat4), 2);

        MTL::Buffer* vbvs[2]    = {positionBuffer.Buffer.get(), vertexColorBuffer.Buffer.get()};
        NS::UInteger offsets[2] = {0, 0};
        NS::Range    vbRange(0, 2);
        pRenderEncoder->setVertexBuffers(vbvs, offsets, vbRange);

        pRenderEncoder->setFrontFacingWinding(MTL::WindingCounterClockwise);
        pRenderEncoder->setCullMode(MTL::CullModeBack);

        pRenderEncoder->drawIndexedPrimitives(
            MTL::PrimitiveType::PrimitiveTypeTriangle,
            numIndices,
            MTL::IndexTypeUInt32,
            indexBuffer.Buffer.get(),
            0);

        // TBN debug
        {
            pRenderEncoder->setRenderPipelineState(tbnDebugPipelineState.State.get());
            pRenderEncoder->setDepthStencilState(tbnDebugDepthStencilState.State.get());

            pRenderEncoder->setVertexBuffer(tbnDebugVertexBuffer.Buffer.get(), 0, 0);

            pRenderEncoder->setCullMode(MTL::CullModeNone);

            pRenderEncoder->drawPrimitives(MTL::PrimitiveTypeLine, 0, tbnDebugNumVertices, 1);
        }

        pRenderEncoder->endEncoding();

        pCommandBuffer->presentDrawable(pDrawable);
        pCommandBuffer->commit();
    }

    return 0;
}

void CreateGeometryBuffers(
    MetalRenderer* pRenderer,
    MetalBuffer*   pIndexBuffer,
    MetalBuffer*   pPositionBuffer,
    MetalBuffer*   pVertexColorBuffer,
    uint32_t*      pNumIndices,
    MetalBuffer*   pTBNVertexBuffer,
    uint32_t*      pNumTBNVertices)
{
    TriMesh::Options options;
    options.enableVertexColors = true;
    options.enableNormals      = true;
    options.enableTangents     = true;

    TriMesh mesh = TriMesh::Cone(1, 1, 32, options);

    CHECK_CALL(CreateBuffer(
        pRenderer,
        SizeInBytes(mesh.GetTriangles()),
        DataPtr(mesh.GetTriangles()),
        pIndexBuffer));

    CHECK_CALL(CreateBuffer(
        pRenderer,
        SizeInBytes(mesh.GetPositions()),
        DataPtr(mesh.GetPositions()),
        pPositionBuffer));

    CHECK_CALL(CreateBuffer(
        pRenderer,
        SizeInBytes(mesh.GetVertexColors()),
        DataPtr(mesh.GetVertexColors()),
        pVertexColorBuffer));

    *pNumIndices = 3 * mesh.GetNumTriangles();

    auto tbnVertexData = mesh.GetTBNLineSegments(pNumTBNVertices);
    CHECK_CALL(CreateBuffer(
        pRenderer,
        SizeInBytes(tbnVertexData),
        DataPtr(tbnVertexData),
        pTBNVertexBuffer));
}
