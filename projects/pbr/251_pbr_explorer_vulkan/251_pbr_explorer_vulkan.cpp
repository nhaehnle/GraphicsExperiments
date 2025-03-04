#include "window.h"

#include "vk_renderer.h"
#include "bitmap.h"
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

#define DISTRIBUTION_TROWBRIDGE_REITZ 0
#define DISTRIBUTION_BECKMANN         1
#define DISTRIBUTION_BLINN_PHONG      2

#define FRESNEL_SCHLICK_ROUGHNESS 0
#define FRESNEL_SCHLICK           1
#define FRESNEL_COOK_TORRANCE     2
#define FRESNEL_NONE              3

#define GEOMETRY_SMITH                 0
#define GEOMETRY_IMPLICIT              1
#define GEOMETRY_NEUMANN               2
#define GEOMETRY_COOK_TORRANCE         3
#define GEOMETRY_KELEMEN               4
#define GEOMETRY_BECKMANN              5
#define GEOMETRY_GGX1                  6
#define GEOMETRY_GGX2                  7
#define GEOMETRY_SCHLICK_GGX           8
#define GEOMETRY_SMITH_CORRELATED      9
#define GEOMETRY_SMITH_CORRELATED_FAST 10

// This will be passed in via constant buffer
struct Light
{
    vec3     position;
    uint32_t __pad;
    vec3     color;
    float    intensity;
};

struct PBRSceneParameters
{
    mat4     viewProjectionMatrix;
    vec3     eyePosition;
    uint32_t numLights;
    Light    lights[8];
    uint32_t iblNumEnvLevels;
    uint32_t iblIndex;
    float    iblDiffuseStrength;
    float    iblSpecularStrength;
};

struct EnvSceneParameters
{
    mat4 MVP;
    uint IBLIndex;
};

struct DrawParameters
{
    mat4 ModelMatrix;
    uint MaterialIndex;
};

struct MaterialParameters
{
    vec3     baseColor;
    float    roughness;
    float    metallic;
    float    specular;
    uint     directComponentMode;
    uint32_t D_Func;
    uint32_t F_Func;
    uint32_t G_Func;
    uint32_t indirectComponentMode;
    uint32_t indirectSpecularMode;
    uint32_t drawMode;
};

struct GeometryBuffers
{
    uint32_t     numIndices;
    VulkanBuffer indexBuffer;
    VulkanBuffer positionBuffer;
    VulkanBuffer texCoordBuffer;
    VulkanBuffer normalBuffer;
    VulkanBuffer tangentBuffer;
    VulkanBuffer bitangentBuffer;
};

// =============================================================================
// Constants
// =============================================================================

const std::vector<std::string> gDistributionNames = {
    "GGX (Trowbridge-Reitz)",
    "Beckmann",
    "Blinn-Phong",
};

const std::vector<std::string> gFresnelNames = {
    "Schlick with Roughness",
    "Schlick",
    "CookTorrance",
    "None",
};

const std::vector<std::string> gGeometryNames = {
    "Smith",
    "Implicit",
    "Neumann",
    "Cook-Torrance",
    "Kelemen",
    "Beckmann",
    "GGX1",
    "GGX2",
    "SchlickGGX",
    "Smith Correlated",
    "Smith Correlated Fast",
};

const std::vector<std::string> gDirectComponentModeNames = {
    "All",
    "Distribution",
    "Fresnel",
    "Geometry",
    "Diffuse",
    "Radiance",
    "kD",
    "Specular",
    "BRDF",
};

const std::vector<std::string> gIndirectComponentModeNames = {
    "All",
    "Diffuse",
    "Specular"};

const std::vector<std::string> gIndirectSpecularModeNames = {
    "LUT",
    "Approx Lazarov",
    "Approx Polynomial",
    "Approx Karis",
};

const std::vector<std::string> gDrawModeNames = {
    "Full Lighting",
    "Direct",
    "Indirect",
};

const std::vector<std::string> gModelNames = {
    "Sphere",
    "Knob",
    "Monkey",
    "Teapot",
};

// =============================================================================
// Globals
// =============================================================================
static uint32_t gWindowWidth      = 1920;
static uint32_t gWindowHeight     = 1080;
static bool     gEnableDebug      = true;

static LPCWSTR gVSShaderName = L"vsmain";
static LPCWSTR gPSShaderName = L"psmain";

static float gTargetAngle = 0.0f;
static float gAngle       = 0.0f;

// clang-format off
static std::vector<MaterialParameters> gMaterialParams = {
    {F0_MetalCopper,         0.25f, 1.00f, 0.5f, 0, 0, 0, 0},
    {F0_MetalGold,           0.05f, 1.00f, 0.5f, 0, 0, 0, 0},
    {F0_MetalSilver,         0.18f, 1.00f, 0.5f, 0, 0, 0, 0},
    {F0_MetalZinc,           0.65f, 1.00f, 0.5f, 0, 0, 0, 0},
    {F0_MetalTitanium,       0.11f, 1.00f, 0.5f, 0, 0, 0, 0},
    {vec3(0.6f, 0.0f, 0.0f), 0.00f, 0.00f, 0.5f, 0, 0, 0, 0},
    {vec3(0.0f, 0.6f, 0.0f), 0.25f, 0.00f, 0.5f, 0, 0, 0, 0},
    {vec3(0.0f, 0.0f, 0.6f), 0.50f, 0.00f, 0.5f, 0, 0, 0, 0},
    {vec3(0.7f, 0.7f, 0.2f), 0.92f, 0.15f, 0.5f, 0, 0, 0, 0},
};
// clang-format on

static std::vector<std::string> gMaterialNames = {
    "Copper",
    "Gold",
    "Silver",
    "Zink",
    "Titanium",
    "Shiny Plastic",
    "Rough Plastic",
    "Rougher Plastic",
    "Roughest Plastic",
};

static uint32_t                 gNumLights           = 0;
static const uint32_t           gMaxIBLs             = 32;
static uint32_t                 gIBLIndex            = 0;
static std::vector<std::string> gIBLNames            = {};
static float                    gIBLDiffuseStrength  = 1.0f;
static float                    gIBLSpecularStrength = 1.0f;
static uint32_t                 gModelIndex          = 0;

void CreatePBRPipeline(VulkanRenderer* pRenderer, VulkanPipelineLayout* pLayout);
void CreateEnvironmentPipeline(VulkanRenderer* pRenderer, VulkanPipelineLayout* pLayout);
void CreateEnvironmentVertexBuffers(
    VulkanRenderer*  pRenderer,
    GeometryBuffers& outGeomtryBuffers);
void CreateMaterialModels(
    VulkanRenderer*               pRenderer,
    std::vector<GeometryBuffers>& outGeomtryBuffers);
void CreateIBLTextures(
    VulkanRenderer*           pRenderer,
    VulkanImage*              pBRDFLUT,
    std::vector<VulkanImage>& outIrradianceTextures,
    std::vector<VulkanImage>& outEnvironmentTextures,
    std::vector<uint32_t>&    outEnvNumLevels);
void CreateDescriptorBuffer(
    VulkanRenderer*       pRenderer,
    VkDescriptorSetLayout descriptorSetLayout,
    VulkanBuffer*         pBuffer);
void WritePBRDescriptors(
    VulkanRenderer*           pRenderer,
    VkDescriptorSetLayout     descriptorSetLayout,
    VulkanBuffer*             pDescriptorBuffer,
    const VulkanBuffer*       pSceneParamsBuffer,
    const VulkanBuffer*       pMaterialsBuffer,
    const VulkanImage*        pBRDFLUT,
    std::vector<VulkanImage>& pIrradianceTexture,
    std::vector<VulkanImage>& pEnvTexture);
void WriteEnvDescriptors(
    VulkanRenderer*           pRenderer,
    VkDescriptorSetLayout     descriptorSetLayout,
    VulkanBuffer*             pDescriptorBuffer,
    std::vector<VulkanImage>& envTextures);

/*
void CreatePBRRootSig(DxRenderer* pRenderer, ID3D12RootSignature** ppRootSig);
void CreateEnvironmentRootSig(DxRenderer* pRenderer, ID3D12RootSignature** ppRootSig);
void CreateDescriptorHeap(
    DxRenderer*            pRenderer,
    ID3D12DescriptorHeap** ppHeap);
    */

