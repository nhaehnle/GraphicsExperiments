#include "window.h"

#include "vk_renderer.h"
#include "tri_mesh.h"

#include <glm/glm.hpp>
#include <glm/matrix.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/transform.hpp>
using namespace glm;

#define CHECK_CALL(FN)                               \
    {                                                \
        HRESULT hr = FN;                             \
        if (FAILED(hr))                              \
        {                                            \
            std::stringstream ss;                    \
            ss << "\n";                              \
            ss << "*** FUNCTION CALL FAILED *** \n"; \
            ss << "FUNCTION: " << #FN << "\n";       \
            ss << "\n";                              \
            GREX_LOG_ERROR(ss.str().c_str());        \
            assert(false);                           \
        }                                            \
    }

// =============================================================================
// Shader code
// =============================================================================
const char* gClearRayGenSamplesShader = R"(


RWTexture2D<float4>      AccumTarget   : register(u0); // Accumulation texture
RWStructuredBuffer<uint> RayGenSamples : register(u1); // Ray generation samples

[numthreads(8, 8, 1)]
void csmain(uint3 tid : SV_DispatchThreadId)
{
    AccumTarget[tid.xy] = float4(0, 0, 0, 0);

    uint idx = tid.y * 1280 + tid.x;
    RayGenSamples[idx] = 0;    
}
)";

// =============================================================================
// Globals
// =============================================================================
static uint32_t gWindowWidth  = 1280;
static uint32_t gWindowHeight = 720;
static bool     gEnableDebug  = true;

static uint32_t gMaxRayRecursionDepth = 8;

static const char* gRayGenShaderName     = "MyRaygenShader";
static const char* gMissShaderName       = "MyMissShader";
static const char* gClosestHitShaderName = "MyClosestHitShader";

static float gTargetAngle = 0.0f;
static float gAngle       = 0.0f;

static bool     gResetRayGenSamples = true;
static uint32_t gMaxSamples         = 4096;
static uint32_t gCurrentMaxSamples  = 0;

struct Light
{
    vec3  Position;
    vec3  Color;
    float Intensity;
};

struct SceneParameters
{
    mat4  ViewInverseMatrix;
    mat4  ProjectionInverseMatrix;
    mat4  ViewProjectionMatrix;
    vec3  EyePosition;
    uint  MaxSamples;
    uint  NumLights;
    Light Lights[8];
};

struct Geometry
{
    uint32_t     indexCount;
    VulkanBuffer indexBuffer;
    uint32_t     vertexCount;
    VulkanBuffer positionBuffer;
    VulkanBuffer normalBuffer;
};

struct IBLTextures
{
    VulkanImage irrTexture;
    VulkanImage envTexture;
    uint32_t    envNumLevels;
};

struct MaterialParameters
{
    vec3  baseColor;
    float roughness;
    float metallic;
    float specularReflectance;
    float ior;
};

void CreateRayTracePipelineLayout(
    VulkanRenderer*       pRenderer,
    VulkanPipelineLayout* pPipelineLayout);
void CreateRayTracingPipeline(
    VulkanRenderer*             pRenderer,
    VkShaderModule              rayTraceModule,
    const VulkanPipelineLayout& pipelineLayout,
    VkPipeline*                 pPipeline);
void CreateShaderBindingTables(
    VulkanRenderer*                                  pRenderer,
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR& rayTracingProperties,
    VkPipeline                                       pipeline,
    VulkanBuffer*                                    pRayGenSBT,
    VulkanBuffer*                                    pMissSBT,
    VulkanBuffer*                                    pHitGroupSBT);
void CreateGeometries(
    VulkanRenderer* pRenderer,
    Geometry&       outSphereGeometry,
    Geometry&       outBoxGeometry);
void CreateBLASes(
    VulkanRenderer*    pRenderer,
    const Geometry&    sphereGeometry,
    const Geometry&    boxGeometry,
    VulkanAccelStruct* pSphereBLAS,
    VulkanAccelStruct* pBoxBLAS);
void CreateTLAS(
    VulkanRenderer*                  pRenderer,
    const VulkanAccelStruct&         sphereBLAS,
    const VulkanAccelStruct&         boxBLAS,
    VulkanAccelStruct*               pTLAS,
    std::vector<MaterialParameters>& outMaterialParams);
void CreateAccumTexture(VulkanRenderer* pRenderer, VulkanImage* pImage);
void CreateIBLTextures(
    VulkanRenderer* pRenderer,
    VulkanImage*    pBRDFLUT,
    IBLTextures&    outIBLTextures);
void CreateDescriptorBuffer(
    VulkanRenderer*       pRenderer,
    VkDescriptorSetLayout descriptorSetLayout,
    VulkanBuffer*         pBuffer);
void WriteDescriptors(
    VulkanRenderer*          pRenderer,
    VkDescriptorSetLayout    descriptorSetLayout,
    VulkanBuffer*            pDescriptorBuffer,
    const VulkanBuffer&      sceneParamsBuffer,
    const VulkanAccelStruct& accelStruct,
    const VulkanImage&       accumTexture,
    const VulkanBuffer&      rayGenSamplesBuffer,
    const VulkanBuffer&      sceneParams,
    const Geometry&          sphereGeometry,
    const Geometry&          boxGeometry,
    const VulkanBuffer&      materialParamsBuffer,
    const IBLTextures&       iblTextures,
    VkSampler                iblSampler,
    VkImageView*             pAccumImageView,
    VkImageView*             pIBLImageView);

void MouseMove(int x, int y, int buttons)
{
    static int prevX = x;
    static int prevY = y;

    if (buttons & MOUSE_BUTTON_LEFT)
    {
        int dx = x - prevX;
        int dy = y - prevY;

        gTargetAngle += 0.25f * dx;

        gResetRayGenSamples = true;
    }

    prevX = x;
    prevY = y;
}

