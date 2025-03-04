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
        if (pError != nullptr)                                                       \
        {                                                                            \
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
	float2 TexCoord;
};

struct VertexData {
	float3 PositionOS [[attribute(0)]];
	float2 TexCoord [[attribute(1)]];
};

VSOutput vertex vertexMain(
	         VertexData vertexData [[stage_in]],
	constant Camera&    Cam        [[buffer(2)]])
{
	VSOutput output;
	float3 position = vertexData.PositionOS;
	output.PositionCS = Cam.MVP * float4(position, 1.0f);
	output.TexCoord = vertexData.TexCoord;
	return output;
}

constexpr sampler Sampler0;

float4 fragment fragmentMain( 
	VSOutput         input [[stage_in]],
	texture2d<float> Tex0  [[texture(0)]])
{
	float4 color = Tex0.sample(Sampler0, input.TexCoord);
	return color;
}
)";

// =============================================================================
// Globals
// =============================================================================
static uint32_t gWindowWidth  = 1280;
static uint32_t gWindowHeight = 720;
static bool     gEnableDebug  = true;

void CreateTexture(MetalRenderer* pRenderer, MetalTexture* pTexture);
void CreateGeometryBuffers(
    MetalRenderer* pRenderer,
    MetalBuffer*   pIndexBuffer,
    MetalBuffer*   pPositionBuffer,
    MetalBuffer*   pTexCoordBuffer);

// =============================================================================
// main()
// =============================================================================
int main(int argc, char** argv)
{
    std::unique_ptr<MetalRenderer> renderer = std::make_unique<MetalRenderer>();

    if (!InitMetal(renderer.get(), gEnableDebug))
    {
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

    if (library.get() == nullptr)
    {
        std::stringstream ss;
        ss << "\n"
           << "Shader compiler error (VS): " << pError->localizedDescription()->utf8String() << "\n";
        GREX_LOG_ERROR(ss.str().c_str());
        assert(false);
        return EXIT_FAILURE;
    }

    vsShader.Function = NS::TransferPtr(library->newFunction(NS::String::string("vertexMain", NS::UTF8StringEncoding)));
    if (vsShader.Function.get() == nullptr)
    {
        assert(false && "VS Shader MTL::Library::newFunction() failed");
        return EXIT_FAILURE;
    }

    fsShader.Function = NS::TransferPtr(library->newFunction(NS::String::string("fragmentMain", NS::UTF8StringEncoding)));
    if (fsShader.Function.get() == nullptr)
    {
        assert(false && "FS Shader MTL::Library::newFunction() failed");
        return EXIT_FAILURE;
    }

    // *************************************************************************
    // Graphics pipeline state object
    // *************************************************************************
    MetalPipelineRenderState renderPipelineState;
    MetalDepthStencilState   depthStencilState;
    CHECK_CALL(CreateDrawTexturePipeline(
        renderer.get(),
        &vsShader,
        &fsShader,
        GREX_DEFAULT_RTV_FORMAT,
        GREX_DEFAULT_DSV_FORMAT,
        &renderPipelineState,
        &depthStencilState));

    // *************************************************************************
    // Geometry data
    // *************************************************************************
    MetalBuffer indexBuffer;
    MetalBuffer positionBuffer;
    MetalBuffer texCoordBuffer;
    CreateGeometryBuffers(renderer.get(), &indexBuffer, &positionBuffer, &texCoordBuffer);

    // *************************************************************************
    // Texture
    // *************************************************************************
    MetalTexture texture;
    CreateTexture(renderer.get(), &texture);

    // *************************************************************************
    // Window
    // *************************************************************************
    auto window = Window::Create(gWindowWidth, gWindowHeight, "301_textured_cube_metal");
    if (!window)
    {
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
    if (!InitSwapchain(renderer.get(), window->GetNativeWindow(), window->GetWidth(), window->GetHeight(), 2, MTL::PixelFormatDepth32Float))
    {
        assert(false && "InitSwapchain failed");
        return EXIT_FAILURE;
    }

    // *************************************************************************
    // Main loop
    // *************************************************************************
    MTL::ClearColor clearColor(0.23f, 0.23f, 0.31f, 0);
    uint32_t        frameIndex = 0;

    while (window->PollEvents())
    {
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

        pRenderEncoder->setRenderPipelineState(renderPipelineState.State.get());
        pRenderEncoder->setDepthStencilState(depthStencilState.State.get());

        // Update the camera model view projection matrix
        mat4 modelMat = rotate(static_cast<float>(glfwGetTime()), vec3(0, 1, 0)) *
                        rotate(static_cast<float>(glfwGetTime()), vec3(1, 0, 0));
        mat4 viewMat = lookAt(vec3(0, 0, 2), vec3(0, 0, 0), vec3(0, 1, 0));
        mat4 projMat = perspective(radians(60.0f), gWindowWidth / static_cast<float>(gWindowHeight), 0.1f, 10000.0f);

        mat4 mvpMat = projMat * viewMat * modelMat;

        pRenderEncoder->setVertexBytes(&mvpMat, sizeof(glm::mat4), 2);
        pRenderEncoder->setFragmentTexture(texture.Texture.get(), 0);

        MTL::Buffer* vbvs[2]    = {positionBuffer.Buffer.get(), texCoordBuffer.Buffer.get()};
        NS::UInteger offsets[2] = {0, 0};
        NS::Range    vbRange(0, 2);
        pRenderEncoder->setVertexBuffers(vbvs, offsets, vbRange);

        pRenderEncoder->drawIndexedPrimitives(MTL::PrimitiveType::PrimitiveTypeTriangle, 36, MTL::IndexTypeUInt32, indexBuffer.Buffer.get(), 0);

        pRenderEncoder->endEncoding();

        pCommandBuffer->presentDrawable(pDrawable);
        pCommandBuffer->commit();
    }

    return 0;
}

void CreateTexture(MetalRenderer* pRenderer, MetalTexture* pTexture)
{
    auto bitmap = LoadImage8u(GetAssetPath("textures/brushed_metal.png"));
    assert((bitmap.GetSizeInBytes() > 0) && "image load failed");

    CHECK_CALL(CreateTexture(
        pRenderer,
        bitmap.GetWidth(),
        bitmap.GetHeight(),
        MTL::PixelFormatRGBA8Unorm,
        bitmap.GetSizeInBytes(),
        bitmap.GetPixels(),
        pTexture));
}

void CreateGeometryBuffers(
    MetalRenderer* pRenderer,
    MetalBuffer*   pIndexBuffer,
    MetalBuffer*   pPositionBuffer,
    MetalBuffer*   pTexCoordBuffer)
{
    TriMesh::Options options;
    options.enableTexCoords = true;

    TriMesh mesh = TriMesh::Cube(vec3(1), false, options);

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
        SizeInBytes(mesh.GetTexCoords()),
        DataPtr(mesh.GetTexCoords()),
        pTexCoordBuffer));
}