void MouseMove(int x, int y, int buttons)
{
    static int prevX = x;
    static int prevY = y;

    if (buttons & MOUSE_BUTTON_LEFT)
    {
        int dx = x - prevX;
        int dy = y - prevY;

        gTargetAngle += 0.25f * dx;
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

    VulkanFeatures features = {};
    if (!InitVulkan(renderer.get(), gEnableDebug, features))
    {
        return EXIT_FAILURE;
    }

    // *************************************************************************
    // Compile shaders
    // *************************************************************************
    // PBR shaders
    std::vector<uint32_t> spirvVS;
    std::vector<uint32_t> spirvFS;
    {
        std::string shaderSource = LoadString("projects/251_pbr_explorer/shaders.hlsl");
        if (shaderSource.empty())
        {
            assert(false && "no shader source");
            return EXIT_FAILURE;
        }

        std::string errorMsg;
        HRESULT     hr = CompileHLSL(shaderSource, "vsmain", "vs_6_0", &spirvVS, &errorMsg);
        if (FAILED(hr))
        {
            std::stringstream ss;
            ss << "\n"
               << "Shader compiler error (VS): " << errorMsg << "\n";
            GREX_LOG_ERROR(ss.str().c_str());
            assert(false);
            return EXIT_FAILURE;
        }

        hr = CompileHLSL(shaderSource, "psmain", "ps_6_0", &spirvFS, &errorMsg);
        if (FAILED(hr))
        {
            std::stringstream ss;
            ss << "\n"
               << "Shader compiler error (PS): " << errorMsg << "\n";
            GREX_LOG_ERROR(ss.str().c_str());
            assert(false);
            return EXIT_FAILURE;
        }
    }

    VkShaderModule shaderModuleVS = VK_NULL_HANDLE;
    {
        VkShaderModuleCreateInfo createInfo = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        createInfo.codeSize                 = SizeInBytes(spirvVS);
        createInfo.pCode                    = DataPtr(spirvVS);

        CHECK_CALL(vkCreateShaderModule(renderer->Device, &createInfo, nullptr, &shaderModuleVS));
    }

    VkShaderModule shaderModuleFS = VK_NULL_HANDLE;
    {
        VkShaderModuleCreateInfo createInfo = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        createInfo.codeSize                 = SizeInBytes(spirvFS);
        createInfo.pCode                    = DataPtr(spirvFS);

        CHECK_CALL(vkCreateShaderModule(renderer->Device, &createInfo, nullptr, &shaderModuleFS));
    }

    // Draw texture shaders
    std::vector<uint32_t> drawTextureSpirvVS;
    std::vector<uint32_t> drawTextureSpirvFS;
    {
        std::string shaderSource = LoadString("projects/251_pbr_explorer/drawtexture.hlsl");
        if (shaderSource.empty())
        {
            assert(false && "no shader source");
            return EXIT_FAILURE;
        }

        std::string errorMsg;
        HRESULT     hr = CompileHLSL(shaderSource, "vsmain", "vs_6_0", &drawTextureSpirvVS, &errorMsg);
        if (FAILED(hr))
        {
            std::stringstream ss;
            ss << "\n"
               << "Shader compiler error (VS): " << errorMsg << "\n";
            GREX_LOG_ERROR(ss.str().c_str());
            assert(false);
            return EXIT_FAILURE;
        }

        hr = CompileHLSL(shaderSource, "psmain", "ps_6_0", &drawTextureSpirvFS, &errorMsg);
        if (FAILED(hr))
        {
            std::stringstream ss;
            ss << "\n"
               << "Shader compiler error (PS): " << errorMsg << "\n";
            GREX_LOG_ERROR(ss.str().c_str());
            assert(false);
            return EXIT_FAILURE;
        }
    }

    VkShaderModule drawTextureShaderModuleVS = VK_NULL_HANDLE;
    {
        VkShaderModuleCreateInfo createInfo = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        createInfo.codeSize                 = SizeInBytes(drawTextureSpirvVS);
        createInfo.pCode                    = DataPtr(drawTextureSpirvVS);

        CHECK_CALL(vkCreateShaderModule(renderer->Device, &createInfo, nullptr, &drawTextureShaderModuleVS));
    }

    VkShaderModule drawTextureShaderModuleFS = VK_NULL_HANDLE;
    {
        VkShaderModuleCreateInfo createInfo = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        createInfo.codeSize                 = SizeInBytes(drawTextureSpirvFS);
        createInfo.pCode                    = DataPtr(drawTextureSpirvFS);

        CHECK_CALL(vkCreateShaderModule(renderer->Device, &createInfo, nullptr, &drawTextureShaderModuleFS));
    }

    // *************************************************************************
    // PBR pipeline layout
    // *************************************************************************
    VulkanPipelineLayout pbrPipelineLayout = {};
    CreatePBRPipeline(renderer.get(), &pbrPipelineLayout);

    // *************************************************************************
    // Environment pipeline layout
    // *************************************************************************
    VulkanPipelineLayout envPipelineLayout = {};
    CreateEnvironmentPipeline(renderer.get(), &envPipelineLayout);

    // *************************************************************************
    // PBR pipeline state object
    // *************************************************************************
    VkPipeline pbrPipelineState = VK_NULL_HANDLE;
    CHECK_CALL(CreateDrawNormalPipeline(
        renderer.get(),
        pbrPipelineLayout.PipelineLayout,
        shaderModuleVS,
        shaderModuleFS,
        GREX_DEFAULT_RTV_FORMAT,
        GREX_DEFAULT_DSV_FORMAT,
        &pbrPipelineState,
        false, // enable tangents
        VK_CULL_MODE_BACK_BIT,
        "vsmain",
        "psmain"));

    // *************************************************************************
    // Environment pipeline state object
    // *************************************************************************
    VkPipeline envPipelineState = VK_NULL_HANDLE;
    CHECK_CALL(CreateDrawTexturePipeline(
        renderer.get(),
        envPipelineLayout.PipelineLayout,
        drawTextureShaderModuleVS,
        drawTextureShaderModuleFS,
        GREX_DEFAULT_RTV_FORMAT,
        GREX_DEFAULT_DSV_FORMAT,
        &envPipelineState,
        VK_CULL_MODE_FRONT_BIT,
        "vsmain",
        "psmain"));

    // *************************************************************************
    // Material buffer
    // *************************************************************************
    VulkanBuffer pbrMaterialParamsBuffer = {};
    CHECK_CALL(CreateBuffer(
        renderer.get(),
        SizeInBytes(gMaterialParams),
        DataPtr(gMaterialParams),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU,
        0,
        &pbrMaterialParamsBuffer));

    // *************************************************************************
    // Constant buffers
    // *************************************************************************
    VulkanBuffer pbrSceneParamsBuffer;
    CHECK_CALL(CreateBuffer(
        renderer.get(),
        Align<size_t>(sizeof(PBRSceneParameters), 256),
        nullptr,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU,
        0,
        &pbrSceneParamsBuffer));

    // *************************************************************************
    // Environment vertex buffers
    // *************************************************************************
    GeometryBuffers envGeoBuffers;
    CreateEnvironmentVertexBuffers(
        renderer.get(),
        envGeoBuffers);

    // *************************************************************************
    // Material models
    // *************************************************************************
    std::vector<GeometryBuffers> matGeoBuffers;
    CreateMaterialModels(
        renderer.get(),
        matGeoBuffers);

    // *************************************************************************
    // Environment texture
    // *************************************************************************
    VulkanImage              brdfLUT;
    std::vector<VulkanImage> irrTextures;
    std::vector<VulkanImage> envTextures;
    std::vector<uint32_t>    envNumLevels;
    CreateIBLTextures(renderer.get(), &brdfLUT, irrTextures, envTextures, envNumLevels);

    // *************************************************************************
    // Descriptor buffers
    // *************************************************************************
    VulkanBuffer pbrDescriptorBuffer = {};
    CreateDescriptorBuffer(renderer.get(), pbrPipelineLayout.DescriptorSetLayout, &pbrDescriptorBuffer);

    WritePBRDescriptors(
        renderer.get(),
        pbrPipelineLayout.DescriptorSetLayout,
        &pbrDescriptorBuffer,
        &pbrSceneParamsBuffer,
        &pbrMaterialParamsBuffer,
        &brdfLUT,
        irrTextures,
        envTextures);

    VulkanBuffer envDescriptorBuffer = {};
    CreateDescriptorBuffer(renderer.get(), envPipelineLayout.DescriptorSetLayout, &envDescriptorBuffer);

    WriteEnvDescriptors(
        renderer.get(),
        envPipelineLayout.DescriptorSetLayout,
        &envDescriptorBuffer,
        envTextures);

    // *************************************************************************
    // Window
    // *************************************************************************
    auto window = Window::Create(gWindowWidth, gWindowHeight, "251_pbr_explorer_vulkan");
    if (!window)
    {
        assert(false && "Window::Create failed");
        return EXIT_FAILURE;
    }
    window->AddMouseMoveCallbacks(MouseMove);

    // *************************************************************************
    // Swapchain
    // *************************************************************************
    if (!InitSwapchain(renderer.get(), window->GetHWND(), window->GetWidth(), window->GetHeight()))
    {
        assert(false && "InitSwapchain failed");
        return EXIT_FAILURE;
    }

    // *************************************************************************
    // Render pass to draw ImGui
    // *************************************************************************
    std::vector<VulkanAttachmentInfo> colorAttachmentInfos = {
        {GREX_DEFAULT_RTV_FORMAT, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, renderer->SwapchainImageUsage}
    };

    VulkanRenderPass renderPass = {};
    CHECK_CALL(CreateRenderPass(renderer.get(), colorAttachmentInfos, {}, gWindowWidth, gWindowHeight, &renderPass));

    // *************************************************************************
    // Imgui
    // *************************************************************************
    if (!window->InitImGuiForVulkan(renderer.get(), renderPass.RenderPass))
    {
        assert(false && "Window::InitImGuiForVulkan failed");
        return EXIT_FAILURE;
    }

    // *************************************************************************
    // Swapchain image views, depth buffers/views
    // *************************************************************************
    std::vector<VkImage>     images;
    std::vector<VkImageView> imageViews;
    std::vector<VkImageView> depthViews;
    {
        CHECK_CALL(GetSwapchainImages(renderer.get(), images));

        for (auto& image : images)
        {
            // Create swap chain images
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

            imageViews.push_back(imageView);
        }

        size_t imageCount = images.size();

        std::vector<VulkanImage> depthImages;
        depthImages.resize(images.size());

        for (int depthIndex = 0; depthIndex < imageCount; depthIndex++)
        {
            // Create depth images
            CHECK_CALL(CreateDSV(renderer.get(), window->GetWidth(), window->GetHeight(), &depthImages[depthIndex]));

            VkImageViewCreateInfo createInfo           = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            createInfo.image                           = depthImages[depthIndex].Image;
            createInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
            createInfo.format                          = GREX_DEFAULT_DSV_FORMAT;
            createInfo.components                      = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
            createInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
            createInfo.subresourceRange.baseMipLevel   = 0;
            createInfo.subresourceRange.levelCount     = 1;
            createInfo.subresourceRange.baseArrayLayer = 0;
            createInfo.subresourceRange.layerCount     = 1;

            VkImageView depthView = VK_NULL_HANDLE;
            CHECK_CALL(vkCreateImageView(renderer->Device, &createInfo, nullptr, &depthView));

            depthViews.push_back(depthView);
        }
    }

    // *************************************************************************
    // Command buffer
    // *************************************************************************
    CommandObjects cmdBuf = {};
    {
        CHECK_CALL(CreateCommandBuffer(renderer.get(), 0, &cmdBuf));
    }

    // *************************************************************************
    // Persistent map scene parameters
    // *************************************************************************
    PBRSceneParameters* pPBRSceneParams = nullptr;
    vmaMapMemory(renderer->Allocator, pbrSceneParamsBuffer.Allocation, reinterpret_cast<void**>(&pPBRSceneParams));

    MaterialParameters* pMaterialParams = nullptr;
    vmaMapMemory(renderer->Allocator, pbrMaterialParamsBuffer.Allocation, reinterpret_cast<void**>(&pMaterialParams));

    // *************************************************************************
    // Main loop
    // *************************************************************************
    VkClearValue clearValues[2];
    clearValues[0].color = {
        {0.0f, 0.0f, 0.2f, 1.0f}
    };
    clearValues[1].depthStencil = {1.0f, 0};

    while (window->PollEvents())
    {
        window->ImGuiNewFrameVulkan();

        if (ImGui::Begin("Scene"))
        {
            static const char* currentIBLName = gIBLNames[0].c_str();
            if (ImGui::BeginCombo("IBL", currentIBLName))
            {
                for (size_t i = 0; i < gIBLNames.size(); ++i)
                {
                    bool isSelected = (currentIBLName == gIBLNames[i]);
                    if (ImGui::Selectable(gIBLNames[i].c_str(), isSelected))
                    {
                        currentIBLName = gIBLNames[i].c_str();
                        gIBLIndex      = static_cast<uint32_t>(i);
                    }
                    if (isSelected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::SliderFloat("IBL Diffuse Strength", &gIBLDiffuseStrength, 0.0f, 2.0f);
            ImGui::SliderFloat("IBL Specular Strength", &gIBLSpecularStrength, 0.0f, 2.0f);
            ImGui::SliderInt("Number of Lights", reinterpret_cast<int*>(&gNumLights), 0, 4);

            ImGui::Separator();

            static const char* currentModelName = gModelNames[0].c_str();
            if (ImGui::BeginCombo("Model", currentModelName))
            {
                for (size_t i = 0; i < gModelNames.size(); ++i)
                {
                    bool isSelected = (currentModelName == gModelNames[i]);
                    if (ImGui::Selectable(gModelNames[i].c_str(), isSelected))
                    {
                        currentModelName = gModelNames[i].c_str();
                        gModelIndex      = static_cast<uint32_t>(i);
                    }
                    if (isSelected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::End();

        if (ImGui::Begin("Material Parameters"))
        {
            static std::vector<const char*> currentDrawModeNames(gMaterialParams.size(), gDrawModeNames[0].c_str());
            static std::vector<const char*> currentDirectComponentModeNames(gMaterialParams.size(), gDirectComponentModeNames[0].c_str());
            static std::vector<const char*> currentDistributionNames(gMaterialParams.size(), gDistributionNames[0].c_str());
            static std::vector<const char*> currentFresnelNames(gMaterialParams.size(), gFresnelNames[0].c_str());
            static std::vector<const char*> currentGeometryNames(gMaterialParams.size(), gGeometryNames[0].c_str());
            static std::vector<const char*> currentIndirectComponentModeNames(gMaterialParams.size(), gIndirectComponentModeNames[0].c_str());
            static std::vector<const char*> currentIndirectSpecularModeNames(gMaterialParams.size(), gIndirectSpecularModeNames[0].c_str());

            for (uint32_t matIdx = 0; matIdx < gMaterialNames.size(); ++matIdx)
            {
                if (ImGui::TreeNodeEx(gMaterialNames[matIdx].c_str(), ImGuiTreeNodeFlags_DefaultOpen))
                {
                    // DrawMode
                    if (ImGui::BeginCombo("DrawMode", currentDrawModeNames[matIdx]))
                    {
                        for (size_t i = 0; i < gDrawModeNames.size(); ++i)
                        {
                            bool isSelected = (currentDrawModeNames[matIdx] == gDrawModeNames[i]);
                            if (ImGui::Selectable(gDrawModeNames[i].c_str(), isSelected))
                            {
                                currentDrawModeNames[matIdx]     = gDrawModeNames[i].c_str();
                                pMaterialParams[matIdx].drawMode = static_cast<uint32_t>(i);
                            }
                            if (isSelected)
                            {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                    }
                    // Direct Light Params
                    if (ImGui::TreeNodeEx("Direct Light Parames", ImGuiTreeNodeFlags_DefaultOpen))
                    {
                        // DirectComponentMode
                        if (ImGui::BeginCombo("Direct Component Mode", currentDirectComponentModeNames[matIdx]))
                        {
                            for (size_t i = 0; i < gDirectComponentModeNames.size(); ++i)
                            {
                                bool isSelected = (currentDirectComponentModeNames[matIdx] == gDirectComponentModeNames[i]);
                                if (ImGui::Selectable(gDirectComponentModeNames[i].c_str(), isSelected))
                                {
                                    currentDirectComponentModeNames[matIdx]     = gDirectComponentModeNames[i].c_str();
                                    pMaterialParams[matIdx].directComponentMode = static_cast<uint32_t>(i);
                                }
                                if (isSelected)
                                {
                                    ImGui::SetItemDefaultFocus();
                                }
                            }
                            ImGui::EndCombo();
                        }
                        // Distribution
                        if (ImGui::BeginCombo("Distribution", currentDistributionNames[matIdx]))
                        {
                            for (size_t i = 0; i < gDistributionNames.size(); ++i)
                            {
                                bool isSelected = (currentDistributionNames[matIdx] == gDistributionNames[i]);
                                if (ImGui::Selectable(gDistributionNames[i].c_str(), isSelected))
                                {
                                    currentDistributionNames[matIdx] = gDistributionNames[i].c_str();
                                    pMaterialParams[matIdx].D_Func   = static_cast<uint32_t>(i);
                                }
                                if (isSelected)
                                {
                                    ImGui::SetItemDefaultFocus();
                                }
                            }
                            ImGui::EndCombo();
                        }
                        // Fresnel
                        if (ImGui::BeginCombo("Fresnel", currentFresnelNames[matIdx]))
                        {
                            for (size_t i = 0; i < gFresnelNames.size(); ++i)
                            {
                                bool isSelected = (currentFresnelNames[matIdx] == gFresnelNames[i]);
                                if (ImGui::Selectable(gFresnelNames[i].c_str(), isSelected))
                                {
                                    currentFresnelNames[matIdx]    = gFresnelNames[i].c_str();
                                    pMaterialParams[matIdx].F_Func = static_cast<uint32_t>(i);
                                }
                                if (isSelected)
                                {
                                    ImGui::SetItemDefaultFocus();
                                }
                            }
                            ImGui::EndCombo();
                        }
                        // Geometry
                        if (ImGui::BeginCombo("Geometry", currentGeometryNames[matIdx]))
                        {
                            for (size_t i = 0; i < gGeometryNames.size(); ++i)
                            {
                                bool isSelected = (currentGeometryNames[matIdx] == gGeometryNames[i]);
                                if (ImGui::Selectable(gGeometryNames[i].c_str(), isSelected))
                                {
                                    currentGeometryNames[matIdx]   = gGeometryNames[i].c_str();
                                    pMaterialParams[matIdx].G_Func = static_cast<uint32_t>(i);
                                }
                                if (isSelected)
                                {
                                    ImGui::SetItemDefaultFocus();
                                }
                            }
                            ImGui::EndCombo();
                        }

                        ImGui::TreePop();
                    }
                    // Indirect Light Params
                    if (ImGui::TreeNodeEx("Indirect Light Parames", ImGuiTreeNodeFlags_DefaultOpen))
                    {
                        // IndirectComponentMode
                        if (ImGui::BeginCombo("Indirect Component Mode", currentIndirectComponentModeNames[matIdx]))
                        {
                            for (size_t i = 0; i < gIndirectComponentModeNames.size(); ++i)
                            {
                                bool isSelected = (currentIndirectComponentModeNames[matIdx] == gIndirectComponentModeNames[i]);
                                if (ImGui::Selectable(gIndirectComponentModeNames[i].c_str(), isSelected))
                                {
                                    currentIndirectComponentModeNames[matIdx]     = gIndirectComponentModeNames[i].c_str();
                                    pMaterialParams[matIdx].indirectComponentMode = static_cast<uint32_t>(i);
                                }
                                if (isSelected)
                                {
                                    ImGui::SetItemDefaultFocus();
                                }
                            }
                            ImGui::EndCombo();
                        }
                        // Specular Mode
                        if (ImGui::BeginCombo("Specular Mode", currentIndirectSpecularModeNames[matIdx]))
                        {
                            for (size_t i = 0; i < gIndirectSpecularModeNames.size(); ++i)
                            {
                                bool isSelected = (currentIndirectSpecularModeNames[matIdx] == gIndirectSpecularModeNames[i]);
                                if (ImGui::Selectable(gIndirectSpecularModeNames[i].c_str(), isSelected))
                                {
                                    currentIndirectSpecularModeNames[matIdx]     = gIndirectSpecularModeNames[i].c_str();
                                    pMaterialParams[matIdx].indirectSpecularMode = static_cast<uint32_t>(i);
                                }
                                if (isSelected)
                                {
                                    ImGui::SetItemDefaultFocus();
                                }
                            }
                            ImGui::EndCombo();
                        }

                        ImGui::TreePop();
                    }

                    ImGui::SliderFloat("Roughness", &(pMaterialParams[matIdx].roughness), 0.0f, 1.0f);
                    ImGui::SliderFloat("Metallic", &(pMaterialParams[matIdx].metallic), 0.0f, 1.0f);
                    ImGui::SliderFloat("Specular", &(pMaterialParams[matIdx].specular), 0.0f, 1.0f);
                    ImGui::ColorPicker3("Albedo", reinterpret_cast<float*>(&(pMaterialParams[matIdx].baseColor)), ImGuiColorEditFlags_NoInputs);

                    ImGui::TreePop();
                }

                ImGui::Separator();
            }
        }
        ImGui::End();

        // ---------------------------------------------------------------------

        UINT bufferIndex = 0;
        if (AcquireNextImage(renderer.get(), &bufferIndex))
        {
            assert(false && "AcquireNextImage failed");
            break;
        }

        VkCommandBufferBeginInfo vkbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkbi.flags                    = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        CHECK_CALL(vkBeginCommandBuffer(cmdBuf.CommandBuffer, &vkbi));

        {
            CmdTransitionImageLayout(
                cmdBuf.CommandBuffer,
                images[bufferIndex],
                GREX_ALL_SUBRESOURCES,
                VK_IMAGE_ASPECT_COLOR_BIT,
                RESOURCE_STATE_PRESENT,
                RESOURCE_STATE_RENDER_TARGET);

            VkRenderingAttachmentInfo colorAttachment = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            colorAttachment.imageView                 = imageViews[bufferIndex];
            colorAttachment.imageLayout               = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
            colorAttachment.loadOp                    = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAttachment.storeOp                   = VK_ATTACHMENT_STORE_OP_STORE;
            colorAttachment.clearValue                = clearValues[0];

            VkRenderingAttachmentInfo depthAttachment = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            depthAttachment.imageView                 = depthViews[bufferIndex];
            depthAttachment.imageLayout               = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depthAttachment.loadOp                    = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthAttachment.storeOp                   = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depthAttachment.clearValue                = clearValues[1];

            VkRenderingInfo vkri          = {VK_STRUCTURE_TYPE_RENDERING_INFO};
            vkri.layerCount               = 1;
            vkri.colorAttachmentCount     = 1;
            vkri.pColorAttachments        = &colorAttachment;
            vkri.pDepthAttachment         = &depthAttachment;
            vkri.renderArea.extent.width  = gWindowWidth;
            vkri.renderArea.extent.height = gWindowHeight;

            vkCmdBeginRendering(cmdBuf.CommandBuffer, &vkri);

            VkViewport viewport = {0, static_cast<float>(gWindowHeight), static_cast<float>(gWindowWidth), -static_cast<float>(gWindowHeight), 0.0f, 1.0f};
            vkCmdSetViewport(cmdBuf.CommandBuffer, 0, 1, &viewport);

            VkRect2D scissor = {0, 0, gWindowWidth, gWindowHeight};
            vkCmdSetScissor(cmdBuf.CommandBuffer, 0, 1, &scissor);

            // Smooth out the rotation on Y
            gAngle += (gTargetAngle - gAngle) * 0.1f;

            // Camera matrices - spin the camera around the target
            mat4 transformEyeMat     = glm::rotate(glm::radians(-gAngle), vec3(0, 1, 0));
            vec3 startingEyePosition = vec3(0, 3, 8);
            vec3 eyePosition         = transformEyeMat * vec4(startingEyePosition, 1);
            mat4 viewMat             = glm::lookAt(eyePosition, vec3(0, 0, 0), vec3(0, 1, 0));
            mat4 projMat             = glm::perspective(glm::radians(60.0f), gWindowWidth / static_cast<float>(gWindowHeight), 0.1f, 10000.0f);

            // Set constant buffer values
            //
            // We're rotating everything in the world...including the lights
            //
            pPBRSceneParams->viewProjectionMatrix = projMat * viewMat;
            pPBRSceneParams->eyePosition          = eyePosition;
            pPBRSceneParams->numLights            = gNumLights;
            pPBRSceneParams->lights[0].position   = vec3(3, 10, 0);
            pPBRSceneParams->lights[0].color      = vec3(1, 1, 1);
            pPBRSceneParams->lights[0].intensity  = 1.5f;
            pPBRSceneParams->lights[1].position   = vec3(-8, 1, 4);
            pPBRSceneParams->lights[1].color      = vec3(0.85f, 0.95f, 0.81f);
            pPBRSceneParams->lights[1].intensity  = 0.4f;
            pPBRSceneParams->lights[2].position   = vec3(0, 8, -8);
            pPBRSceneParams->lights[2].color      = vec3(0.89f, 0.89f, 0.97f);
            pPBRSceneParams->lights[2].intensity  = 0.95f;
            pPBRSceneParams->lights[3].position   = vec3(15, 0, 0);
            pPBRSceneParams->lights[3].color      = vec3(0.92f, 0.5f, 0.7f);
            pPBRSceneParams->lights[3].intensity  = 0.5f;
            pPBRSceneParams->iblNumEnvLevels      = envNumLevels[gIBLIndex];
            pPBRSceneParams->iblIndex             = gIBLIndex;
            pPBRSceneParams->iblDiffuseStrength   = gIBLDiffuseStrength;
            pPBRSceneParams->iblSpecularStrength  = gIBLSpecularStrength;

            // Draw environment
            {
                VkDescriptorBufferBindingInfoEXT descriptorBufferBindingInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT};
                descriptorBufferBindingInfo.pNext                            = nullptr;
                descriptorBufferBindingInfo.address                          = GetDeviceAddress(renderer.get(), &envDescriptorBuffer);
                descriptorBufferBindingInfo.usage                            = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT;
                fn_vkCmdBindDescriptorBuffersEXT(cmdBuf.CommandBuffer, 1, &descriptorBufferBindingInfo);

                uint32_t     bufferIndices           = 0;
                VkDeviceSize descriptorBufferOffsets = 0;
                fn_vkCmdSetDescriptorBufferOffsetsEXT(
                    cmdBuf.CommandBuffer,
                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                    envPipelineLayout.PipelineLayout,
                    0, // firstSet
                    1, // setCount
                    &bufferIndices,
                    &descriptorBufferOffsets);

                // Bind the VS/FS Graphics Pipeline
                vkCmdBindPipeline(cmdBuf.CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, envPipelineState);

                glm::mat4 moveUp = glm::translate(vec3(0, 5, 0));

                // SceneParmas (b0)
                mat4 mvp = projMat * viewMat * moveUp;

                EnvSceneParameters envSceneParams = {};
                envSceneParams.IBLIndex           = gIBLIndex;
                envSceneParams.MVP                = mvp;

                vkCmdPushConstants(cmdBuf.CommandBuffer, envPipelineLayout.PipelineLayout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(EnvSceneParameters), &envSceneParams);

                // Bind the Index Buffer
                vkCmdBindIndexBuffer(cmdBuf.CommandBuffer, envGeoBuffers.indexBuffer.Buffer, 0, VK_INDEX_TYPE_UINT32);

                // Bind the Vertex Buffer
                VkBuffer     vertexBuffers[] = {envGeoBuffers.positionBuffer.Buffer, envGeoBuffers.texCoordBuffer.Buffer};
                VkDeviceSize offsets[]       = {0, 0};
                vkCmdBindVertexBuffers(cmdBuf.CommandBuffer, 0, 2, vertexBuffers, offsets);

                vkCmdDrawIndexed(cmdBuf.CommandBuffer, envGeoBuffers.numIndices, 1, 0, 0, 0);
            }

            // Draw sample spheres
            {
                VkDescriptorBufferBindingInfoEXT descriptorBufferBindingInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT};
                descriptorBufferBindingInfo.pNext                            = nullptr;
                descriptorBufferBindingInfo.address                          = GetDeviceAddress(renderer.get(), &pbrDescriptorBuffer);
                descriptorBufferBindingInfo.usage                            = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT;
                fn_vkCmdBindDescriptorBuffersEXT(cmdBuf.CommandBuffer, 1, &descriptorBufferBindingInfo);

                uint32_t     bufferIndices           = 0;
                VkDeviceSize descriptorBufferOffsets = 0;
                fn_vkCmdSetDescriptorBufferOffsetsEXT(
                    cmdBuf.CommandBuffer,
                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pbrPipelineLayout.PipelineLayout,
                    0, // firstSet
                    1, // setCount
                    &bufferIndices,
                    &descriptorBufferOffsets);

                // Select which model to draw
                const GeometryBuffers& geoBuffers    = matGeoBuffers[gModelIndex];
                uint32_t               geoIndexCount = geoBuffers.numIndices;

                // Bind the Index Buffer
                vkCmdBindIndexBuffer(cmdBuf.CommandBuffer, geoBuffers.indexBuffer.Buffer, 0, VK_INDEX_TYPE_UINT32);

                // Bind the Vertex Buffer
                VkBuffer     vertexBuffers[] = {geoBuffers.positionBuffer.Buffer, geoBuffers.normalBuffer.Buffer};
                VkDeviceSize offsets[]       = {0, 0};
                vkCmdBindVertexBuffers(cmdBuf.CommandBuffer, 0, 2, vertexBuffers, offsets);

                // Pipeline state
                vkCmdBindPipeline(cmdBuf.CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pbrPipelineState);

                const float yPos = 0.0f;

                // Copper
                {
                    glm::mat4 modelMat      = glm::translate(vec3(-3, yPos, 3));
                    uint32_t  materialIndex = 0;

                    DrawParameters drawParams = {};
                    drawParams.ModelMatrix    = modelMat;
                    drawParams.MaterialIndex  = materialIndex;

                    vkCmdPushConstants(cmdBuf.CommandBuffer, pbrPipelineLayout.PipelineLayout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(DrawParameters), &drawParams);

                    vkCmdDrawIndexed(cmdBuf.CommandBuffer, geoIndexCount, 1, 0, 0, 0);
                }

                // Gold
                {
                    glm::mat4 modelMat      = glm::translate(vec3(0, yPos, 3));
                    uint32_t  materialIndex = 1;

                    DrawParameters drawParams = {};
                    drawParams.ModelMatrix    = modelMat;
                    drawParams.MaterialIndex  = materialIndex;

                    vkCmdPushConstants(cmdBuf.CommandBuffer, pbrPipelineLayout.PipelineLayout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(DrawParameters), &drawParams);

                    vkCmdDrawIndexed(cmdBuf.CommandBuffer, geoIndexCount, 1, 0, 0, 0);
                }

                // Silver
                {
                    glm::mat4 modelMat      = glm::translate(vec3(3, yPos, 3));
                    uint32_t  materialIndex = 2;

                    DrawParameters drawParams = {};
                    drawParams.ModelMatrix    = modelMat;
                    drawParams.MaterialIndex  = materialIndex;

                    vkCmdPushConstants(cmdBuf.CommandBuffer, pbrPipelineLayout.PipelineLayout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(DrawParameters), &drawParams);

                    vkCmdDrawIndexed(cmdBuf.CommandBuffer, geoIndexCount, 1, 0, 0, 0);
                }

                // Zink
                {
                    glm::mat4 modelMat      = glm::translate(vec3(-3, yPos, 0));
                    uint32_t  materialIndex = 3;

                    DrawParameters drawParams = {};
                    drawParams.ModelMatrix    = modelMat;
                    drawParams.MaterialIndex  = materialIndex;

                    vkCmdPushConstants(cmdBuf.CommandBuffer, pbrPipelineLayout.PipelineLayout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(DrawParameters), &drawParams);

                    vkCmdDrawIndexed(cmdBuf.CommandBuffer, geoIndexCount, 1, 0, 0, 0);
                }

                // Titanium
                {
                    glm::mat4 modelMat      = glm::translate(vec3(0, yPos, 0));
                    uint32_t  materialIndex = 4;

                    DrawParameters drawParams = {};
                    drawParams.ModelMatrix    = modelMat;
                    drawParams.MaterialIndex  = materialIndex;

                    vkCmdPushConstants(cmdBuf.CommandBuffer, pbrPipelineLayout.PipelineLayout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(DrawParameters), &drawParams);

                    vkCmdDrawIndexed(cmdBuf.CommandBuffer, geoIndexCount, 1, 0, 0, 0);
                }

                // Shiny Plastic
                {
                    glm::mat4 modelMat      = glm::translate(vec3(3, yPos, 0));
                    uint32_t  materialIndex = 5;

                    DrawParameters drawParams = {};
                    drawParams.ModelMatrix    = modelMat;
                    drawParams.MaterialIndex  = materialIndex;

                    vkCmdPushConstants(cmdBuf.CommandBuffer, pbrPipelineLayout.PipelineLayout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(DrawParameters), &drawParams);

                    vkCmdDrawIndexed(cmdBuf.CommandBuffer, geoIndexCount, 1, 0, 0, 0);
                }

                // Rough Plastic
                {
                    glm::mat4 modelMat      = glm::translate(vec3(-3, yPos, -3));
                    uint32_t  materialIndex = 6;

                    DrawParameters drawParams = {};
                    drawParams.ModelMatrix    = modelMat;
                    drawParams.MaterialIndex  = materialIndex;

                    vkCmdPushConstants(cmdBuf.CommandBuffer, pbrPipelineLayout.PipelineLayout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(DrawParameters), &drawParams);

                    vkCmdDrawIndexed(cmdBuf.CommandBuffer, geoIndexCount, 1, 0, 0, 0);
                }

                // Rougher Plastic
                {
                    glm::mat4 modelMat      = glm::translate(vec3(0, yPos, -3));
                    uint32_t  materialIndex = 7;

                    DrawParameters drawParams = {};
                    drawParams.ModelMatrix    = modelMat;
                    drawParams.MaterialIndex  = materialIndex;

                    vkCmdPushConstants(cmdBuf.CommandBuffer, pbrPipelineLayout.PipelineLayout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(DrawParameters), &drawParams);

                    vkCmdDrawIndexed(cmdBuf.CommandBuffer, geoIndexCount, 1, 0, 0, 0);
                }

                // Roughest Plastic
                {
                    glm::mat4 modelMat      = glm::translate(vec3(3, yPos, -3));
                    uint32_t  materialIndex = 8;

                    DrawParameters drawParams = {};
                    drawParams.ModelMatrix    = modelMat;
                    drawParams.MaterialIndex  = materialIndex;

                    vkCmdPushConstants(cmdBuf.CommandBuffer, pbrPipelineLayout.PipelineLayout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(DrawParameters), &drawParams);

                    vkCmdDrawIndexed(cmdBuf.CommandBuffer, geoIndexCount, 1, 0, 0, 0);
                }
            }

            vkCmdEndRendering(cmdBuf.CommandBuffer);

            // Setup render passes and draw ImGui
            {
                VkRenderPassAttachmentBeginInfo attachmentBeginInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO};
                attachmentBeginInfo.pNext                           = 0;
                attachmentBeginInfo.attachmentCount                 = 1;
                attachmentBeginInfo.pAttachments                    = &imageViews[bufferIndex];

                VkRenderPassBeginInfo beginInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
                beginInfo.pNext                 = &attachmentBeginInfo;
                beginInfo.renderPass            = renderPass.RenderPass;
                beginInfo.framebuffer           = renderPass.Framebuffer;
                beginInfo.renderArea            = scissor;

                vkCmdBeginRenderPass(cmdBuf.CommandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

                // Draw ImGui
                window->ImGuiRenderDrawData(renderer.get(), cmdBuf.CommandBuffer);

                vkCmdEndRenderPass(cmdBuf.CommandBuffer);
            }

            CmdTransitionImageLayout(
                cmdBuf.CommandBuffer,
                images[bufferIndex],
                GREX_ALL_SUBRESOURCES,
                VK_IMAGE_ASPECT_COLOR_BIT,
                RESOURCE_STATE_RENDER_TARGET,
                RESOURCE_STATE_PRESENT);
        }

        CHECK_CALL(vkEndCommandBuffer(cmdBuf.CommandBuffer));

        // Execute command buffer
        CHECK_CALL(ExecuteCommandBuffer(renderer.get(), &cmdBuf));

        // Wait for the GPU to finish the work
        if (!WaitForGpu(renderer.get()))
        {
            assert(false && "WaitForGpu failed");
            break;
        }

        // Present
        if (!SwapchainPresent(renderer.get(), bufferIndex))
        {
            assert(false && "SwapchainPresent failed");
            break;
        }
    }

    return 0;
}

void CreatePBRPipeline(VulkanRenderer* pRenderer, VulkanPipelineLayout* pLayout)
{
    // Descriptor set layout
    {
        std::vector<VkDescriptorSetLayoutBinding> bindings = {};
        // ConstantBuffer<SceneParameters>      SceneParams        : register(b0);
        {
            VkDescriptorSetLayoutBinding binding = {};
            binding.binding                      = 0;
            binding.descriptorType               = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            binding.descriptorCount              = 1;
            binding.stageFlags                   = VK_SHADER_STAGE_ALL_GRAPHICS;
            bindings.push_back(binding);
        }
        // StructuredBuffer<MaterialParameters> MaterialParams     : register(t2);
        {
            VkDescriptorSetLayoutBinding binding = {};
            binding.binding                      = 2;
            binding.descriptorType               = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            binding.descriptorCount              = 1;
            binding.stageFlags                   = VK_SHADER_STAGE_ALL_GRAPHICS;
            bindings.push_back(binding);
        }
        // SamplerState                         ClampedSampler     : register(s4);
        {
            VkDescriptorSetLayoutBinding binding = {};
            binding.binding                      = 4;
            binding.descriptorType               = VK_DESCRIPTOR_TYPE_SAMPLER;
            binding.descriptorCount              = 1;
            binding.stageFlags                   = VK_SHADER_STAGE_ALL_GRAPHICS;
            bindings.push_back(binding);
        }
        // SamplerState                         UWrapSampler       : register(s5);
        {
            VkDescriptorSetLayoutBinding binding = {};
            binding.binding                      = 5;
            binding.descriptorType               = VK_DESCRIPTOR_TYPE_SAMPLER;
            binding.descriptorCount              = 1;
            binding.stageFlags                   = VK_SHADER_STAGE_ALL_GRAPHICS;
            bindings.push_back(binding);
        }
        // Texture2D                            BRDFLUT            : register(t10);
        {
            VkDescriptorSetLayoutBinding binding = {};
            binding.binding                      = 10;
            binding.descriptorType               = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            binding.descriptorCount              = 1;
            binding.stageFlags                   = VK_SHADER_STAGE_ALL_GRAPHICS;
            bindings.push_back(binding);
        }
        // Texture2D                            IrradianceMap[32]  : register(t16);
        {
            VkDescriptorSetLayoutBinding binding = {};
            binding.binding                      = 16;
            binding.descriptorType               = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            binding.descriptorCount              = 32;
            binding.stageFlags                   = VK_SHADER_STAGE_ALL_GRAPHICS;
            bindings.push_back(binding);
        }
        // Texture2D                            EnvironmentMap[32] : register(t48);
        {
            VkDescriptorSetLayoutBinding binding = {};
            binding.binding                      = 48;
            binding.descriptorType               = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            binding.descriptorCount              = 32;
            binding.stageFlags                   = VK_SHADER_STAGE_ALL_GRAPHICS;
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
            &pLayout->DescriptorSetLayout));
    }

    std::vector<VkPushConstantRange> push_constants;
    {
        VkPushConstantRange push_constant = {};
        push_constant.offset              = 0;
        push_constant.size                = sizeof(DrawParameters);
        push_constant.stageFlags          = VK_SHADER_STAGE_ALL_GRAPHICS;
        push_constants.push_back(push_constant);
    }

    VkPipelineLayoutCreateInfo createInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    createInfo.setLayoutCount             = 1;
    createInfo.pSetLayouts                = &pLayout->DescriptorSetLayout;
    createInfo.pushConstantRangeCount     = CountU32(push_constants);
    createInfo.pPushConstantRanges        = DataPtr(push_constants);

    CHECK_CALL(vkCreatePipelineLayout(pRenderer->Device, &createInfo, nullptr, &pLayout->PipelineLayout));
}

void CreateEnvironmentPipeline(VulkanRenderer* pRenderer, VulkanPipelineLayout* pLayout)
{
    // Descriptor set layout
    {
        std::vector<VkDescriptorSetLayoutBinding> bindings = {};
        {
            VkDescriptorSetLayoutBinding binding = {};
            binding.binding                      = 1;
            binding.descriptorType               = VK_DESCRIPTOR_TYPE_SAMPLER;
            binding.descriptorCount              = 1;
            binding.stageFlags                   = VK_SHADER_STAGE_ALL_GRAPHICS;
            bindings.push_back(binding);
        }
        {
            VkDescriptorSetLayoutBinding binding = {};
            binding.binding                      = 32;
            binding.descriptorType               = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            binding.descriptorCount              = 16;
            binding.stageFlags                   = VK_SHADER_STAGE_ALL_GRAPHICS;
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
            &pLayout->DescriptorSetLayout));
    }

    VkPushConstantRange push_constant = {};
    push_constant.offset              = 0;
    push_constant.size                = sizeof(EnvSceneParameters);
    push_constant.stageFlags          = VK_SHADER_STAGE_ALL_GRAPHICS;

    VkPipelineLayoutCreateInfo createInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    createInfo.setLayoutCount             = 1;
    createInfo.pSetLayouts                = &pLayout->DescriptorSetLayout;
    createInfo.pushConstantRangeCount     = 1;
    createInfo.pPushConstantRanges        = &push_constant;

    CHECK_CALL(vkCreatePipelineLayout(pRenderer->Device, &createInfo, nullptr, &pLayout->PipelineLayout));
}

void CreateEnvironmentVertexBuffers(
    VulkanRenderer*  pRenderer,
    GeometryBuffers& outGeomtryBuffers)
{
    TriMesh mesh = TriMesh::Sphere(25, 64, 64, {.enableTexCoords = true, .faceInside = true});

    outGeomtryBuffers.numIndices = 3 * mesh.GetNumTriangles();

    CHECK_CALL(CreateBuffer(
        pRenderer,
        SizeInBytes(mesh.GetTriangles()),
        DataPtr(mesh.GetTriangles()),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY,
        0,
        &outGeomtryBuffers.indexBuffer));

    CHECK_CALL(CreateBuffer(
        pRenderer,
        SizeInBytes(mesh.GetPositions()),
        DataPtr(mesh.GetPositions()),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY,
        0,
        &outGeomtryBuffers.positionBuffer));

    CHECK_CALL(CreateBuffer(
        pRenderer,
        SizeInBytes(mesh.GetTexCoords()),
        DataPtr(mesh.GetTexCoords()),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY,
        0,
        &outGeomtryBuffers.texCoordBuffer));
}

void CreateMaterialModels(
    VulkanRenderer*               pRenderer,
    std::vector<GeometryBuffers>& outGeomtryBuffers)
{
    // Sphere
    {
        TriMesh mesh = TriMesh::Sphere(1, 256, 256, {.enableNormals = true});

        GeometryBuffers buffers = {};

        buffers.numIndices = 3 * mesh.GetNumTriangles();

        CHECK_CALL(CreateBuffer(
            pRenderer,
            SizeInBytes(mesh.GetTriangles()),
            DataPtr(mesh.GetTriangles()),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VMA_MEMORY_USAGE_GPU_ONLY,
            0,
            &buffers.indexBuffer));

        CHECK_CALL(CreateBuffer(
            pRenderer,
            SizeInBytes(mesh.GetPositions()),
            DataPtr(mesh.GetPositions()),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VMA_MEMORY_USAGE_GPU_ONLY,
            0,
            &buffers.positionBuffer));

        CHECK_CALL(CreateBuffer(
            pRenderer,
            SizeInBytes(mesh.GetNormals()),
            DataPtr(mesh.GetNormals()),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VMA_MEMORY_USAGE_GPU_ONLY,
            0,
            &buffers.normalBuffer));

        outGeomtryBuffers.push_back(buffers);
    }

    // Knob
    {
        TriMesh::Options options = {};
        options.enableNormals    = true;
        options.applyTransform   = true;
        options.transformRotate  = glm::vec3(0, glm::radians(180.0f), 0);

        TriMesh mesh;
        if (!TriMesh::LoadOBJ(GetAssetPath("models/material_knob.obj").string(), "", options, &mesh))
        {
            return;
        }
        mesh.ScaleToFit(1.0f);

        GeometryBuffers buffers = {};

        buffers.numIndices = 3 * mesh.GetNumTriangles();

        CHECK_CALL(CreateBuffer(
            pRenderer,
            SizeInBytes(mesh.GetTriangles()),
            DataPtr(mesh.GetTriangles()),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VMA_MEMORY_USAGE_GPU_ONLY,
            0,
            &buffers.indexBuffer));

        CHECK_CALL(CreateBuffer(
            pRenderer,
            SizeInBytes(mesh.GetPositions()),
            DataPtr(mesh.GetPositions()),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VMA_MEMORY_USAGE_GPU_ONLY,
            0,
            &buffers.positionBuffer));

        CHECK_CALL(CreateBuffer(
            pRenderer,
            SizeInBytes(mesh.GetNormals()),
            DataPtr(mesh.GetNormals()),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VMA_MEMORY_USAGE_GPU_ONLY,
            0,
            &buffers.normalBuffer));

        outGeomtryBuffers.push_back(buffers);
    }

    // Monkey
    {
        TriMesh::Options options = {};
        options.enableNormals    = true;
        options.applyTransform   = true;
        options.transformRotate  = glm::vec3(0, glm::radians(180.0f), 0);

        TriMesh mesh;
        if (!TriMesh::LoadOBJ(GetAssetPath("models/monkey.obj").string(), "", options, &mesh))
        {
            return;
        }
        // mesh.ScaleToUnit();

        GeometryBuffers buffers = {};

        buffers.numIndices = 3 * mesh.GetNumTriangles();

        CHECK_CALL(CreateBuffer(
            pRenderer,
            SizeInBytes(mesh.GetTriangles()),
            DataPtr(mesh.GetTriangles()),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VMA_MEMORY_USAGE_GPU_ONLY,
            0,
            &buffers.indexBuffer));

        CHECK_CALL(CreateBuffer(
            pRenderer,
            SizeInBytes(mesh.GetPositions()),
            DataPtr(mesh.GetPositions()),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VMA_MEMORY_USAGE_GPU_ONLY,
            0,
            &buffers.positionBuffer));

        CHECK_CALL(CreateBuffer(
            pRenderer,
            SizeInBytes(mesh.GetNormals()),
            DataPtr(mesh.GetNormals()),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VMA_MEMORY_USAGE_GPU_ONLY,
            0,
            &buffers.normalBuffer));

        outGeomtryBuffers.push_back(buffers);
    }

    // Teapot
    {
        TriMesh::Options options = {};
        options.enableNormals    = true;
        options.applyTransform   = true;
        options.transformRotate  = glm::vec3(0, glm::radians(135.0f), 0);

        TriMesh mesh;
        if (!TriMesh::LoadOBJ(GetAssetPath("models/teapot.obj").string(), "", options, &mesh))
        {
            return;
        }
        mesh.ScaleToFit(2.0f);

        GeometryBuffers buffers = {};

        buffers.numIndices = 3 * mesh.GetNumTriangles();

        CHECK_CALL(CreateBuffer(
            pRenderer,
            SizeInBytes(mesh.GetTriangles()),
            DataPtr(mesh.GetTriangles()),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VMA_MEMORY_USAGE_GPU_ONLY,
            0,
            &buffers.indexBuffer));

        CHECK_CALL(CreateBuffer(
            pRenderer,
            SizeInBytes(mesh.GetPositions()),
            DataPtr(mesh.GetPositions()),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VMA_MEMORY_USAGE_GPU_ONLY,
            0,
            &buffers.positionBuffer));

        CHECK_CALL(CreateBuffer(
            pRenderer,
            SizeInBytes(mesh.GetNormals()),
            DataPtr(mesh.GetNormals()),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VMA_MEMORY_USAGE_GPU_ONLY,
            0,
            &buffers.normalBuffer));

        outGeomtryBuffers.push_back(buffers);
    }
}

void CreateIBLTextures(
    VulkanRenderer*           pRenderer,
    VulkanImage*              pBRDFLUT,
    std::vector<VulkanImage>& outIrradianceTextures,
    std::vector<VulkanImage>& outEnvironmentTextures,
    std::vector<uint32_t>&    outEnvNumLevels)
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

    auto                               iblDir = GetAssetPath("IBL");
    std::vector<std::filesystem::path> iblFiles;
    for (auto& entry : std::filesystem::directory_iterator(iblDir))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }
        auto path = entry.path();
        auto ext  = path.extension();
        if (ext == ".ibl")
        {
            path = std::filesystem::relative(path, iblDir.parent_path());
            iblFiles.push_back(path);
        }
    }

    size_t maxEntries = std::min<size_t>(gMaxIBLs, iblFiles.size());
    for (size_t i = 0; i < maxEntries; ++i)
    {
        auto& iblFile = iblFiles[i];

        IBLMaps ibl = {};
        if (!LoadIBLMaps32f(iblFile, &ibl))
        {
            GREX_LOG_ERROR("failed to load: " << iblFile);
            return;
        }

        outEnvNumLevels.push_back(ibl.numLevels);

        // Irradiance
        {
            VulkanImage texture;
            CHECK_CALL(CreateTexture(
                pRenderer,
                ibl.irradianceMap.GetWidth(),
                ibl.irradianceMap.GetHeight(),
                VK_FORMAT_R32G32B32A32_SFLOAT,
                ibl.irradianceMap.GetSizeInBytes(),
                ibl.irradianceMap.GetPixels(),
                &texture));
            outIrradianceTextures.push_back(texture);
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

            VulkanImage texture;
            CHECK_CALL(CreateTexture(
                pRenderer,
                ibl.baseWidth,
                ibl.baseHeight,
                VK_FORMAT_R32G32B32A32_SFLOAT,
                mipOffsets,
                ibl.environmentMap.GetSizeInBytes(),
                ibl.environmentMap.GetPixels(),
                &texture));
            outEnvironmentTextures.push_back(texture);
        }

        gIBLNames.push_back(iblFile.filename().replace_extension().string());

        GREX_LOG_INFO("Loaded " << iblFile);
    }
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

void WritePBRDescriptors(
    VulkanRenderer*           pRenderer,
    VkDescriptorSetLayout     descriptorSetLayout,
    VulkanBuffer*             pDescriptorBuffer,
    const VulkanBuffer*       pSceneParamsBuffer,
    const VulkanBuffer*       pMaterialsBuffer,
    const VulkanImage*        pBRDFLUT,
    std::vector<VulkanImage>& irradianceTextures,
    std::vector<VulkanImage>& envTextures)
{
    char* pDescriptorBufferStartAddress = nullptr;
    CHECK_CALL(vmaMapMemory(
        pRenderer->Allocator,
        pDescriptorBuffer->Allocation,
        reinterpret_cast<void**>(&pDescriptorBufferStartAddress)));

    // ConstantBuffer<SceneParameters>    SceneParams           : register(b0);
    WriteDescriptor(
        pRenderer,
        pDescriptorBufferStartAddress,
        descriptorSetLayout,
        0, // binding
        0, // arrayElement
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        pSceneParamsBuffer);

    // Set via push constants
    // ConstantBuffer<DrawParameters>     DrawParams            : register(b1);

    // ConstantBuffer<MaterialParameters> MaterialParams        : register(b2);
    WriteDescriptor(
        pRenderer,
        pDescriptorBufferStartAddress,
        descriptorSetLayout,
        2, // binding
        0, // arrayElement
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        pMaterialsBuffer);

    // Samplers are setup in the immutable samplers in the DescriptorSetLayout
    // SamplerState                       IBLIntegrationSampler : register(s4);
    {
        VkSamplerCreateInfo samplerInfo     = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.flags                   = 0;
        samplerInfo.magFilter               = VK_FILTER_LINEAR;
        samplerInfo.minFilter               = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.mipLodBias              = 0;
        samplerInfo.anisotropyEnable        = VK_FALSE;
        samplerInfo.maxAnisotropy           = 0;
        samplerInfo.compareEnable           = VK_TRUE;
        samplerInfo.compareOp               = VK_COMPARE_OP_LESS_OR_EQUAL;
        samplerInfo.minLod                  = 0;
        samplerInfo.maxLod                  = 1;
        samplerInfo.borderColor             = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;

        VkSampler clampedSampler = VK_NULL_HANDLE;
        CHECK_CALL(vkCreateSampler(
            pRenderer->Device,
            &samplerInfo,
            nullptr,
            &clampedSampler));

        WriteDescriptor(
            pRenderer,
            pDescriptorBufferStartAddress,
            descriptorSetLayout,
            4, // binding
            0, // arrayElement
            clampedSampler);
    }

    // SamplerState                         UWrapSampler       : register(s5);
    {
        VkSamplerCreateInfo samplerInfo     = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.flags                   = 0;
        samplerInfo.magFilter               = VK_FILTER_LINEAR;
        samplerInfo.minFilter               = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU            = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.mipLodBias              = 0;
        samplerInfo.anisotropyEnable        = VK_FALSE;
        samplerInfo.maxAnisotropy           = 0;
        samplerInfo.compareEnable           = VK_TRUE;
        samplerInfo.compareOp               = VK_COMPARE_OP_LESS_OR_EQUAL;
        samplerInfo.minLod                  = 0;
        samplerInfo.maxLod                  = FLT_MAX;
        samplerInfo.borderColor             = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;

        VkSampler uWrapSampler = VK_NULL_HANDLE;
        CHECK_CALL(vkCreateSampler(
            pRenderer->Device,
            &samplerInfo,
            nullptr,
            &uWrapSampler));

        WriteDescriptor(
            pRenderer,
            pDescriptorBufferStartAddress,
            descriptorSetLayout,
            5, // binding
            0, // arrayElement
            uWrapSampler);
    }

    // Texture2D                            BRDFLUT            : register(t10);
    {
        VkImageView imageView = VK_NULL_HANDLE;
        CHECK_CALL(CreateImageView(
            pRenderer,
            pBRDFLUT,
            VK_IMAGE_VIEW_TYPE_2D,
            VK_FORMAT_R32G32B32A32_SFLOAT,
            GREX_ALL_SUBRESOURCES,
            &imageView));

        WriteDescriptor(
            pRenderer,
            pDescriptorBufferStartAddress,
            descriptorSetLayout,
            10, // binding
            0,  // arrayElement
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            imageView,
            VK_IMAGE_LAYOUT_GENERAL);
    }

    // Texture2D                            IrradianceMap[32]  : register(t16);
    {
        uint32_t arrayElement = 0;
        for (auto& irradianceTexture : irradianceTextures)
        {
            VkImageView imageView = VK_NULL_HANDLE;
            CHECK_CALL(CreateImageView(
                pRenderer,
                &irradianceTexture,
                VK_IMAGE_VIEW_TYPE_2D,
                VK_FORMAT_R32G32B32A32_SFLOAT,
                GREX_ALL_SUBRESOURCES,
                &imageView));

            WriteDescriptor(
                pRenderer,
                pDescriptorBufferStartAddress,
                descriptorSetLayout,
                16,           // binding
                arrayElement, // arrayElement
                VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                imageView,
                VK_IMAGE_LAYOUT_GENERAL);

            arrayElement++;
        }
    }

    // Texture2D                            EnvironmentMap[32] : register(t48);
    {
        uint32_t arrayElement = 0;
        for (auto& envTexture : envTextures)
        {
            VkImageView imageView = VK_NULL_HANDLE;
            CHECK_CALL(CreateImageView(
                pRenderer,
                &envTexture,
                VK_IMAGE_VIEW_TYPE_2D,
                VK_FORMAT_R32G32B32A32_SFLOAT,
                GREX_ALL_SUBRESOURCES,
                &imageView));

            WriteDescriptor(
                pRenderer,
                pDescriptorBufferStartAddress,
                descriptorSetLayout,
                48,           // binding
                arrayElement, // arrayElement
                VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                imageView,
                VK_IMAGE_LAYOUT_GENERAL);

            arrayElement++;
        }
    }

    vmaUnmapMemory(pRenderer->Allocator, pDescriptorBuffer->Allocation);
}

void WriteEnvDescriptors(
    VulkanRenderer*           pRenderer,
    VkDescriptorSetLayout     descriptorSetLayout,
    VulkanBuffer*             pDescriptorBuffer,
    std::vector<VulkanImage>& envTextures)
{
    char* pDescriptorBufferStartAddress = nullptr;
    CHECK_CALL(vmaMapMemory(
        pRenderer->Allocator,
        pDescriptorBuffer->Allocation,
        reinterpret_cast<void**>(&pDescriptorBufferStartAddress)));

    // set via push constants
    // ConstantBuffer<SceneParameters> SceneParams       : register(b0);

    // SamplerState                    IBLMapSampler     : register(s1);
    {
        VkSamplerCreateInfo samplerInfo     = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.flags                   = 0;
        samplerInfo.magFilter               = VK_FILTER_LINEAR;
        samplerInfo.minFilter               = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU            = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.mipLodBias              = 0;
        samplerInfo.anisotropyEnable        = VK_FALSE;
        samplerInfo.maxAnisotropy           = 0;
        samplerInfo.compareEnable           = VK_TRUE;
        samplerInfo.compareOp               = VK_COMPARE_OP_NEVER;
        samplerInfo.minLod                  = 0;
        samplerInfo.maxLod                  = FLT_MAX;
        samplerInfo.borderColor             = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;

        VkSampler uWrapSampler = VK_NULL_HANDLE;
        CHECK_CALL(vkCreateSampler(
            pRenderer->Device,
            &samplerInfo,
            nullptr,
            &uWrapSampler));

        WriteDescriptor(
            pRenderer,
            pDescriptorBufferStartAddress,
            descriptorSetLayout,
            1, // binding
            0, // arrayElement
            uWrapSampler);
    }

    // Texture2D                       IBLEnvironmentMap : register(t2);
    {
        uint32_t arrayElement = 0;
        for (auto& envTexture : envTextures)
        {
            VkImageView imageView = VK_NULL_HANDLE;
            CHECK_CALL(CreateImageView(
                pRenderer,
                &envTexture,
                VK_IMAGE_VIEW_TYPE_2D,
                VK_FORMAT_R32G32B32A32_SFLOAT,
                GREX_ALL_SUBRESOURCES,
                &imageView));

            WriteDescriptor(
                pRenderer,
                pDescriptorBufferStartAddress,
                descriptorSetLayout,
                32,           // binding
                arrayElement, // arrayElement
                VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                imageView,
                VK_IMAGE_LAYOUT_GENERAL);

            arrayElement++;
        }
    }

    vmaUnmapMemory(pRenderer->Allocator, pDescriptorBuffer->Allocation);
}

/*
void CreatePBRRootSig(DxRenderer* pRenderer, ID3D12RootSignature** ppRootSig)
{
    // BRDFLUT (t10)
    D3D12_DESCRIPTOR_RANGE range1            = {};
    range1.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range1.NumDescriptors                    = 1;
    range1.BaseShaderRegister                = 10;
    range1.RegisterSpace                     = 0;
    range1.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // IrradianceMap (t16)
    D3D12_DESCRIPTOR_RANGE range2            = {};
    range2.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range2.NumDescriptors                    = gMaxIBLs;
    range2.BaseShaderRegister                = 16;
    range2.RegisterSpace                     = 0;
    range2.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // EnvironmentMap (t32)
    D3D12_DESCRIPTOR_RANGE range3            = {};
    range3.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range3.NumDescriptors                    = gMaxIBLs;
    range3.BaseShaderRegister                = 48;
    range3.RegisterSpace                     = 0;
    range3.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParameters[6] = {};
    // SceneParams (b0)
    rootParameters[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[0].Descriptor.ShaderRegister = 0;
    rootParameters[0].Descriptor.RegisterSpace  = 0;
    rootParameters[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    // DrawParams (b1)
    rootParameters[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParameters[1].Constants.Num32BitValues = 36;
    rootParameters[1].Constants.ShaderRegister = 1;
    rootParameters[1].Constants.RegisterSpace  = 0;
    rootParameters[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;
    // MaterialParams (t2)
    rootParameters[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParameters[2].Descriptor.ShaderRegister = 2;
    rootParameters[2].Descriptor.RegisterSpace  = 0;
    rootParameters[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    // BRDFLUT (t10)
    rootParameters[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[3].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[3].DescriptorTable.pDescriptorRanges   = &range1;
    rootParameters[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
    // IrradianceMap (t16)
    rootParameters[4].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[4].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[4].DescriptorTable.pDescriptorRanges   = &range2;
    rootParameters[4].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
    // EnvironmentMap (t32)
    rootParameters[5].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[5].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[5].DescriptorTable.pDescriptorRanges   = &range3;
    rootParameters[5].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC staticSamplers[2] = {};
    // ClampedSampler (s4)
    staticSamplers[0].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSamplers[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[0].MipLODBias       = D3D12_DEFAULT_MIP_LOD_BIAS;
    staticSamplers[0].MaxAnisotropy    = 0;
    staticSamplers[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    staticSamplers[0].MinLOD           = 0;
    staticSamplers[0].MaxLOD           = 1;
    staticSamplers[0].ShaderRegister   = 4;
    staticSamplers[0].RegisterSpace    = 0;
    staticSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // UWrapSampler (s5)
    staticSamplers[1].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSamplers[1].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[1].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[1].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[1].MipLODBias       = D3D12_DEFAULT_MIP_LOD_BIAS;
    staticSamplers[1].MaxAnisotropy    = 0;
    staticSamplers[1].ComparisonFunc   = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    staticSamplers[1].MinLOD           = 0;
    staticSamplers[1].MaxLOD           = D3D12_FLOAT32_MAX;
    staticSamplers[1].ShaderRegister   = 5;
    staticSamplers[1].RegisterSpace    = 0;
    staticSamplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters             = 6;
    rootSigDesc.pParameters               = rootParameters;
    rootSigDesc.NumStaticSamplers         = 2;
    rootSigDesc.pStaticSamplers           = staticSamplers;
    rootSigDesc.Flags                     = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;
    CHECK_CALL(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error));
    CHECK_CALL(pRenderer->Device->CreateRootSignature(
        0,                         // nodeMask
        blob->GetBufferPointer(),  // pBloblWithRootSignature
        blob->GetBufferSize(),     // blobLengthInBytes
        IID_PPV_ARGS(ppRootSig))); // riid, ppvRootSignature
}

void CreateEnvironmentRootSig(DxRenderer* pRenderer, ID3D12RootSignature** ppRootSig)
{
    // Textures (t32)
    D3D12_DESCRIPTOR_RANGE range            = {};
    range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors                    = gMaxIBLs;
    range.BaseShaderRegister                = 32;
    range.RegisterSpace                     = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParameters[4] = {};
    // SceneParams (b0)
    rootParameters[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParameters[0].Constants.Num32BitValues = 17;
    rootParameters[0].Constants.ShaderRegister = 0;
    rootParameters[0].Constants.RegisterSpace  = 0;
    rootParameters[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;
    // Textures (t32)
    rootParameters[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[1].DescriptorTable.pDescriptorRanges   = &range;
    rootParameters[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC staticSampler = {};
    // Sampler0 (s1)
    staticSampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSampler.MipLODBias       = D3D12_DEFAULT_MIP_LOD_BIAS;
    staticSampler.MaxAnisotropy    = 0;
    staticSampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    staticSampler.MinLOD           = 0;
    staticSampler.MaxLOD           = 1;
    staticSampler.ShaderRegister   = 1;
    staticSampler.RegisterSpace    = 0;
    staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters             = 2;
    rootSigDesc.pParameters               = rootParameters;
    rootSigDesc.NumStaticSamplers         = 1;
    rootSigDesc.pStaticSamplers           = &staticSampler;
    rootSigDesc.Flags                     = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;
    CHECK_CALL(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error));
    CHECK_CALL(pRenderer->Device->CreateRootSignature(
        0,                         // nodeMask
        blob->GetBufferPointer(),  // pBloblWithRootSignature
        blob->GetBufferSize(),     // blobLengthInBytes
        IID_PPV_ARGS(ppRootSig))); // riid, ppvRootSignature
}

void CreateDescriptorHeap(
    DxRenderer*            pRenderer,
    ID3D12DescriptorHeap** ppHeap)
{
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors             = 256;
    desc.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    CHECK_CALL(pRenderer->Device->CreateDescriptorHeap(
        &desc,
        IID_PPV_ARGS(ppHeap)));
}
*/