// =============================================================================
// main()
// =============================================================================
int main(int argc, char** argv)
{
    std::unique_ptr<VulkanRenderer> renderer = std::make_unique<VulkanRenderer>();

    VulkanFeatures features   = {};
    features.EnableRayTracing = true;
    if (!InitVulkan(renderer.get(), gEnableDebug, features))
    {
        return EXIT_FAILURE;
    }

    // *************************************************************************
    // Get ray tracing properties
    // *************************************************************************
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rayTracingProperties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    {
        VkPhysicalDeviceProperties2 properties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext                       = &rayTracingProperties;
        vkGetPhysicalDeviceProperties2(renderer->PhysicalDevice, &properties);

        assert((gMaxRayRecursionDepth <= rayTracingProperties.maxRayRecursionDepth) && "Max ray recursion depth exceeded!");
    }

    // *************************************************************************
    // Get descriptor buffer properties
    // *************************************************************************
    VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptorBufferProperties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT};
    {
        VkPhysicalDeviceProperties2 properties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext                       = &descriptorBufferProperties;
        vkGetPhysicalDeviceProperties2(renderer->PhysicalDevice, &properties);
    }

    // Compile shaders
    // *************************************************************************
    std::vector<uint32_t> rayTraceSpv;
    {
        auto source = LoadString("projects/030_raytracing_path_trace/shaders.hlsl");
        assert((!source.empty()) && "no shader source!");

        std::string errorMsg;
        HRESULT     hr = CompileHLSL(source, "", "lib_6_5", &rayTraceSpv, &errorMsg);
        if (FAILED(hr))
        {
            std::stringstream ss;
            ss << "\n"
               << "Shader compiler error (raytracing): " << errorMsg << "\n";
            GREX_LOG_ERROR(ss.str().c_str());
            assert(false);
            return EXIT_FAILURE;
        }
    }

    std::vector<uint32_t> clearRayGenSpv;
    {
        std::string errorMsg;
        HRESULT     hr = CompileHLSL(gClearRayGenSamplesShader, "csmain", "cs_6_5", &clearRayGenSpv, &errorMsg);
        if (FAILED(hr))
        {
            std::stringstream ss;
            ss << "\n"
               << "Shader compiler error (clear ray gen): " << errorMsg << "\n";
            GREX_LOG_ERROR(ss.str().c_str());
            assert(false);
            return EXIT_FAILURE;
        }
    }

    // *************************************************************************
    // Ray tracing descriptor set and pipeline layout
    //
    // This is used for pipeline creation and setting the descriptor buffer(s).
    //
    // *************************************************************************
    VulkanPipelineLayout rayTracePipelineLayout = {};
    CreateRayTracePipelineLayout(renderer.get(), &rayTracePipelineLayout);

    // *************************************************************************
    // Ray tracing Shader module
    // *************************************************************************
    VkShaderModule rayTraceShaderModule = VK_NULL_HANDLE;
    {
        VkShaderModuleCreateInfo createInfo = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        createInfo.codeSize                 = SizeInBytes(rayTraceSpv);
        createInfo.pCode                    = DataPtr(rayTraceSpv);

        CHECK_CALL(vkCreateShaderModule(renderer->Device, &createInfo, nullptr, &rayTraceShaderModule));
    }

    // *************************************************************************
    // Ray tracing pipeline
    //
    // The pipeline is created with 3 shader groups:
    //    1) Ray gen
    //    2) Miss
    //    3) Hitgroup
    //
    // *************************************************************************
    VkPipeline rayTracePipeline = VK_NULL_HANDLE;
    CreateRayTracingPipeline(
        renderer.get(),
        rayTraceShaderModule,
        rayTracePipelineLayout,
        &rayTracePipeline);

    // *************************************************************************
    // Shader binding tables
    //
    // This assumes that there are 3 shader groups in the pipeline:
    //    1) Ray gen
    //    2) Miss
    //    3) Hitgroup
    //
    // *************************************************************************
    VulkanBuffer rgenSBT = {};
    VulkanBuffer missSBT = {};
    VulkanBuffer hitgSBT = {};
    CreateShaderBindingTables(
        renderer.get(),
        rayTracingProperties,
        rayTracePipeline,
        &rgenSBT,
        &missSBT,
        &hitgSBT);

    // *************************************************************************
    // Clear ray gen pipeline
    // *************************************************************************
    VulkanPipelineLayout clearRayGenPipelineLayout = {};
    VkPipeline           clearRayGenPipeline       = VK_NULL_HANDLE;
    {
        // Descriptor set layout
        {
            std::vector<VkDescriptorSetLayoutBinding> bindings = {
                {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  1, VK_SHADER_STAGE_COMPUTE_BIT},
                {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
            };

            VkDescriptorSetLayoutCreateInfo createInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            createInfo.flags                           = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
            createInfo.bindingCount                    = CountU32(bindings);
            createInfo.pBindings                       = DataPtr(bindings);

            CHECK_CALL(vkCreateDescriptorSetLayout(renderer->Device, &createInfo, nullptr, &clearRayGenPipelineLayout.DescriptorSetLayout));
        }

        // Pipeline layout
        {
            VkPipelineLayoutCreateInfo createInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            createInfo.flags                      = 0;
            createInfo.setLayoutCount             = 1;
            createInfo.pSetLayouts                = &clearRayGenPipelineLayout.DescriptorSetLayout;

            CHECK_CALL(vkCreatePipelineLayout(renderer->Device, &createInfo, nullptr, &clearRayGenPipelineLayout.PipelineLayout));
        }

        // Shader module
        VkShaderModule shaderModule = VK_NULL_HANDLE;
        {
            VkShaderModuleCreateInfo createInfo = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            createInfo.flags                    = 0;
            createInfo.codeSize                 = SizeInBytes(clearRayGenSpv);
            createInfo.pCode                    = DataPtr(clearRayGenSpv);

            CHECK_CALL(vkCreateShaderModule(
                renderer->Device,
                &createInfo,
                nullptr,
                &shaderModule));
        }

        // Pipeline
        {
            VkComputePipelineCreateInfo createInfo = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            createInfo.flags                       = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
            createInfo.stage                       = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            createInfo.stage.flags                 = 0;
            createInfo.stage.stage                 = VK_SHADER_STAGE_COMPUTE_BIT;
            createInfo.stage.module                = shaderModule;
            createInfo.stage.pName                 = "csmain";
            createInfo.layout                      = clearRayGenPipelineLayout.PipelineLayout;

            CHECK_CALL(vkCreateComputePipelines(
                renderer->Device,
                VK_NULL_HANDLE,
                1,
                &createInfo,
                nullptr,
                &clearRayGenPipeline));
        }
    }

    // *************************************************************************
    // Create geometry
    // *************************************************************************
    Geometry sphereGeometry;
    Geometry boxGeometry;
    CreateGeometries(
        renderer.get(),
        sphereGeometry,
        boxGeometry);

    // *************************************************************************
    // Bottom level acceleration structure
    // *************************************************************************
    VulkanAccelStruct sphereBLAS;
    VulkanAccelStruct boxBLAS;
    CreateBLASes(
        renderer.get(),
        sphereGeometry,
        boxGeometry,
        &sphereBLAS,
        &boxBLAS);

    // *************************************************************************
    // Top level acceleration structure
    // *************************************************************************
    VulkanAccelStruct               TLAS;
    std::vector<MaterialParameters> materialParams;
    CreateTLAS(
        renderer.get(),
        sphereBLAS,
        boxBLAS,
        &TLAS,
        materialParams);

    // *************************************************************************
    // Output and accumulation texture
    // *************************************************************************
    VulkanImage accumTexture = {};
    CreateAccumTexture(renderer.get(), &accumTexture);

    // *************************************************************************
    // Material params buffer
    // *************************************************************************
    VulkanBuffer materialParamsBuffer = {};
    CreateBuffer(
        renderer.get(),
        SizeInBytes(materialParams),
        DataPtr(materialParams),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY,
        0,
        &materialParamsBuffer);

    // *************************************************************************
    // Scene params constant buffer
    // *************************************************************************
    VulkanBuffer sceneParamsBuffer = {};
    CHECK_CALL(CreateBuffer(
        renderer.get(),
        Align<size_t>(sizeof(SceneParameters), 256),
        nullptr,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        0,
        &sceneParamsBuffer));

    // *************************************************************************
    // Ray gen samples buffer
    // *************************************************************************
    VulkanBuffer rayGenSamplesBuffer = {};
    CHECK_CALL(CreateBuffer(
        renderer.get(),
        (gWindowWidth * gWindowHeight * sizeof(uint32_t)),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY,
        0,
        &rayGenSamplesBuffer));

    // *************************************************************************
    // IBL txtures
    // *************************************************************************
    VulkanImage brdfLUT;
    IBLTextures iblTextures = {};
    CreateIBLTextures(
        renderer.get(),
        &brdfLUT,
        iblTextures);

    // *************************************************************************
    // IBL Sampler
    // *************************************************************************
    VkSamplerCreateInfo createInfo     = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    createInfo.flags                   = 0;
    createInfo.magFilter               = VK_FILTER_LINEAR;
    createInfo.minFilter               = VK_FILTER_LINEAR;
    createInfo.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    createInfo.addressModeU            = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    createInfo.addressModeV            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    createInfo.addressModeW            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    createInfo.mipLodBias              = 0;
    createInfo.anisotropyEnable        = VK_FALSE;
    createInfo.maxAnisotropy           = 0;
    createInfo.compareEnable           = VK_TRUE;
    createInfo.compareOp               = VK_COMPARE_OP_LESS_OR_EQUAL;
    createInfo.minLod                  = 0;
    createInfo.maxLod                  = FLT_MAX;
    createInfo.borderColor             = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    createInfo.unnormalizedCoordinates = VK_FALSE;

    VkSampler iblSampler = VK_NULL_HANDLE;
    CHECK_CALL(vkCreateSampler(
        renderer->Device,
        &createInfo,
        nullptr,
        &iblSampler));

    // *************************************************************************
    // Descriptor buffers
    // *************************************************************************
    VulkanBuffer rayTraceDescriptorBuffer = {};
    CreateDescriptorBuffer(renderer.get(), rayTracePipelineLayout.DescriptorSetLayout, &rayTraceDescriptorBuffer);

    // Write descriptors
    VkImageView accumImageView = VK_NULL_HANDLE;
    VkImageView iblImageView   = VK_NULL_HANDLE;
    WriteDescriptors(
        renderer.get(),
        rayTracePipelineLayout.DescriptorSetLayout,
        &rayTraceDescriptorBuffer,
        sceneParamsBuffer,
        TLAS,
        accumTexture,
        rayGenSamplesBuffer,
        sceneParamsBuffer,
        sphereGeometry,
        boxGeometry,
        materialParamsBuffer,
        iblTextures,
        iblSampler,
        &accumImageView,
        &iblImageView);

    // Clear ray gen descriptor buffer
    VulkanBuffer clearRayGenDescriptorBuffer = {};
    CreateDescriptorBuffer(renderer.get(), clearRayGenPipelineLayout.DescriptorSetLayout, &clearRayGenDescriptorBuffer);

    // Write descriptors
    {
        char* pDescriptorBuffeStartAddress = nullptr;
        CHECK_CALL(vmaMapMemory(
            renderer->Allocator,
            clearRayGenDescriptorBuffer.Allocation,
            reinterpret_cast<void**>(&pDescriptorBuffeStartAddress)));

        WriteDescriptor(
            renderer.get(),
            pDescriptorBuffeStartAddress,
            clearRayGenPipelineLayout.DescriptorSetLayout,
            0, // binding
            0, // arrayElement
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            accumImageView,
            VK_IMAGE_LAYOUT_GENERAL);

        WriteDescriptor(
            renderer.get(),
            pDescriptorBuffeStartAddress,
            clearRayGenPipelineLayout.DescriptorSetLayout,
            1, // binding
            0, // arrayElement
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            &rayGenSamplesBuffer);

        vmaUnmapMemory(renderer->Allocator, clearRayGenDescriptorBuffer.Allocation);
    }

    // *************************************************************************
    // Window
    // *************************************************************************
    auto window = Window::Create(gWindowWidth, gWindowHeight, "030_raytracing_path_trace_vulkan");
    if (!window)
    {
        assert(false && "Window::Create failed");
        return EXIT_FAILURE;
    }
    window->AddMouseMoveCallbacks(MouseMove);

    // *************************************************************************
    // Swapchain
    // *************************************************************************
    if (!InitSwapchain(renderer.get(), window->GetHWND(), window->GetWidth(), window->GetHeight(), 3))
    {
        assert(false && "InitSwapchain failed");
        return EXIT_FAILURE;
    }

    // *************************************************************************
    // Swapchain image views
    // *************************************************************************
    std::vector<VkImage>     swapchainImages;
    std::vector<VkImageView> swapchainImageViews;
    {
        CHECK_CALL(GetSwapchainImages(renderer.get(), swapchainImages));

        for (auto& image : swapchainImages)
        {
            VkImageViewCreateInfo createInfo           = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            createInfo.image                           = image;
            createInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
            createInfo.format                          = GREX_DEFAULT_RTV_FORMAT;
            createInfo.components                      = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
            createInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            createInfo.subresourceRange.baseMipLevel   = 0;
            createInfo.subresourceRange.levelCount     = 1;
            createInfo.subresourceRange.baseArrayLayer = 0;
            createInfo.subresourceRange.layerCount     = 1;

            VkImageView imageView = VK_NULL_HANDLE;
            CHECK_CALL(vkCreateImageView(renderer->Device, &createInfo, nullptr, &imageView));

            swapchainImageViews.push_back(imageView);
        }
    }

    // *************************************************************************
    // Render pass to draw ImGui
    // *************************************************************************
    std::vector<VulkanAttachmentInfo> colorAttachmentInfos = {
        {VK_FORMAT_B8G8R8A8_UNORM, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, renderer->SwapchainImageUsage}
    };

    VulkanRenderPass renderPass = {};
    CHECK_CALL(CreateRenderPass(renderer.get(), colorAttachmentInfos, {}, gWindowWidth, gWindowHeight, &renderPass));

    // *************************************************************************
    // Imgui
    // *************************************************************************
    if (!window->InitImGuiForVulkan(renderer.get(), renderPass.RenderPass))
    {
        assert(false && "Window::InitImGuiForD3D12 failed");
        return EXIT_FAILURE;
    }

    // *************************************************************************
    // Command buffer
    // *************************************************************************
    CommandObjects cmdBuf = {};
    CHECK_CALL(CreateCommandBuffer(renderer.get(), 0, &cmdBuf));

    // *************************************************************************
    // Persistent map scene parameters
    // *************************************************************************
    SceneParameters* pSceneParams = nullptr;
    CHECK_CALL(vmaMapMemory(renderer->Allocator, sceneParamsBuffer.Allocation, reinterpret_cast<void**>(&pSceneParams)));

    // *************************************************************************
    // Persistent map ray trace descriptor buffer
    // *************************************************************************
    char* pRayTraceDescriptorBuffeStartAddress = nullptr;
    CHECK_CALL(vmaMapMemory(
        renderer->Allocator,
        rayTraceDescriptorBuffer.Allocation,
        reinterpret_cast<void**>(&pRayTraceDescriptorBuffeStartAddress)));

    // *************************************************************************
    // Misc vars
    // *************************************************************************
    uint32_t sampleCount     = 0;
    float    rayGenStartTime = 0;

    // *************************************************************************
    // Main loop
    // *************************************************************************
    while (window->PollEvents())
    {
        window->ImGuiNewFrameVulkan();

        if (ImGui::Begin("Scene"))
        {
            ImGui::SliderInt("Max Samples Per Pixel", reinterpret_cast<int*>(&gMaxSamples), 1, 16384);

            ImGui::Separator();

            float progress = sampleCount / static_cast<float>(gMaxSamples);
            char  buf[256] = {};
            sprintf(buf, "%d/%d Samples", sampleCount, gMaxSamples);
            ImGui::ProgressBar(progress, ImVec2(-1, 0), buf);

            ImGui::Separator();

            static float elapsedTime = 0;
            if (sampleCount < gMaxSamples)
            {
                float currentTime = static_cast<float>(glfwGetTime());
                elapsedTime       = currentTime - rayGenStartTime;
            }

            ImGui::Text("Render time: %0.3f seconds", elapsedTime);
        }
        ImGui::End();

        // ---------------------------------------------------------------------

        if (gCurrentMaxSamples != gMaxSamples)
        {
            gCurrentMaxSamples  = gMaxSamples;
            gResetRayGenSamples = true;
        }

        // Smooth out the rotation on Y
        gAngle += (gTargetAngle - gAngle) * 0.25f;
        // Keep resetting until the angle is somewhat stable
        if (fabs(gTargetAngle - gAngle) > 0.1f)
        {
            gResetRayGenSamples = true;
        }

        // Camera matrices
        mat4 transformEyeMat     = glm::rotate(glm::radians(-gAngle), vec3(0, 1, 0));
        vec3 startingEyePosition = vec3(0, 4.0f, 8.5f);
        vec3 eyePosition         = transformEyeMat * vec4(startingEyePosition, 1);
        mat4 viewMat             = glm::lookAt(eyePosition, vec3(0, 3, 0), vec3(0, 1, 0));
        mat4 projMat             = glm::perspective(glm::radians(60.0f), gWindowWidth / static_cast<float>(gWindowHeight), 0.1f, 10000.0f);

        // Set constant buffer values
        pSceneParams->ViewInverseMatrix       = glm::inverse(viewMat);
        pSceneParams->ProjectionInverseMatrix = glm::inverse(projMat);
        pSceneParams->EyePosition             = eyePosition;
        pSceneParams->MaxSamples              = gCurrentMaxSamples;

        // ---------------------------------------------------------------------
        // Acquire swapchain image index
        // ---------------------------------------------------------------------
        uint32_t swapchainImageIndex = 0;
        if (AcquireNextImage(renderer.get(), &swapchainImageIndex))
        {
            assert(false && "AcquireNextImage failed");
            break;
        }

        // Update output texture (u1)
        //
        // Most Vulkan implementations support STORAGE_IMAGE so we can
        // write directly to the image and skip a copy.
        //
        WriteDescriptor(
            renderer.get(),
            pRayTraceDescriptorBuffeStartAddress,
            rayTracePipelineLayout.DescriptorSetLayout,
            1, // binding
            0, // arrayElement
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            swapchainImageViews[swapchainImageIndex],
            VK_IMAGE_LAYOUT_GENERAL);

        // ---------------------------------------------------------------------
        // Build command buffer to trace rays
        // ---------------------------------------------------------------------
        VkCommandBufferBeginInfo vkbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkbi.flags                    = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
        CHECK_CALL(vkBeginCommandBuffer(cmdBuf.CommandBuffer, &vkbi));

        // Reset ray gen samples
        if (gResetRayGenSamples)
        {
            sampleCount     = 0;
            rayGenStartTime = static_cast<float>(glfwGetTime());

            vkCmdBindPipeline(cmdBuf.CommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, clearRayGenPipeline);

            VkDescriptorBufferBindingInfoEXT descriptorBufferBindingInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT};
            descriptorBufferBindingInfo.pNext                            = nullptr;
            descriptorBufferBindingInfo.address                          = GetDeviceAddress(renderer.get(), &clearRayGenDescriptorBuffer);
            descriptorBufferBindingInfo.usage                            = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT;
            fn_vkCmdBindDescriptorBuffersEXT(cmdBuf.CommandBuffer, 1, &descriptorBufferBindingInfo);

            uint32_t     bufferIndices           = 0;
            VkDeviceSize descriptorBufferOffsets = 0;
            fn_vkCmdSetDescriptorBufferOffsetsEXT(
                cmdBuf.CommandBuffer,
                VK_PIPELINE_BIND_POINT_COMPUTE,
                clearRayGenPipelineLayout.PipelineLayout,
                0, // firstSet
                1, // setCount
                &bufferIndices,
                &descriptorBufferOffsets);

            vkCmdDispatch(cmdBuf.CommandBuffer, gWindowWidth / 8, gWindowHeight / 8, 1);

            gResetRayGenSamples = false;
        }

        // Trace rays
        {
            CmdTransitionImageLayout(
                cmdBuf.CommandBuffer,
                swapchainImages[swapchainImageIndex],
                GREX_ALL_SUBRESOURCES,
                VK_IMAGE_ASPECT_COLOR_BIT,
                RESOURCE_STATE_PRESENT,
                RESOURCE_STATE_COMPUTE_UNORDERED_ACCESS);

            vkCmdBindPipeline(cmdBuf.CommandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rayTracePipeline);

            VkDescriptorBufferBindingInfoEXT descriptorBufferBindingInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT};
            descriptorBufferBindingInfo.pNext                            = nullptr;
            descriptorBufferBindingInfo.address                          = GetDeviceAddress(renderer.get(), &rayTraceDescriptorBuffer);
            descriptorBufferBindingInfo.usage                            = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT;

            fn_vkCmdBindDescriptorBuffersEXT(cmdBuf.CommandBuffer, 1, &descriptorBufferBindingInfo);

            uint32_t     bufferIndices           = 0;
            VkDeviceSize descriptorBufferOffsets = 0;
            fn_vkCmdSetDescriptorBufferOffsetsEXT(
                cmdBuf.CommandBuffer,
                VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                rayTracePipelineLayout.PipelineLayout,
                0, // firstSet
                1, // setCount
                &bufferIndices,
                &descriptorBufferOffsets);

            const uint32_t alignedHandleSize = Align(
                rayTracingProperties.shaderGroupHandleSize,
                rayTracingProperties.shaderGroupHandleAlignment);

            VkStridedDeviceAddressRegionKHR rgenShaderSBTEntry = {};
            rgenShaderSBTEntry.deviceAddress                   = GetDeviceAddress(renderer.get(), &rgenSBT);
            rgenShaderSBTEntry.stride                          = alignedHandleSize;
            rgenShaderSBTEntry.size                            = alignedHandleSize;

            VkStridedDeviceAddressRegionKHR missShaderSBTEntry = {};
            missShaderSBTEntry.deviceAddress                   = GetDeviceAddress(renderer.get(), &missSBT);
            missShaderSBTEntry.stride                          = alignedHandleSize;
            missShaderSBTEntry.size                            = alignedHandleSize;

            VkStridedDeviceAddressRegionKHR chitShaderSBTEntry = {};
            chitShaderSBTEntry.deviceAddress                   = GetDeviceAddress(renderer.get(), &hitgSBT);
            chitShaderSBTEntry.stride                          = alignedHandleSize;
            chitShaderSBTEntry.size                            = alignedHandleSize;

            VkStridedDeviceAddressRegionKHR callableShaderSbtEntry = {};

            fn_vkCmdTraceRaysKHR(
                cmdBuf.CommandBuffer,
                &rgenShaderSBTEntry,
                &missShaderSBTEntry,
                &chitShaderSBTEntry,
                &callableShaderSbtEntry,
                gWindowWidth,
                gWindowHeight,
                1);

            CHECK_CALL(vkEndCommandBuffer(cmdBuf.CommandBuffer));

            // Execute command buffer
            CHECK_CALL(ExecuteCommandBuffer(renderer.get(), &cmdBuf));

            // Wait for the GPU to finish the work
            if (!WaitForGpu(renderer.get()))
            {
                assert(false && "WaitForGpu failed");
            }
        }

        // Reset command buffer to render ImGui
        CHECK_CALL(vkBeginCommandBuffer(cmdBuf.CommandBuffer, &vkbi));

        // ImGui
        {
            CmdTransitionImageLayout(
                cmdBuf.CommandBuffer,
                swapchainImages[swapchainImageIndex],
                GREX_ALL_SUBRESOURCES,
                VK_IMAGE_ASPECT_COLOR_BIT,
                RESOURCE_STATE_COMPUTE_UNORDERED_ACCESS,
                RESOURCE_STATE_RENDER_TARGET);

            VkRenderPassAttachmentBeginInfo attachmentBeginInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO};
            attachmentBeginInfo.pNext                           = 0;
            attachmentBeginInfo.attachmentCount                 = 1;
            attachmentBeginInfo.pAttachments                    = &swapchainImageViews[swapchainImageIndex];

            VkRect2D renderArea = {
                {0,            0            },
                {gWindowWidth, gWindowHeight}
            };

            VkRenderPassBeginInfo beginInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            beginInfo.pNext                 = &attachmentBeginInfo;
            beginInfo.renderPass            = renderPass.RenderPass;
            beginInfo.framebuffer           = renderPass.Framebuffer;
            beginInfo.renderArea            = renderArea;

            vkCmdBeginRenderPass(cmdBuf.CommandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

            VkViewport viewport = {0, 0, static_cast<float>(gWindowWidth), static_cast<float>(gWindowHeight), 0, 1};
            vkCmdSetViewport(cmdBuf.CommandBuffer, 0, 1, &viewport);

            vkCmdSetScissor(cmdBuf.CommandBuffer, 0, 1, &renderArea);

            // Draw ImGui
            window->ImGuiRenderDrawData(renderer.get(), cmdBuf.CommandBuffer);

            vkCmdEndRenderPass(cmdBuf.CommandBuffer);

            CmdTransitionImageLayout(
                cmdBuf.CommandBuffer,
                swapchainImages[swapchainImageIndex],
                GREX_ALL_SUBRESOURCES,
                VK_IMAGE_ASPECT_COLOR_BIT,
                RESOURCE_STATE_RENDER_TARGET,
                RESOURCE_STATE_PRESENT);

            CHECK_CALL(vkEndCommandBuffer(cmdBuf.CommandBuffer));

            // Execute command buffer
            CHECK_CALL(ExecuteCommandBuffer(renderer.get(), &cmdBuf));

            // Wait for the GPU to finish the work
            if (!WaitForGpu(renderer.get()))
            {
                assert(false && "WaitForGpu failed");
            }
        }

        // Update sample count
        if (sampleCount < gMaxSamples)
        {
            ++sampleCount;
        }

        if (!SwapchainPresent(renderer.get(), swapchainImageIndex))
        {
            assert(false && "SwapchainPresent failed");
            break;
        }
    }

    return 0;
}

void CreateRayTracePipelineLayout(
    VulkanRenderer*       pRenderer,
    VulkanPipelineLayout* pPipelineLayout)
{
    // Descriptor set layout
    {
        std::vector<VkDescriptorSetLayoutBinding> bindings = {};
        // Acceleration structure (t0)
        {
            VkDescriptorSetLayoutBinding binding = {};
            binding.binding                      = 0;
            binding.descriptorType               = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            binding.descriptorCount              = 1;
            binding.stageFlags                   = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
            bindings.push_back(binding);
        }
        // Output texture (u1)
        // Accumulation texture (u2)
        // Ray generaiton sampling (u3)
        {
            VkDescriptorSetLayoutBinding binding = {};
            binding.binding                      = 1;
            binding.descriptorType               = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            binding.descriptorCount              = 1;
            binding.stageFlags                   = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
            bindings.push_back(binding);

            binding                 = {};
            binding.binding         = 2;
            binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            binding.descriptorCount = 1;
            binding.stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
            bindings.push_back(binding);

            binding                 = {};
            binding.binding         = 3;
            binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            binding.descriptorCount = 1;
            binding.stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
            bindings.push_back(binding);
        }
        // Scene params (b5)
        {
            VkDescriptorSetLayoutBinding binding = {};
            binding.binding                      = 5;
            binding.descriptorType               = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            binding.descriptorCount              = 1;
            binding.stageFlags                   = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;
            bindings.push_back(binding);
        }
        //  Index buffers (t20)
        //  Position buffers (t25)
        //  Normal buffers (t30)
        {
            VkDescriptorSetLayoutBinding binding = {};
            binding.binding                      = 20;
            binding.descriptorType               = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            binding.descriptorCount              = 5;
            binding.stageFlags                   = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
            bindings.push_back(binding);

            binding                 = {};
            binding.binding         = 25;
            binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            binding.descriptorCount = 5;
            binding.stageFlags      = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
            bindings.push_back(binding);

            binding                 = {};
            binding.binding         = 30;
            binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            binding.descriptorCount = 5;
            binding.stageFlags      = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
            bindings.push_back(binding);
        }

        // Environment map (t100)
        {
            VkDescriptorSetLayoutBinding binding = {};
            binding.binding                      = 100;
            binding.descriptorType               = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            binding.descriptorCount              = 1;
            binding.stageFlags                   = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;
            bindings.push_back(binding);
        }

        // Material params (t9)
        {
            VkDescriptorSetLayoutBinding binding = {};
            binding.binding                      = 9;
            binding.descriptorType               = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            binding.descriptorCount              = 1;
            binding.stageFlags                   = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
            bindings.push_back(binding);
        }

        // IBLMapSampler (s10)
        {
            VkDescriptorSetLayoutBinding binding = {};
            binding.binding                      = 10;
            binding.descriptorType               = VK_DESCRIPTOR_TYPE_SAMPLER;
            binding.descriptorCount              = 1;
            binding.stageFlags                   = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;
            binding.pImmutableSamplers           = nullptr;
            bindings.push_back(binding);
        }

        VkDescriptorSetLayoutCreateInfo createInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        createInfo.flags                           = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
        createInfo.bindingCount                    = CountU32(bindings);
        createInfo.pBindings                       = DataPtr(bindings);

        CHECK_CALL(vkCreateDescriptorSetLayout(
            pRenderer->Device,
            &createInfo,
            nullptr,
            &pPipelineLayout->DescriptorSetLayout));
    }

    // Pipeline layout
    {
        VkPipelineLayoutCreateInfo createInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        createInfo.setLayoutCount             = 1;
        createInfo.pSetLayouts                = &pPipelineLayout->DescriptorSetLayout;

        CHECK_CALL(vkCreatePipelineLayout(
            pRenderer->Device,
            &createInfo,
            nullptr,
            &pPipelineLayout->PipelineLayout));
    }
}

void CreateRayTracingPipeline(
    VulkanRenderer*             pRenderer,
    VkShaderModule              rayTraceModule,
    const VulkanPipelineLayout& pipelineLayout,
    VkPipeline*                 pPipeline)
{
    // Shader stages
    std::vector<VkPipelineShaderStageCreateInfo> shaderStages = {};
    // Ray gen
    {
        VkPipelineShaderStageCreateInfo createInfo = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        createInfo.stage                           = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
        createInfo.module                          = rayTraceModule;
        createInfo.pName                           = gRayGenShaderName;

        shaderStages.push_back(createInfo);
    }
    // Miss
    {
        VkPipelineShaderStageCreateInfo createInfo = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        createInfo.stage                           = VK_SHADER_STAGE_MISS_BIT_KHR;
        createInfo.module                          = rayTraceModule;
        createInfo.pName                           = gMissShaderName;

        shaderStages.push_back(createInfo);
    }
    // Closest hit
    {
        VkPipelineShaderStageCreateInfo createInfo = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        createInfo.stage                           = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
        createInfo.module                          = rayTraceModule;
        createInfo.pName                           = gClosestHitShaderName;

        shaderStages.push_back(createInfo);
    }

    // Shader groups
    std::vector<VkRayTracingShaderGroupCreateInfoKHR> shaderGroups = {};
    // Ray gen
    {
        VkRayTracingShaderGroupCreateInfoKHR createInfo = {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
        createInfo.type                                 = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        createInfo.generalShader                        = 0; // shaderStages[0]
        createInfo.closestHitShader                     = VK_SHADER_UNUSED_KHR;
        createInfo.anyHitShader                         = VK_SHADER_UNUSED_KHR;
        createInfo.intersectionShader                   = VK_SHADER_UNUSED_KHR;

        shaderGroups.push_back(createInfo);
    }
    // Miss
    {
        VkRayTracingShaderGroupCreateInfoKHR createInfo = {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
        createInfo.type                                 = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        createInfo.generalShader                        = 1; // shaderStages[1]
        createInfo.closestHitShader                     = VK_SHADER_UNUSED_KHR;
        createInfo.anyHitShader                         = VK_SHADER_UNUSED_KHR;
        createInfo.intersectionShader                   = VK_SHADER_UNUSED_KHR;

        shaderGroups.push_back(createInfo);
    }
    // Closest hit
    {
        VkRayTracingShaderGroupCreateInfoKHR createInfo = {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
        createInfo.type                                 = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        createInfo.generalShader                        = VK_SHADER_UNUSED_KHR;
        createInfo.closestHitShader                     = 2; // shaderStages[2]
        createInfo.anyHitShader                         = VK_SHADER_UNUSED_KHR;
        createInfo.intersectionShader                   = VK_SHADER_UNUSED_KHR;

        shaderGroups.push_back(createInfo);
    }

    VkRayTracingPipelineInterfaceCreateInfoKHR pipelineInterfaceCreateInfo = {VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_INTERFACE_CREATE_INFO_KHR};
    //
    pipelineInterfaceCreateInfo.maxPipelineRayPayloadSize      = 4 * sizeof(float) + 3 * sizeof(uint32_t); // color, ray depth, sample count, ior;
    pipelineInterfaceCreateInfo.maxPipelineRayHitAttributeSize = 2 * sizeof(float);                        // barycentrics;

    VkRayTracingPipelineCreateInfoKHR createInfo = {VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    createInfo.flags                             = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    createInfo.stageCount                        = CountU32(shaderStages);
    createInfo.pStages                           = DataPtr(shaderStages);
    createInfo.groupCount                        = CountU32(shaderGroups);
    createInfo.pGroups                           = DataPtr(shaderGroups);
    createInfo.maxPipelineRayRecursionDepth      = gMaxRayRecursionDepth;
    createInfo.pLibraryInterface                 = &pipelineInterfaceCreateInfo;
    createInfo.layout                            = pipelineLayout.PipelineLayout;
    createInfo.basePipelineHandle                = VK_NULL_HANDLE;
    createInfo.basePipelineIndex                 = -1;

    CHECK_CALL(fn_vkCreateRayTracingPipelinesKHR(
        pRenderer->Device, // device
        VK_NULL_HANDLE,    // deferredOperation
        VK_NULL_HANDLE,    // pipelineCache
        1,                 // createInfoCount
        &createInfo,       // pCreateInfos
        nullptr,           // pAllocator
        pPipeline));       // pPipelines
}

void CreateShaderBindingTables(
    VulkanRenderer*                                  pRenderer,
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR& rayTracingProperties,
    VkPipeline                                       pipeline,
    VulkanBuffer*                                    pRayGenSBT,
    VulkanBuffer*                                    pMissSBT,
    VulkanBuffer*                                    pHitGroupSBT)
{
    // Hardcoded group count
    const uint32_t groupCount = 3;

    // Handle sizes
    uint32_t groupHandleSize        = rayTracingProperties.shaderGroupHandleSize;
    uint32_t groupHandleAlignment   = rayTracingProperties.shaderGroupHandleAlignment;
    uint32_t alignedGroupHandleSize = Align(groupHandleSize, groupHandleAlignment);
    uint32_t totalGroupDataSize     = groupCount * groupHandleSize;

    //
    // This is what the shader group handles look like
    // in handlesData based on the pipeline. The offsets
    // are in bytes - assuming handleSize is 32 bytes.
    //
    //  +--------------+
    //  |  RGEN        | offset = 0
    //  +--------------+
    //  |  MISS        | offset = 32
    //  +--------------+
    //  |  HITG        | offset = 64
    //  +--------------+
    //
    std::vector<char> groupHandlesData(totalGroupDataSize);
    CHECK_CALL(fn_vkGetRayTracingShaderGroupHandlesKHR(
        pRenderer->Device,         // device
        pipeline,                  // pipeline
        0,                         // firstGroup
        groupCount,                // groupCount
        totalGroupDataSize,        // dataSize
        groupHandlesData.data())); // pData)

    // Usage flags for SBT buffer
    VkBufferUsageFlags usageFlags = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR;

    char* pShaderGroupHandleRGEN = groupHandlesData.data();
    char* pShaderGroupHandleMISS = groupHandlesData.data() + groupHandleSize;
    char* pShaderGroupHandleHITG = groupHandlesData.data() + 2 * groupHandleSize;

    //
    // Create buffers for each shader group's SBT and copy the
    // the shader group handles into each buffer.
    //
    // The size of the SBT buffers must be aligned to
    // VkPhysicalDeviceRayTracingPipelinePropertiesKHR::shaderGroupBaseAlignment.
    //
    const uint32_t shaderGroupBaseAlignment = rayTracingProperties.shaderGroupBaseAlignment;
    // Ray gen
    {
        CHECK_CALL(CreateBuffer(
            pRenderer,                // pRenderer
            groupHandleSize,          // srcSize
            pShaderGroupHandleRGEN,   // pSrcData
            usageFlags,               // usageFlags
            shaderGroupBaseAlignment, // minAlignment
            pRayGenSBT));             // pBuffer
    }
    // Miss
    {
        CHECK_CALL(CreateBuffer(
            pRenderer,                // pRenderer
            groupHandleSize,          // srcSize
            pShaderGroupHandleMISS,   // pSrcData
            usageFlags,               // usageFlags
            shaderGroupBaseAlignment, // minAlignment
            pMissSBT));               // pBuffer
    }
    // HITG: closest hit
    {
        CHECK_CALL(CreateBuffer(
            pRenderer,                // pRenderer
            groupHandleSize,          // srcSize
            pShaderGroupHandleHITG,   // pSrcData
            usageFlags,               // usageFlags
            shaderGroupBaseAlignment, // minAlignment
            pHitGroupSBT));           // pBuffer
    }
}

void CreateGeometries(
    VulkanRenderer* pRenderer,
    Geometry&       outSphereGeometry,
    Geometry&       outBoxGeometry)
{
    VkBufferUsageFlags usageFlags =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    // Sphere
    {
        TriMesh mesh = TriMesh::Sphere(1.0f, 256, 256, {.enableNormals = true});

        Geometry& geo = outSphereGeometry;

        CHECK_CALL(CreateBuffer(
            pRenderer,
            SizeInBytes(mesh.GetTriangles()),
            DataPtr(mesh.GetTriangles()),
            usageFlags,
            0,
            &geo.indexBuffer));

        CHECK_CALL(CreateBuffer(
            pRenderer,
            SizeInBytes(mesh.GetPositions()),
            DataPtr(mesh.GetPositions()),
            usageFlags,
            0,
            &geo.positionBuffer));

        CHECK_CALL(CreateBuffer(
            pRenderer,
            SizeInBytes(mesh.GetNormals()),
            DataPtr(mesh.GetNormals()),
            usageFlags,
            0,
            &geo.normalBuffer));

        geo.indexCount  = 3 * mesh.GetNumTriangles();
        geo.vertexCount = mesh.GetNumVertices();
    }

    // Box
    {
        TriMesh   mesh = TriMesh::Cube(glm::vec3(15, 1, 4.5f), false, {.enableNormals = true});
        Geometry& geo  = outBoxGeometry;

        CHECK_CALL(CreateBuffer(
            pRenderer,
            SizeInBytes(mesh.GetTriangles()),
            DataPtr(mesh.GetTriangles()),
            usageFlags,
            0,
            &geo.indexBuffer));

        CHECK_CALL(CreateBuffer(
            pRenderer,
            SizeInBytes(mesh.GetPositions()),
            DataPtr(mesh.GetPositions()),
            usageFlags,
            0,
            &geo.positionBuffer));

        CHECK_CALL(CreateBuffer(
            pRenderer,
            SizeInBytes(mesh.GetNormals()),
            DataPtr(mesh.GetNormals()),
            usageFlags,
            0,
            &geo.normalBuffer));

        geo.indexCount  = 3 * mesh.GetNumTriangles();
        geo.vertexCount = mesh.GetNumVertices();
    }
}

void CreateBLASes(
    VulkanRenderer*    pRenderer,
    const Geometry&    sphereGeometry,
    const Geometry&    boxGeometry,
    VulkanAccelStruct* pSphereBLAS,
    VulkanAccelStruct* pBoxBLAS)
{
    std::vector<const Geometry*>    geometries = {&sphereGeometry, &boxGeometry};
    std::vector<VulkanAccelStruct*> BLASes     = {pSphereBLAS, pBoxBLAS};

    uint32_t n = static_cast<uint32_t>(geometries.size());
    for (uint32_t i = 0; i < n; ++i)
    {
        auto pGeometry = geometries[i];
        auto pBLAS     = BLASes[i];

        VkAccelerationStructureGeometryKHR geometry = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        //
        geometry.flags                                       = VK_GEOMETRY_OPAQUE_BIT_KHR;
        geometry.geometryType                                = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.geometry.triangles.sType                    = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        geometry.geometry.triangles.vertexFormat             = VK_FORMAT_R32G32B32_SFLOAT;
        geometry.geometry.triangles.vertexData.deviceAddress = GetDeviceAddress(pRenderer, &pGeometry->positionBuffer);
        geometry.geometry.triangles.vertexStride             = 12;
        geometry.geometry.triangles.maxVertex                = pGeometry->vertexCount;
        geometry.geometry.triangles.indexType                = VK_INDEX_TYPE_UINT32;
        geometry.geometry.triangles.indexData.deviceAddress  = GetDeviceAddress(pRenderer, &pGeometry->indexBuffer);

        VkAccelerationStructureBuildGeometryInfoKHR buildGeometryInfo = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        //
        buildGeometryInfo.type          = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        buildGeometryInfo.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        buildGeometryInfo.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        buildGeometryInfo.geometryCount = 1;
        buildGeometryInfo.pGeometries   = &geometry;

        VkAccelerationStructureBuildSizesInfoKHR buildSizesInfo = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        const uint32_t                           numTriangles   = pGeometry->indexCount / 3;
        fn_vkGetAccelerationStructureBuildSizesKHR(
            pRenderer->Device,
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
            &buildGeometryInfo,
            &numTriangles,
            &buildSizesInfo);

        // Scratch buffer
        VulkanBuffer scratchBuffer = {};
        {
            // Get acceleration structure properties
            VkPhysicalDeviceAccelerationStructurePropertiesKHR accelStructProperties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
            VkPhysicalDeviceProperties2                        properties            = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            properties.pNext                                                         = &accelStructProperties;
            vkGetPhysicalDeviceProperties2(pRenderer->PhysicalDevice, &properties);

            VkBufferUsageFlags usageFlags = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

            CHECK_CALL(CreateBuffer(
                pRenderer,                                                            // pRenderer
                buildSizesInfo.buildScratchSize,                                      // srcSize
                usageFlags,                                                           // usageFlags
                VMA_MEMORY_USAGE_GPU_ONLY,                                            // memoryUsage
                accelStructProperties.minAccelerationStructureScratchOffsetAlignment, // minAlignment
                &scratchBuffer));                                                     // pBuffer
        }

        // Create acceleration structure buffer
        {
            VkBufferUsageFlags usageFlags = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;

            CHECK_CALL(CreateBuffer(
                pRenderer,
                buildSizesInfo.accelerationStructureSize,
                usageFlags,
                VMA_MEMORY_USAGE_GPU_ONLY,
                0,
                &pBLAS->Buffer));
        }

        // Create accleration structure object
        {
            VkAccelerationStructureCreateInfoKHR createInfo = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
            createInfo.buffer                               = pBLAS->Buffer.Buffer;
            createInfo.offset                               = 0;
            createInfo.size                                 = buildSizesInfo.accelerationStructureSize;
            createInfo.type                                 = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;

            CHECK_CALL(fn_vkCreateAccelerationStructureKHR(pRenderer->Device, &createInfo, nullptr, &pBLAS->AccelStruct));
        }

        // Build acceleration structure
        //
        {
            // Build geometry info
            VkAccelerationStructureBuildGeometryInfoKHR buildGeometryInfo = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            //
            buildGeometryInfo.type                      = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            buildGeometryInfo.flags                     = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            buildGeometryInfo.mode                      = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            buildGeometryInfo.dstAccelerationStructure  = pBLAS->AccelStruct;
            buildGeometryInfo.geometryCount             = 1;
            buildGeometryInfo.pGeometries               = &geometry;
            buildGeometryInfo.scratchData.deviceAddress = GetDeviceAddress(pRenderer, &scratchBuffer);

            // Build range info
            VkAccelerationStructureBuildRangeInfoKHR buildRangeInfo = {};
            buildRangeInfo.primitiveCount                           = numTriangles;

            CommandObjects cmdBuf = {};
            CHECK_CALL(CreateCommandBuffer(pRenderer, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, &cmdBuf));

            VkCommandBufferBeginInfo vkbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            vkbi.flags                    = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

            CHECK_CALL(vkBeginCommandBuffer(cmdBuf.CommandBuffer, &vkbi));

            const VkAccelerationStructureBuildRangeInfoKHR* pBuildRangeInfo = &buildRangeInfo;
            fn_vkCmdBuildAccelerationStructuresKHR(cmdBuf.CommandBuffer, 1, &buildGeometryInfo, &pBuildRangeInfo);

            CHECK_CALL(vkEndCommandBuffer(cmdBuf.CommandBuffer));

            CHECK_CALL(ExecuteCommandBuffer(pRenderer, &cmdBuf));

            if (!WaitForGpu(pRenderer))
            {
                assert(false && "WaitForGpu failed");
            }
        }
        DestroyBuffer(pRenderer, &scratchBuffer);
    }
}

void CreateTLAS(
    VulkanRenderer*                  pRenderer,
    const VulkanAccelStruct&         sphereBLAS,
    const VulkanAccelStruct&         boxBLAS,
    VulkanAccelStruct*               pTLAS,
    std::vector<MaterialParameters>& outMaterialParams)
{
    // clang-format off
   std::vector<glm::mat3x4> transforms = {
      // Rough plastic sphere
      {{1.0f, 0.0f, 0.0f, -3.75f},
       {0.0f, 1.0f, 0.0f,  2.0f},
       {0.0f, 0.0f, 1.0f,  0.0f}},
      // Shiny plastic sphere 
      {{1.0f, 0.0f, 0.0f, -1.25f},
       {0.0f, 1.0f, 0.0f,  2.0f},
       {0.0f, 0.0f, 1.0f,  0.0f}},
      // Glass sphere
      {{1.0f, 0.0f, 0.0f,  1.25f},
       {0.0f, 1.0f, 0.0f,  2.0f},
       {0.0f, 0.0f, 1.0f,  0.0f}},
      // Gold sphere
      {{1.0f, 0.0f, 0.0f,  3.75f},
       {0.0f, 1.0f, 0.0f,  2.0f},
       {0.0f, 0.0f, 1.0f,  0.0f}},
      // Box
      {{1.0f, 0.0f, 0.0f,  0.0f},
       {0.0f, 1.0f, 0.0f,  0.5f},
       {0.0f, 0.0f, 1.0f,  0.0f}},
   };
    // clang-format on

    // Material params
    {
        // Rough plastic
        {
            MaterialParameters materialParams  = {};
            materialParams.baseColor           = vec3(1, 1, 1);
            materialParams.roughness           = 1.0f;
            materialParams.metallic            = 0;
            materialParams.specularReflectance = 0.0f;
            materialParams.ior                 = 0;

            outMaterialParams.push_back(materialParams);
        }

        // Shiny plastic
        {
            MaterialParameters materialParams  = {};
            materialParams.baseColor           = vec3(1, 1, 1);
            materialParams.roughness           = 0;
            materialParams.metallic            = 0;
            materialParams.specularReflectance = 0.5f;
            materialParams.ior                 = 0;

            outMaterialParams.push_back(materialParams);
        }

        // Glass
        {
            MaterialParameters materialParams  = {};
            materialParams.baseColor           = vec3(1, 1, 1);
            materialParams.roughness           = 0;
            materialParams.metallic            = 0;
            materialParams.specularReflectance = 0.0f;
            materialParams.ior                 = 1.50f;

            outMaterialParams.push_back(materialParams);
        }

        // Gold with a bit of roughness
        {
            MaterialParameters materialParams  = {};
            materialParams.baseColor           = F0_MetalGold;
            materialParams.roughness           = 0.30f;
            materialParams.metallic            = 1;
            materialParams.specularReflectance = 0.0f;
            materialParams.ior                 = 0;

            outMaterialParams.push_back(materialParams);
        }

        // Box
        {
            MaterialParameters materialParams  = {};
            materialParams.baseColor           = vec3(0.6f, 0.7f, 0.75f);
            materialParams.roughness           = 1.0f;
            materialParams.metallic            = 0;
            materialParams.specularReflectance = 0.0f;
            materialParams.ior                 = 0;

            outMaterialParams.push_back(materialParams);
        }
    }

    std::vector<VkAccelerationStructureInstanceKHR> instances;
    {
        VkAccelerationStructureInstanceKHR instance = {};
        instance.mask                               = 0xFF;
        instance.flags                              = VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
        instance.accelerationStructureReference     = GetDeviceAddress(pRenderer, sphereBLAS.AccelStruct);

        uint32_t transformIdx = 0;

        // Rough plastic sphere
        memcpy(&instance.transform, &transforms[transformIdx], sizeof(glm::mat3x4));
        instances.push_back(instance);
        ++transformIdx;

        // Shiny plastic sphere
        memcpy(&instance.transform, &transforms[transformIdx], sizeof(glm::mat3x4));
        instances.push_back(instance);
        ++transformIdx;

        // Glass sphere
        VkAccelerationStructureInstanceKHR thisInstance = instance;
        thisInstance.flags                              = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR | VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
        memcpy(&thisInstance.transform, &transforms[transformIdx], sizeof(glm::mat3x4));
        instances.push_back(thisInstance);
        ++transformIdx;

        // Gold sphere
        memcpy(&instance.transform, &transforms[transformIdx], sizeof(glm::mat3x4));
        instances.push_back(instance);
        ++transformIdx;

        // Box
        instance.accelerationStructureReference = GetDeviceAddress(pRenderer, boxBLAS.AccelStruct);
        memcpy(&instance.transform, &transforms[transformIdx], sizeof(glm::mat3x4));
        instances.push_back(instance);
        ++transformIdx;
    }

    VulkanBuffer instanceBuffer = {};
    CHECK_CALL(CreateBuffer(
        pRenderer,
        SizeInBytes(instances),
        DataPtr(instances),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
        0,
        &instanceBuffer));

    // Get acceleration structure build size

    // Geometry
    VkAccelerationStructureGeometryKHR geometry    = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.flags                                 = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geometry.geometryType                          = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances.sType              = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geometry.geometry.instances.arrayOfPointers    = VK_FALSE;
    geometry.geometry.instances.data.deviceAddress = GetDeviceAddress(pRenderer, &instanceBuffer);

    // Build geometry info - fill out enough to get build sizes
    VkAccelerationStructureBuildGeometryInfoKHR buildGeometryInfo = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    buildGeometryInfo.type                                        = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildGeometryInfo.flags                                       = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildGeometryInfo.mode                                        = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildGeometryInfo.geometryCount                               = 1;
    buildGeometryInfo.pGeometries                                 = &geometry;

    // Get acceleration structure build size
    VkAccelerationStructureBuildSizesInfoKHR buildSizesInfo = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    const uint32_t                           numInstances   = CountU32(instances);
    fn_vkGetAccelerationStructureBuildSizesKHR(
        pRenderer->Device,
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &buildGeometryInfo,
        &numInstances,
        &buildSizesInfo);

    // Create scratch buffer
    VulkanBuffer scratchBuffer = {};
    {
        // Get acceleration structure properties
        //
        // Obviously this can be cached if it's accessed frequently.
        //
        VkPhysicalDeviceAccelerationStructurePropertiesKHR accelStructProperties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2                        properties            = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext                                                         = &accelStructProperties;
        vkGetPhysicalDeviceProperties2(pRenderer->PhysicalDevice, &properties);

        VkBufferUsageFlags usageFlags = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

        CHECK_CALL(CreateBuffer(
            pRenderer,                                                            // pRenderer
            buildSizesInfo.buildScratchSize,                                      // srcSize
            usageFlags,                                                           // usageFlags
            VMA_MEMORY_USAGE_GPU_ONLY,                                            // memoryUsage
            accelStructProperties.minAccelerationStructureScratchOffsetAlignment, // minAlignment
            &scratchBuffer));                                                     // pBuffer
    }

    // Create acceleration structure buffer
    {
        VkBufferUsageFlags usageFlags = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;

        CHECK_CALL(CreateBuffer(
            pRenderer,
            buildSizesInfo.accelerationStructureSize,
            usageFlags,
            VMA_MEMORY_USAGE_GPU_ONLY,
            0,
            &pTLAS->Buffer));
    }

    // Create acceleration structure object
    {
        VkAccelerationStructureCreateInfoKHR createInfo = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        createInfo.buffer                               = pTLAS->Buffer.Buffer;
        createInfo.offset                               = 0;
        createInfo.size                                 = buildSizesInfo.accelerationStructureSize;
        createInfo.type                                 = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;

        CHECK_CALL(fn_vkCreateAccelerationStructureKHR(pRenderer->Device, &createInfo, nullptr, &pTLAS->AccelStruct));
    }

    // Build acceleration structure
    {
        buildGeometryInfo.dstAccelerationStructure  = pTLAS->AccelStruct;
        buildGeometryInfo.scratchData.deviceAddress = GetDeviceAddress(pRenderer, &scratchBuffer);

        // Build range info
        VkAccelerationStructureBuildRangeInfoKHR buildRangeInfo = {};
        buildRangeInfo.primitiveCount                           = numInstances;

        CommandObjects cmdBuf = {};
        CHECK_CALL(CreateCommandBuffer(pRenderer, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, &cmdBuf));

        VkCommandBufferBeginInfo vkbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkbi.flags                    = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        CHECK_CALL(vkBeginCommandBuffer(cmdBuf.CommandBuffer, &vkbi));

        const VkAccelerationStructureBuildRangeInfoKHR* pBuildRangeInfo = &buildRangeInfo;
        fn_vkCmdBuildAccelerationStructuresKHR(cmdBuf.CommandBuffer, 1, &buildGeometryInfo, &pBuildRangeInfo);

        CHECK_CALL(vkEndCommandBuffer(cmdBuf.CommandBuffer));

        CHECK_CALL(ExecuteCommandBuffer(pRenderer, &cmdBuf));

        if (!WaitForGpu(pRenderer))
        {
            assert(false && "WaitForGpu failed");
        }
    }
    DestroyBuffer(pRenderer, &instanceBuffer);
    DestroyBuffer(pRenderer, &scratchBuffer);
}

void CreateAccumTexture(VulkanRenderer* pRenderer, VulkanImage* pImage)
{
    CHECK_CALL(CreateImage(
        pRenderer,
        VK_IMAGE_TYPE_2D,
        VK_IMAGE_USAGE_STORAGE_BIT,
        gWindowWidth,
        gWindowHeight,
        1,
        VK_FORMAT_R32G32B32A32_SFLOAT,
        1,
        1,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VMA_MEMORY_USAGE_GPU_ONLY,
        pImage));

    CHECK_CALL(TransitionImageLayout(
        pRenderer,
        pImage->Image,
        GREX_ALL_SUBRESOURCES,
        VK_IMAGE_ASPECT_COLOR_BIT,
        RESOURCE_STATE_UNKNOWN,
        RESOURCE_STATE_COMMON));
}

void CreateIBLTextures(
    VulkanRenderer* pRenderer,
    VulkanImage*    pBRDFLUT,
    IBLTextures&    outIBLTextures)
{
    // BRDF LUT
    {
        auto bitmap = LoadImage32f(GetAssetPath("IBL/brdf_lut.hdr"));
        if (bitmap.Empty())
        {
            assert(false && "Load image failed");
            return;
        }

        CHECK_CALL(CreateTexture(
            pRenderer,
            bitmap.GetWidth(),
            bitmap.GetHeight(),
            VK_FORMAT_R32G32B32A32_SFLOAT,
            bitmap.GetSizeInBytes(),
            bitmap.GetPixels(),
            pBRDFLUT));
    }

    // IBL file
    auto iblFile = GetAssetPath("IBL/old_depot_4k.ibl");

    IBLMaps ibl = {};
    if (!LoadIBLMaps32f(iblFile, &ibl))
    {
        GREX_LOG_ERROR("failed to load: " << iblFile);
        return;
    }

    outIBLTextures.envNumLevels = ibl.numLevels;

    // Irradiance
    {
        CHECK_CALL(CreateTexture(
            pRenderer,
            ibl.irradianceMap.GetWidth(),
            ibl.irradianceMap.GetHeight(),
            VK_FORMAT_R32G32B32A32_SFLOAT,
            ibl.irradianceMap.GetSizeInBytes(),
            ibl.irradianceMap.GetPixels(),
            &outIBLTextures.irrTexture));
    }

    // Environment
    {
        const uint32_t pixelStride = ibl.environmentMap.GetPixelStride();
        const uint32_t rowStride   = ibl.environmentMap.GetRowStride();

        std::vector<MipOffset> mipOffsets;
        uint32_t               levelOffset = 0;
        uint32_t               levelWidth  = ibl.baseWidth;
        uint32_t               levelHeight = ibl.baseHeight;
        for (uint32_t i = 0; i < ibl.numLevels; ++i)
        {
            MipOffset mipOffset = {};
            mipOffset.Offset    = levelOffset;
            mipOffset.RowStride = rowStride;

            mipOffsets.push_back(mipOffset);

            levelOffset += (rowStride * levelHeight);
            levelWidth >>= 1;
            levelHeight >>= 1;
        }

        CHECK_CALL(CreateTexture(
            pRenderer,
            ibl.baseWidth,
            ibl.baseHeight,
            VK_FORMAT_R32G32B32A32_SFLOAT,
            mipOffsets,
            ibl.environmentMap.GetSizeInBytes(),
            ibl.environmentMap.GetPixels(),
            &outIBLTextures.envTexture));
    }

    GREX_LOG_INFO("Loaded " << iblFile);
}

void CreateDescriptorBuffer(
    VulkanRenderer*       pRenderer,
    VkDescriptorSetLayout descriptorSetLayout,
    VulkanBuffer*         pBuffer)
{
    VkDeviceSize size = 0;
    fn_vkGetDescriptorSetLayoutSizeEXT(pRenderer->Device, descriptorSetLayout, &size);

    VkBufferUsageFlags usageFlags =
        VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    CHECK_CALL(CreateBuffer(
        pRenderer,  // pRenderer
        size,       // srcSize
        nullptr,    // pSrcData
        usageFlags, // usageFlags
        0,          // minAlignment
        pBuffer));  // pBuffer
}

void WriteDescriptors(
    VulkanRenderer*          pRenderer,
    VkDescriptorSetLayout    descriptorSetLayout,
    VulkanBuffer*            pDescriptorBuffer,
    const VulkanBuffer&      sceneParamsBuffer,
    const VulkanAccelStruct& accelStruct,
    const VulkanImage&       accumTexture,
    const VulkanBuffer&      rayGenSamplesBuffer,
    const VulkanBuffer&      sceneParams,
    const Geometry&          sphereGeometry,
    const Geometry&          boxGeometry,
    const VulkanBuffer&      materialParamsBuffer,
    const IBLTextures&       iblTextures,
    VkSampler                iblSampler,
    VkImageView*             pAccumImageView,
    VkImageView*             pIBLImageView)
{
    char* pDescriptorBufferStartAddress = nullptr;
    CHECK_CALL(vmaMapMemory(
        pRenderer->Allocator,
        pDescriptorBuffer->Allocation,
        reinterpret_cast<void**>(&pDescriptorBufferStartAddress)));

    // Scene params (b5)
    WriteDescriptor(
        pRenderer,
        pDescriptorBufferStartAddress,
        descriptorSetLayout,
        5, // binding
        0, // arrayElement
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        &sceneParamsBuffer);

    // Acceleration strcutured (t0)
    WriteDescriptor(
        pRenderer,
        pDescriptorBufferStartAddress,
        descriptorSetLayout,
        0, // binding,
        0, // arrayElement,
        &accelStruct);

    //
    // NOTE: Ouput texture (u1) will be updated per frame
    //

    // Accumulation texture (u2)
    {
        VkImageView imageView = VK_NULL_HANDLE;
        CHECK_CALL(CreateImageView(
            pRenderer,
            &accumTexture,
            VK_IMAGE_VIEW_TYPE_2D,
            VK_FORMAT_R32G32B32A32_SFLOAT,
            GREX_ALL_SUBRESOURCES,
            &imageView));
        *pAccumImageView = imageView;

        WriteDescriptor(
            pRenderer,
            pDescriptorBufferStartAddress,
            descriptorSetLayout,
            2, // binding
            0, // arrayElement
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            imageView,
            VK_IMAGE_LAYOUT_GENERAL);
    }

    // Ray generation samples (u3)
    WriteDescriptor(
        pRenderer,
        pDescriptorBufferStartAddress,
        descriptorSetLayout,
        3, // binding
        0, // arrayElement
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        &rayGenSamplesBuffer);

    // Geometry
    {
        const uint32_t kNumSpheres            = 4;
        const uint32_t kIndexBufferBinding    = 20; // Index buffer (t20)
        const uint32_t kPositionBufferBinding = 25; // Position buffer (t25)
        const uint32_t kNormalBufferBinding   = 30; // Normal buffer (t30)

        uint32_t arrayElement = 0;

        // Spheres
        for (uint32_t i = 0; i < kNumSpheres; ++i, ++arrayElement)
        {
            // Index buffer (t20)
            WriteDescriptor(
                pRenderer,
                pDescriptorBufferStartAddress,
                descriptorSetLayout,
                kIndexBufferBinding,
                arrayElement,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                &sphereGeometry.indexBuffer);

            // Position buffer (t25)
            WriteDescriptor(
                pRenderer,
                pDescriptorBufferStartAddress,
                descriptorSetLayout,
                kPositionBufferBinding,
                arrayElement,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                &sphereGeometry.positionBuffer);

            // Normal buffer (t30)
            WriteDescriptor(
                pRenderer,
                pDescriptorBufferStartAddress,
                descriptorSetLayout,
                kNormalBufferBinding,
                arrayElement,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                &sphereGeometry.normalBuffer);
        }

        // Box
        {
            // Index buffer (t20)
            WriteDescriptor(
                pRenderer,
                pDescriptorBufferStartAddress,
                descriptorSetLayout,
                kIndexBufferBinding,
                arrayElement,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                &boxGeometry.indexBuffer);

            // Position buffer (t25)
            WriteDescriptor(
                pRenderer,
                pDescriptorBufferStartAddress,
                descriptorSetLayout,
                kPositionBufferBinding,
                arrayElement,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                &boxGeometry.positionBuffer);

            // Normal buffer (t30)
            WriteDescriptor(
                pRenderer,
                pDescriptorBufferStartAddress,
                descriptorSetLayout,
                kNormalBufferBinding,
                arrayElement,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                &boxGeometry.normalBuffer);
        }
    }

    // Material params (t9)
    WriteDescriptor(
        pRenderer,
        pDescriptorBufferStartAddress,
        descriptorSetLayout,
        9, // binding
        0, // arrayElement
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        &materialParamsBuffer);

    // IBL environment texture (t100)
    {
        CHECK_CALL(CreateImageView(
            pRenderer,
            &iblTextures.envTexture,
            VK_IMAGE_VIEW_TYPE_2D,
            VK_FORMAT_R32G32B32A32_SFLOAT,
            0,
            iblTextures.envNumLevels,
            0,
            1,
            pIBLImageView));

        WriteDescriptor(
            pRenderer,
            pDescriptorBufferStartAddress,
            descriptorSetLayout,
            100, // binding
            0,   // arrayElement
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            *pIBLImageView,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    // IBL sampler (s10)
    WriteDescriptor(
        pRenderer,
        pDescriptorBufferStartAddress,
        descriptorSetLayout,
        10, // binding
        0,  // arrayElement
        iblSampler);

    vmaUnmapMemory(pRenderer->Allocator, pDescriptorBuffer->Allocation);
}