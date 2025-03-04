
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include "mtl_renderer.h"
#include "mtl_renderer_utils.h"

bool     IsCompressed(MTL::PixelFormat fmt);
uint32_t BitsPerPixel(MTL::PixelFormat fmt);
uint32_t BitsPerElement(MTL::VertexFormat fmt);

uint32_t PixelStride(MTL::PixelFormat fmt)
{
    uint32_t nbytes = BitsPerPixel(fmt) / 8;
    return nbytes;
}

// =================================================================================================
// MetalRenderer
// =================================================================================================

MetalRenderer::MetalRenderer()
{
}

MetalRenderer::~MetalRenderer()
{
    SwapchainBufferCount = 0;
}

bool InitMetal(
    MetalRenderer* pRenderer,
    bool           enableDebug)
{
    if (IsNull(pRenderer))
    {
        return false;
    }

    pRenderer->DebugEnabled = enableDebug;

    pRenderer->Device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    if (pRenderer->Device.get() == nullptr)
    {
        assert(false && "MTL::CreateSystemDefaultDevice() failed");
        return false;
    }

    pRenderer->Queue = NS::TransferPtr(pRenderer->Device->newCommandQueue());
    if (pRenderer->Queue.get() == nullptr)
    {
        assert(false && "MTL::Device::newCommandQueue() failed");
        return false;
    }

    return true;
}

bool InitSwapchain(
    MetalRenderer*   pRenderer,
    void*            pCocoaWindow,
    uint32_t         width,
    uint32_t         height,
    uint32_t         bufferCount,
    MTL::PixelFormat dsvFormat)
{
    CGSize layerSize = {(float)width, (float)height};

    // Don't need to Retain/Release the swapchain as the function doesn't match the
    // naming template given in the metal-cpp README.md
    CA::MetalLayer* layer = CA::MetalLayer::layer();

    if (layer != nullptr)
    {
        layer->setDevice(pRenderer->Device.get());
        layer->setPixelFormat(GREX_DEFAULT_RTV_FORMAT);
        layer->setDrawableSize(layerSize);

        MetalSetNSWindowSwapchain(pCocoaWindow, layer);

        pRenderer->pSwapchain           = layer;
        pRenderer->SwapchainBufferCount = bufferCount;

        if (dsvFormat != MTL::PixelFormatInvalid)
        {
            for (int dsvIndex = 0; (dsvIndex < bufferCount); dsvIndex++)
            {
                auto depthBufferDesc = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());

                depthBufferDesc->setPixelFormat(dsvFormat);
                depthBufferDesc->setWidth(width);
                depthBufferDesc->setHeight(height);
                depthBufferDesc->setMipmapLevelCount(1);
                depthBufferDesc->setResourceOptions(MTL::ResourceStorageModePrivate);
                depthBufferDesc->setUsage(MTL::TextureUsageRenderTarget);

                auto localTexture = NS::TransferPtr(pRenderer->Device->newTexture(depthBufferDesc.get()));

                if (localTexture.get() != nullptr)
                {
                    pRenderer->SwapchainDSVBuffers.push_back(localTexture);
                }
                else
                {
                    assert(false && "Depth Buffer MTL::Device::newTexture() failed");
                    return false;
                }
            }
        }
    }
    else
    {
        assert(false && "pSwapchain creation CA::MetalLayer::layer() failed");
        return false;
    }

    return true;
}

MTL::PixelFormat ToMTLFormat(
    GREXFormat format)
{
    switch (static_cast<int>(format))
    {
        case GREX_FORMAT_R8_UNORM:
            return MTL::PixelFormatR8Unorm;

        case GREX_FORMAT_R8G8_UNORM:
            return MTL::PixelFormatRG8Unorm;

        case GREX_FORMAT_R8G8B8A8_UNORM:
            return MTL::PixelFormatRGBA8Unorm;

        case GREX_FORMAT_R8_UINT:
            return MTL::PixelFormatR8Uint;

        case GREX_FORMAT_R16_UINT:
            return MTL::PixelFormatR16Uint;

        case GREX_FORMAT_R16G16_UINT:
            return MTL::PixelFormatRG16Uint;

        case GREX_FORMAT_R16G16B16A16_UINT:
            return MTL::PixelFormatRGBA16Uint;

        case GREX_FORMAT_R32_UINT:
            return MTL::PixelFormatR32Uint;

        case GREX_FORMAT_R32_FLOAT:
            return MTL::PixelFormatR32Float;

        case GREX_FORMAT_R32G32_FLOAT:
            return MTL::PixelFormatRG32Float;

        case GREX_FORMAT_R32G32B32A32_FLOAT:
            return MTL::PixelFormatRGBA32Float;

        case GREX_FORMAT_BC1_RGB:
            return MTL::PixelFormatBC1_RGBA;

        case GREX_FORMAT_BC3_RGBA:
            return MTL::PixelFormatBC3_RGBA;

        case GREX_FORMAT_BC4_R:
            return MTL::PixelFormatBC4_RUnorm;

        case GREX_FORMAT_BC5_RG:
            return MTL::PixelFormatBC5_RGUnorm;

        case GREX_FORMAT_BC6H_SFLOAT:
            return MTL::PixelFormatBC6H_RGBFloat;

        case GREX_FORMAT_BC6H_UFLOAT:
            return MTL::PixelFormatBC6H_RGBUfloat;

        case GREX_FORMAT_BC7_RGBA:
            return MTL::PixelFormatBC7_RGBAUnorm;

        case GREX_FORMAT_R32G32B32_FLOAT: // Undefined in MTL::PixelFormat
        default:
            return MTL::PixelFormatInvalid;
    }
}

MTL::IndexType ToMTLIndexType(
    GREXFormat format)
{
    switch (static_cast<int>(format))
    {
        case GREX_FORMAT_R16_UINT:
            return MTL::IndexTypeUInt16;

            // There is no "Invalid" type, so just return INT32 if 16 is not specify
        default:
            return MTL::IndexTypeUInt32;
    }
}

NS::Error* CreateBuffer(
    MetalRenderer*       pRenderer,
    size_t               srcSize,
    const void*          pSrcData,
    MTL::ResourceOptions storageMode,
    MetalBuffer*         pBuffer)
{
    pBuffer->Buffer = NS::TransferPtr(pRenderer->Device->newBuffer(srcSize, storageMode));

    if (pBuffer->Buffer.get() != nullptr) {
       if (pSrcData != nullptr) {
          memcpy(pBuffer->Buffer->contents(), pSrcData, srcSize);
          if (storageMode == MTL::ResourceStorageModeManaged) {
            pBuffer->Buffer->didModifyRange(NS::Range::Make(0, pBuffer->Buffer->length()));
          }
       }
    }
    else {
        assert(false && "CreateBuffer() - MTL::Device::newBuffer() failed");
    }

    return nullptr;
}

NS::Error* CreateBuffer(
    MetalRenderer* pRenderer,
    size_t         srcSize,
    const void*    pSrcData,
    MetalBuffer*   pBuffer)
{
    return CreateBuffer(pRenderer, srcSize, pSrcData, MTL::ResourceStorageModeShared, pBuffer);

/*
    pBuffer->Buffer = NS::TransferPtr(pRenderer->Device->newBuffer(srcSize, MTL::ResourceStorageModeManaged));

    if (pBuffer->Buffer.get() != nullptr)
    {
        if (pSrcData != nullptr)
        {
            memcpy(pBuffer->Buffer->contents(), pSrcData, srcSize);
            pBuffer->Buffer->didModifyRange(NS::Range::Make(0, pBuffer->Buffer->length()));
        }
    }
    else
    {
        assert(false && "CreateBuffer() - MTL::Device::newBuffer() failed");
    }

    return nullptr;
*/
}

NS::Error* CreateBuffer(
    MetalRenderer* pRenderer,
    MetalBuffer*   pSrcBuffer,
    MetalBuffer*   pBuffer)
{
    MTL::Buffer* pSrcMtlBuffer = pSrcBuffer->Buffer.get();
    size_t       bufferSize    = pSrcMtlBuffer->length();

    pBuffer->Buffer = NS::TransferPtr(pRenderer->Device->newBuffer(bufferSize, MTL::ResourceStorageModeManaged));

    if (pBuffer->Buffer.get() != nullptr)
    {
        if (pSrcBuffer != nullptr)
        {
            memcpy(pBuffer->Buffer->contents(), pSrcMtlBuffer->contents(), bufferSize);
            pBuffer->Buffer->didModifyRange(NS::Range::Make(0, bufferSize));
        }
    }
    else
    {
        assert(false && "CreateBuffer() - MTL::Device::newBuffer() failed");
    }

    return nullptr;
}

NS::Error* CreateTexture(
    MetalRenderer*                pRenderer,
    uint32_t                      width,
    uint32_t                      height,
    MTL::PixelFormat              format,
    const std::vector<MipOffset>& mipOffsets,
    uint64_t                      srcSizeBytes,
    const void*                   pSrcData,
    MetalTexture*                 pResource)
{
    auto pTextureDesc = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
    pTextureDesc->setWidth(width);
    pTextureDesc->setHeight(height);
    pTextureDesc->setPixelFormat(format);
    pTextureDesc->setTextureType(MTL::TextureType2D);
    //pTextureDesc->setStorageMode(MTL::StorageModeManaged);
    pTextureDesc->setStorageMode(MTL::StorageModeShared);
    pTextureDesc->setUsage(MTL::ResourceUsageSample | MTL::ResourceUsageRead);
    pTextureDesc->setMipmapLevelCount(CountU32(mipOffsets));

    pResource->Texture = NS::TransferPtr(pRenderer->Device->newTexture(pTextureDesc.get()));

    uint32_t mipIndex  = 0;
    uint32_t mipWidth  = width;
    uint32_t mipHeight = height;
    for (const auto& mipOffset : mipOffsets)
    {
        auto        region  = MTL::Region::Make2D(0, 0, mipWidth, mipHeight);
        const void* mipData = reinterpret_cast<const char*>(pSrcData) + mipOffset.Offset;

        uint32_t mipRowStride = mipOffset.RowStride;
        if (IsCompressed(format))
        {
            mipRowStride = mipWidth * 4;
            mipRowStride = (mipRowStride > 16) ? mipRowStride : 16;
        }

        pResource->Texture->replaceRegion(region, mipIndex, mipData, mipRowStride);

        mipWidth >>= 1;
        mipHeight >>= 1;
        mipIndex++;
    }

    return nullptr;
}

NS::Error* CreateTexture(
    MetalRenderer*   pRenderer,
    uint32_t         width,
    uint32_t         height,
    MTL::PixelFormat format,
    uint64_t         srcSizeBytes,
    const void*      pSrcData,
    MetalTexture*    pResource)
{
    MipOffset mipOffset = {};
    mipOffset.Offset    = 0;
    mipOffset.RowStride = width * PixelStride(format);

    return CreateTexture(
        pRenderer,
        width,
        height,
        format,
        {mipOffset},
        srcSizeBytes,
        pSrcData,
        pResource);
}

NS::Error* CreateRWTexture(
    MetalRenderer*   pRenderer,
    uint32_t         width,
    uint32_t         height,
    MTL::PixelFormat format,
    MetalTexture*    pResource)
{
    auto pTextureDesc = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
    pTextureDesc->setWidth(width);
    pTextureDesc->setHeight(height);
    pTextureDesc->setPixelFormat(format);
    pTextureDesc->setTextureType(MTL::TextureType2D);
    pTextureDesc->setStorageMode(MTL::StorageModePrivate);
    pTextureDesc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
    pTextureDesc->setMipmapLevelCount(1);
    pTextureDesc->setArrayLength(1);

    pResource->Texture = NS::TransferPtr(pRenderer->Device->newTexture(pTextureDesc.get()));

    return nullptr;
}

NS::Error* CreateAccelerationStructure(
    MetalRenderer*                        pRenderer,
    MTL::AccelerationStructureDescriptor* pAccelDesc,
    MetalAS*                              pAccelStructure)
{
    NS::AutoreleasePool* pPoolAllocator = NS::AutoreleasePool::alloc()->init();

    // Query for the sizes needed to store and build the acceleration structure
    MTL::AccelerationStructureSizes accelSizes = pRenderer->Device->accelerationStructureSizes(pAccelDesc);

    // Allocate an acceleration structure large enough for this descriptor. This method
    // doesn't actually build the acceleration strcutre, but rather allocates memory
    MTL::AccelerationStructure* accelerationStructure =
        pRenderer->Device->newAccelerationStructure(accelSizes.accelerationStructureSize);

    // Allocate scratch space Metal uses to build the acceleration structure.
    // Use MTLResourceStorageModePrivate for the best performance because the sample
    // doesn't need access to buffer's contents.
    MTL::Buffer* scratchBuffer = pRenderer->Device->newBuffer(
        accelSizes.buildScratchBufferSize,
        MTL::ResourceStorageModePrivate);

    // Create a command buffer that performs the acceleration structure build
    MTL::CommandBuffer* pCommandBuffer = pRenderer->Queue->commandBuffer();

    // Create an acceleration structure command encoder
    MTL::AccelerationStructureCommandEncoder* pASEncoder = pCommandBuffer->accelerationStructureCommandEncoder();

    // Allocate a buffer for Metal to write the compacted accelerated structure's size into
    MTL::Buffer* compactedSizeBuffer = pRenderer->Device->newBuffer(
        sizeof(uint32_t),
        MTL::ResourceStorageModeShared);

    // Schedule the actual acceleration structure build
    pASEncoder->buildAccelerationStructure(accelerationStructure, pAccelDesc, scratchBuffer, 0);

    // Compute and write the compacted acceleration structure size into the buffer. You
    // must already have a built acceleration structure because Metal determines the compacted
    // size based on the final size of the acceleration structure. Compacting an acclerations
    // structure can potentially reclaim significant amounts of memory because Metal must
    // create the initial structur eusing a conservative approach
    pASEncoder->writeCompactedAccelerationStructureSize(accelerationStructure, compactedSizeBuffer, 0);

    // End encoding, and commit the command buffer so the GPU can start building the
    // acceleration structure
    pASEncoder->endEncoding();
    pCommandBuffer->commit();

    // This code waits for Metal to finish executing the command buffer so it can
    // read back the compacted size
    pCommandBuffer->waitUntilCompleted();

    uint32_t compactedSize = *(uint32_t*)compactedSizeBuffer->contents();

    // Allocate a smaller acceleration structure based on the returned size
    MTL::AccelerationStructure* compactedAS = pRenderer->Device->newAccelerationStructure(compactedSize);

    // Create anotehr command buffer and encoder
    pCommandBuffer = pRenderer->Queue->commandBuffer();
    pASEncoder     = pCommandBuffer->accelerationStructureCommandEncoder();

    // Encode the command to copy and compact the acceleration structure into the
    // smaller acceleartion structure
    pASEncoder->copyAndCompactAccelerationStructure(accelerationStructure, compactedAS);

    // End encoding and commit the command buffer. You don't need to wait for Metal to finish
    // executing this command buffer as long as you synchronize any ray-intersection work
    // to run after this command buffer completes. The sample relies on Metal's default
    // dependency trackign on resourcers to automatically synchronize access to the new
    // compacted acceleration structure
    pASEncoder->endEncoding();
    pCommandBuffer->commit();

    pAccelStructure->AS = NS::TransferPtr(compactedAS);

    return nullptr;
}

NS::Error* CreateDrawVertexColorPipeline(
    MetalRenderer*              pRenderer,
    MetalShader*                vsShaderModule,
    MetalShader*                fsShaderModule,
    MTL::PixelFormat            rtvFormat,
    MTL::PixelFormat            dsvFormat,
    MetalPipelineRenderState*   pPipelineRenderState,
    MetalDepthStencilState*     pDepthStencilState,
    MTL::PrimitiveTopologyClass topologyType,
    uint32_t                    pipelineFlags)
{
    auto vertexDescriptor = NS::TransferPtr(MTL::VertexDescriptor::alloc()->init());
    if (vertexDescriptor.get() != nullptr)
    {
        if (pipelineFlags & METAL_PIPELINE_FLAGS_INTERLEAVED_ATTRS)
        {
            {
                // Position Buffer
                MTL::VertexAttributeDescriptor* vertexAttribute0 = vertexDescriptor->attributes()->object(0);
                vertexAttribute0->setOffset(0);
                vertexAttribute0->setFormat(MTL::VertexFormatFloat3);
                vertexAttribute0->setBufferIndex(0);

                // Vertex Color Buffer
                MTL::VertexAttributeDescriptor* vertexAttribute1 = vertexDescriptor->attributes()->object(1);
                vertexAttribute1->setOffset(12);
                vertexAttribute1->setFormat(MTL::VertexFormatFloat3);
                vertexAttribute1->setBufferIndex(0);
            }

            MTL::VertexBufferLayoutDescriptor* vertexBufferLayout = vertexDescriptor->layouts()->object(0);
            vertexBufferLayout->setStride(24);
            vertexBufferLayout->setStepRate(1);
            vertexBufferLayout->setStepFunction(MTL::VertexStepFunctionPerVertex);
        }
        else
        {
            // Position Buffer
            {
                MTL::VertexAttributeDescriptor* vertexAttribute = vertexDescriptor->attributes()->object(0);
                vertexAttribute->setOffset(0);
                vertexAttribute->setFormat(MTL::VertexFormatFloat3);
                vertexAttribute->setBufferIndex(0);

                MTL::VertexBufferLayoutDescriptor* vertexBufferLayout = vertexDescriptor->layouts()->object(0);
                vertexBufferLayout->setStride(12);
                vertexBufferLayout->setStepRate(1);
                vertexBufferLayout->setStepFunction(MTL::VertexStepFunctionPerVertex);
            }
            // Vertex Color Buffer
            {
                MTL::VertexAttributeDescriptor* vertexAttribute = vertexDescriptor->attributes()->object(1);
                vertexAttribute->setOffset(0);
                vertexAttribute->setFormat(MTL::VertexFormatFloat3);
                vertexAttribute->setBufferIndex(1);

                MTL::VertexBufferLayoutDescriptor* vertexBufferLayout = vertexDescriptor->layouts()->object(1);
                vertexBufferLayout->setStride(12);
                vertexBufferLayout->setStepRate(1);
                vertexBufferLayout->setStepFunction(MTL::VertexStepFunctionPerVertex);
            }
        }
    }
    else
    {
        assert(false && "CreateDrawVertexColorPipeline() - MTL::VertexDescriptor::alloc::init() failed");
        return nullptr;
    }

    auto desc = NS::TransferPtr(MTL::RenderPipelineDescriptor::alloc()->init());

    if (desc.get() != nullptr)
    {
        desc->setVertexFunction(vsShaderModule->Function.get());
        desc->setFragmentFunction(fsShaderModule->Function.get());
        desc->setVertexDescriptor(vertexDescriptor.get());
        desc->colorAttachments()->object(0)->setPixelFormat(rtvFormat);
        desc->setDepthAttachmentPixelFormat(dsvFormat);
        desc->setInputPrimitiveTopology(topologyType);

        NS::Error* pError           = nullptr;
        pPipelineRenderState->State = NS::TransferPtr(pRenderer->Device->newRenderPipelineState(desc.get(), &pError));
        if (pPipelineRenderState->State.get() == nullptr)
        {
            assert(false && "CreateDrawVertexColorPipeline() - MTL::Device::newRenderPipelineState() failed");
            return pError;
        }
    }
    else
    {
        assert(false && "CreateDrawVertexColorPipeline() - MTL::RenderPipelineDescriptor::alloc()->init() failed");
        return nullptr;
    }

    auto depthStateDesc = NS::TransferPtr(MTL::DepthStencilDescriptor::alloc()->init());

    if (depthStateDesc.get() != nullptr)
    {
        depthStateDesc->setDepthCompareFunction(MTL::CompareFunctionLess);
        depthStateDesc->setDepthWriteEnabled(true);

        pDepthStencilState->State = NS::TransferPtr(pRenderer->Device->newDepthStencilState(depthStateDesc.get()));
        if (pDepthStencilState->State.get() == nullptr)
        {
            assert(false && "CreateDrawVertexColorPipeline() - MTL::Device::newDepthStencilState() failed");
            return nullptr;
        }
    }

    return nullptr;
}

NS::Error* CreateDrawNormalPipeline(
    MetalRenderer*            pRenderer,
    MetalShader*              pVsShaderModule,
    MetalShader*              pFsShaderModule,
    MTL::PixelFormat          rtvFormat,
    MTL::PixelFormat          dsvFormat,
    MetalPipelineRenderState* pPipelineRenderState,
    MetalDepthStencilState*   pDepthStencilState,
    bool                      enableTangents)
{
    NS::AutoreleasePool* pPoolAllocator = NS::AutoreleasePool::alloc()->init();

    MTL::VertexDescriptor* pVertexDescriptor = MTL::VertexDescriptor::alloc()->init();
    if (pVertexDescriptor != nullptr)
    {
        // Position Buffer
        {
            MTL::VertexAttributeDescriptor* vertexAttribute = pVertexDescriptor->attributes()->object(0);
            vertexAttribute->setOffset(0);
            vertexAttribute->setFormat(MTL::VertexFormatFloat3);
            vertexAttribute->setBufferIndex(0);

            MTL::VertexBufferLayoutDescriptor* vertexBufferLayout = pVertexDescriptor->layouts()->object(0);
            vertexBufferLayout->setStride(12);
            vertexBufferLayout->setStepRate(1);
            vertexBufferLayout->setStepFunction(MTL::VertexStepFunctionPerVertex);
        }
        // Vertex Normal Buffer
        {
            MTL::VertexAttributeDescriptor* vertexAttribute = pVertexDescriptor->attributes()->object(1);
            vertexAttribute->setOffset(0);
            vertexAttribute->setFormat(MTL::VertexFormatFloat3);
            vertexAttribute->setBufferIndex(1);

            MTL::VertexBufferLayoutDescriptor* vertexBufferLayout = pVertexDescriptor->layouts()->object(1);
            vertexBufferLayout->setStride(12);
            vertexBufferLayout->setStepRate(1);
            vertexBufferLayout->setStepFunction(MTL::VertexStepFunctionPerVertex);
        }
        if (enableTangents)
        {
            // Vertex Tangent Buffer
            {
                MTL::VertexAttributeDescriptor* vertexAttribute = pVertexDescriptor->attributes()->object(2);
                vertexAttribute->setOffset(0);
                vertexAttribute->setFormat(MTL::VertexFormatFloat3);
                vertexAttribute->setBufferIndex(2);

                MTL::VertexBufferLayoutDescriptor* vertexBufferLayout = pVertexDescriptor->layouts()->object(2);
                vertexBufferLayout->setStride(12);
                vertexBufferLayout->setStepRate(1);
                vertexBufferLayout->setStepFunction(MTL::VertexStepFunctionPerVertex);
            }
            // Vertex Bitangent Buffer
            {
                MTL::VertexAttributeDescriptor* vertexAttribute = pVertexDescriptor->attributes()->object(3);
                vertexAttribute->setOffset(0);
                vertexAttribute->setFormat(MTL::VertexFormatFloat3);
                vertexAttribute->setBufferIndex(3);

                MTL::VertexBufferLayoutDescriptor* vertexBufferLayout = pVertexDescriptor->layouts()->object(3);
                vertexBufferLayout->setStride(12);
                vertexBufferLayout->setStepRate(1);
                vertexBufferLayout->setStepFunction(MTL::VertexStepFunctionPerVertex);
            }
        }
    }
    else
    {
        assert(false && "CreateDrawNormalPipeline() - MTL::VertexDescriptor::alloc::init() failed");
    }

    NS::Error* pError = nullptr;
    {
        MTL::RenderPipelineDescriptor* pDesc = MTL::RenderPipelineDescriptor::alloc()->init();

        if (pDesc != nullptr)
        {
            pDesc->setVertexFunction(pVsShaderModule->Function.get());
            pDesc->setFragmentFunction(pFsShaderModule->Function.get());
            pDesc->setVertexDescriptor(pVertexDescriptor);
            pDesc->colorAttachments()->object(0)->setPixelFormat(rtvFormat);
            pDesc->setDepthAttachmentPixelFormat(dsvFormat);

            MTL::RenderPipelineState* pLocalPipelineState = pRenderer->Device->newRenderPipelineState(pDesc, &pError);
            if (pLocalPipelineState != nullptr)
            {
                pPipelineRenderState->State = NS::TransferPtr(pLocalPipelineState);
            }
            else
            {
                assert(false && "CreateDrawNormalPipeline() - MTL::Device::newRenderPipelineState() failed");
            }

            pDesc->release();
        }
        else
        {
            assert(false && "CreateDrawNormalPipeline() - MTL::RenderPipelineDescriptor::alloc()->init() failed");
        }
    }

    if (pError == nullptr)
    {
        MTL::DepthStencilDescriptor* pDepthStateDesc = MTL::DepthStencilDescriptor::alloc()->init();

        if (pDepthStateDesc != nullptr)
        {
            pDepthStateDesc->setDepthCompareFunction(MTL::CompareFunctionLess);
            pDepthStateDesc->setDepthWriteEnabled(true);

            MTL::DepthStencilState* pLocalDepthState = pRenderer->Device->newDepthStencilState(pDepthStateDesc);
            if (pLocalDepthState != nullptr)
            {
                pDepthStencilState->State = NS::TransferPtr(pLocalDepthState);
            }
            else
            {
                assert(false && "CreateDrawNormalPipeline() - MTL::Device::newDepthStencilState() failed");
            }

            pDepthStateDesc->release();
        }
    }

    pVertexDescriptor->release();
    pPoolAllocator->release();

    return pError;
}

NS::Error* CreateDrawTexturePipeline(
    MetalRenderer*            pRenderer,
    MetalShader*              pVsShaderModule,
    MetalShader*              pFsShaderModule,
    MTL::PixelFormat          rtvFormat,
    MTL::PixelFormat          dsvFormat,
    MetalPipelineRenderState* pPipelineRenderState,
    MetalDepthStencilState*   pDepthStencilState)
{
    NS::AutoreleasePool* pPoolAllocator = NS::AutoreleasePool::alloc()->init();

    MTL::VertexDescriptor* pVertexDescriptor = MTL::VertexDescriptor::alloc()->init();
    if (pVertexDescriptor != nullptr)
    {
        // Position Buffer
        {
            MTL::VertexAttributeDescriptor* vertexAttribute = pVertexDescriptor->attributes()->object(0);
            vertexAttribute->setOffset(0);
            vertexAttribute->setFormat(MTL::VertexFormatFloat3);
            vertexAttribute->setBufferIndex(0);

            MTL::VertexBufferLayoutDescriptor* vertexBufferLayout = pVertexDescriptor->layouts()->object(0);
            vertexBufferLayout->setStride(12);
            vertexBufferLayout->setStepRate(1);
            vertexBufferLayout->setStepFunction(MTL::VertexStepFunctionPerVertex);
        }
        // TexCoord Buffer
        {
            MTL::VertexAttributeDescriptor* vertexAttribute = pVertexDescriptor->attributes()->object(1);
            vertexAttribute->setOffset(0);
            vertexAttribute->setFormat(MTL::VertexFormatFloat2);
            vertexAttribute->setBufferIndex(1);

            MTL::VertexBufferLayoutDescriptor* vertexBufferLayout = pVertexDescriptor->layouts()->object(1);
            vertexBufferLayout->setStride(8);
            vertexBufferLayout->setStepRate(1);
            vertexBufferLayout->setStepFunction(MTL::VertexStepFunctionPerVertex);
        }
    }
    else
    {
        assert(false && "CreateDrawTexturePipeline() - MTL::VertexDescriptor::alloc::init() failed");
    }

    NS::Error* pError = nullptr;
    {
        MTL::RenderPipelineDescriptor* pDesc = MTL::RenderPipelineDescriptor::alloc()->init();

        if (pDesc != nullptr)
        {
            pDesc->setVertexFunction(pVsShaderModule->Function.get());
            pDesc->setFragmentFunction(pFsShaderModule->Function.get());
            pDesc->setVertexDescriptor(pVertexDescriptor);
            pDesc->colorAttachments()->object(0)->setPixelFormat(rtvFormat);
            pDesc->setDepthAttachmentPixelFormat(dsvFormat);

            MTL::RenderPipelineState* pLocalPipelineState = pRenderer->Device->newRenderPipelineState(pDesc, &pError);
            if (pLocalPipelineState != nullptr)
            {
                pPipelineRenderState->State = NS::TransferPtr(pLocalPipelineState);
            }
            else
            {
                assert(false && "CreateDrawTexturePipeline() - MTL::Device::newRenderPipelineState() failed");
            }

            pDesc->release();
        }
        else
        {
            assert(false && "CreateDrawTexturePipeline() - MTL::RenderPipelineDescriptor::alloc()->init() failed");
        }
    }

    if (pError == nullptr)
    {
        MTL::DepthStencilDescriptor* pDepthStateDesc = MTL::DepthStencilDescriptor::alloc()->init();

        if (pDepthStateDesc != nullptr)
        {
            pDepthStateDesc->setDepthCompareFunction(MTL::CompareFunctionLess);
            pDepthStateDesc->setDepthWriteEnabled(true);

            MTL::DepthStencilState* pLocalDepthState = pRenderer->Device->newDepthStencilState(pDepthStateDesc);
            if (pLocalDepthState != nullptr)
            {
                pDepthStencilState->State = NS::TransferPtr(pLocalDepthState);
            }
            else
            {
                assert(false && "CreateDrawTexturePipeline() - MTL::Device::newDepthStencilState() failed");
            }

            pDepthStateDesc->release();
        }
    }

    pVertexDescriptor->release();
    pPoolAllocator->release();

    return pError;
}

NS::Error* CreateDrawBasicPipeline(
    MetalRenderer*            pRenderer,
    MetalShader*              pVsShaderModule,
    MetalShader*              pFsShaderModule,
    MTL::PixelFormat          rtvFormat,
    MTL::PixelFormat          dsvFormat,
    MetalPipelineRenderState* pPipelineRenderState,
    MetalDepthStencilState*   pDepthStencilState)
{
    NS::AutoreleasePool* pPoolAllocator = NS::AutoreleasePool::alloc()->init();

    MTL::VertexDescriptor* pVertexDescriptor = MTL::VertexDescriptor::alloc()->init();
    if (pVertexDescriptor != nullptr)
    {
        // Position Buffer
        {
            MTL::VertexAttributeDescriptor* vertexAttribute = pVertexDescriptor->attributes()->object(0);
            vertexAttribute->setOffset(0);
            vertexAttribute->setFormat(MTL::VertexFormatFloat3);
            vertexAttribute->setBufferIndex(0);

            MTL::VertexBufferLayoutDescriptor* vertexBufferLayout = pVertexDescriptor->layouts()->object(0);
            vertexBufferLayout->setStride(12);
            vertexBufferLayout->setStepRate(1);
            vertexBufferLayout->setStepFunction(MTL::VertexStepFunctionPerVertex);
        }
        // TexCoord Buffer
        {
            MTL::VertexAttributeDescriptor* vertexAttribute = pVertexDescriptor->attributes()->object(1);
            vertexAttribute->setOffset(0);
            vertexAttribute->setFormat(MTL::VertexFormatFloat2);
            vertexAttribute->setBufferIndex(1);

            MTL::VertexBufferLayoutDescriptor* vertexBufferLayout = pVertexDescriptor->layouts()->object(1);
            vertexBufferLayout->setStride(8);
            vertexBufferLayout->setStepRate(1);
            vertexBufferLayout->setStepFunction(MTL::VertexStepFunctionPerVertex);
        }
        // Normal Buffer
        {
            MTL::VertexAttributeDescriptor* vertexAttribute = pVertexDescriptor->attributes()->object(2);
            vertexAttribute->setOffset(0);
            vertexAttribute->setFormat(MTL::VertexFormatFloat3);
            vertexAttribute->setBufferIndex(2);

            MTL::VertexBufferLayoutDescriptor* vertexBufferLayout = pVertexDescriptor->layouts()->object(2);
            vertexBufferLayout->setStride(12);
            vertexBufferLayout->setStepRate(1);
            vertexBufferLayout->setStepFunction(MTL::VertexStepFunctionPerVertex);
        }
    }
    else
    {
        assert(false && "CreateDrawTexturePipeline() - MTL::VertexDescriptor::alloc::init() failed");
    }

    NS::Error* pError = nullptr;
    {
        MTL::RenderPipelineDescriptor* pDesc = MTL::RenderPipelineDescriptor::alloc()->init();

        if (pDesc != nullptr)
        {
            pDesc->setVertexFunction(pVsShaderModule->Function.get());
            pDesc->setFragmentFunction(pFsShaderModule->Function.get());
            pDesc->setVertexDescriptor(pVertexDescriptor);
            pDesc->colorAttachments()->object(0)->setPixelFormat(rtvFormat);
            pDesc->setDepthAttachmentPixelFormat(dsvFormat);

            MTL::RenderPipelineState* pLocalPipelineState = pRenderer->Device->newRenderPipelineState(pDesc, &pError);
            if (pLocalPipelineState != nullptr)
            {
                pPipelineRenderState->State = NS::TransferPtr(pLocalPipelineState);
            }
            else
            {
                assert(false && "CreateDrawBasicPipeline() - MTL::Device::newRenderPipelineState() failed");
            }

            pDesc->release();
        }
        else
        {
            assert(false && "CreateDrawBasicPipeline() - MTL::RenderPipelineDescriptor::alloc()->init() failed");
        }
    }

    if (pError == nullptr)
    {
        MTL::DepthStencilDescriptor* pDepthStateDesc = MTL::DepthStencilDescriptor::alloc()->init();

        if (pDepthStateDesc != nullptr)
        {
            pDepthStateDesc->setDepthCompareFunction(MTL::CompareFunctionLess);
            pDepthStateDesc->setDepthWriteEnabled(true);

            MTL::DepthStencilState* pLocalDepthState = pRenderer->Device->newDepthStencilState(pDepthStateDesc);
            if (pLocalDepthState != nullptr)
            {
                pDepthStencilState->State = NS::TransferPtr(pLocalDepthState);
            }
            else
            {
                assert(false && "CreateDrawBasicPipeline() - MTL::Device::newDepthStencilState() failed");
            }

            pDepthStateDesc->release();
        }
    }

    pVertexDescriptor->release();
    pPoolAllocator->release();

    return pError;
}

NS::Error* CreateGraphicsPipeline1(
    MetalRenderer*            pRenderer,
    MetalShader*              pVsShaderModule,
    MetalShader*              pFsShaderModule,
    MTL::PixelFormat          rtvFormat,
    MTL::PixelFormat          dsvFormat,
    MetalPipelineRenderState* pPipelineRenderState,
    MetalDepthStencilState*   pDepthStencilState)
{
    MTL::VertexDescriptor* pVertexDescriptor = MTL::VertexDescriptor::alloc()->init();
    if (pVertexDescriptor != nullptr)
    {
        const uint32_t inputCount = 5;

        MTL::VertexFormat formats[inputCount] = {
            MTL::VertexFormatFloat3,  // Position
            MTL::VertexFormatFloat2,  // TexCoord
            MTL::VertexFormatFloat3,  // Normal
            MTL::VertexFormatFloat3,  // Tangent
            MTL::VertexFormatFloat3}; // Bitangent

        uint32_t offsets[inputCount] = {0, 0, 0, 0, 0};
        uint32_t strides[inputCount];

        for (int inputIndex = 0; inputIndex < inputCount; inputIndex++)
        {
            strides[inputIndex] = BitsPerElement(formats[inputIndex]) / 8;
        }

        for (int inputIndex = 0; inputIndex < inputCount; inputIndex++)
        {
            MTL::VertexAttributeDescriptor* vertexAttribute = pVertexDescriptor->attributes()->object(inputIndex);
            vertexAttribute->setOffset(offsets[inputIndex]);
            vertexAttribute->setFormat(formats[inputIndex]);
            vertexAttribute->setBufferIndex(inputIndex);

            MTL::VertexBufferLayoutDescriptor* vertexBufferLayout = pVertexDescriptor->layouts()->object(inputIndex);
            vertexBufferLayout->setStride(strides[inputIndex]);
            vertexBufferLayout->setStepRate(1);
            vertexBufferLayout->setStepFunction(MTL::VertexStepFunctionPerVertex);
        }
    }
    else
    {
        assert(false && "CreateGraphicsPipeline1() - MTL::VertexDescriptor::alloc::init() failed");
    }

    NS::Error* pError = nullptr;
    {
        MTL::RenderPipelineDescriptor* pDesc = MTL::RenderPipelineDescriptor::alloc()->init();

        if (pDesc != nullptr)
        {
            pDesc->setVertexFunction(pVsShaderModule->Function.get());
            pDesc->setFragmentFunction(pFsShaderModule->Function.get());
            pDesc->setVertexDescriptor(pVertexDescriptor);
            pDesc->colorAttachments()->object(0)->setPixelFormat(rtvFormat);
            pDesc->setDepthAttachmentPixelFormat(dsvFormat);

            MTL::RenderPipelineState* pLocalPipelineState = pRenderer->Device->newRenderPipelineState(pDesc, &pError);
            if (pLocalPipelineState != nullptr)
            {
                pPipelineRenderState->State = NS::TransferPtr(pLocalPipelineState);
            }
            else
            {
                assert(false && "CreateGraphicsPipeline1() - MTL::Device::newRenderPipelineState() failed");
            }

            pDesc->release();
        }
        else
        {
            assert(false && "CreateGraphicsPipeline1() - MTL::RenderPipelineDescriptor::alloc()->init() failed");
        }
    }

    if (pError == nullptr)
    {
        MTL::DepthStencilDescriptor* pDepthStateDesc = MTL::DepthStencilDescriptor::alloc()->init();

        if (pDepthStateDesc != nullptr)
        {
            pDepthStateDesc->setDepthCompareFunction(MTL::CompareFunctionLess);
            pDepthStateDesc->setDepthWriteEnabled(true);

            MTL::DepthStencilState* pLocalDepthState = pRenderer->Device->newDepthStencilState(pDepthStateDesc);
            if (pLocalDepthState != nullptr)
            {
                pDepthStencilState->State = NS::TransferPtr(pLocalDepthState);
            }
            else
            {
                assert(false && "CreateGraphicsPipeline1() - MTL::Device::newDepthStencilState() failed");
            }

            pDepthStateDesc->release();
        }
    }

    pVertexDescriptor->release();

    return pError;
}

NS::Error* CreateGraphicsPipeline2(
    MetalRenderer*            pRenderer,
    MetalShader*              pVsShaderModule,
    MetalShader*              pFsShaderModule,
    MTL::PixelFormat          rtvFormat,
    MTL::PixelFormat          dsvFormat,
    MetalPipelineRenderState* pPipelineRenderState,
    MetalDepthStencilState*   pDepthStencilState,
    uint32_t*                 pOptionalVertexStrides)
{
    MTL::VertexDescriptor* pVertexDescriptor = MTL::VertexDescriptor::alloc()->init();
    if (pVertexDescriptor != nullptr)
    {
        const uint32_t inputCount = 4;

        MTL::VertexFormat formats[inputCount] = {
            MTL::VertexFormatFloat3,  // Position
            MTL::VertexFormatFloat2,  // TexCoord
            MTL::VertexFormatFloat3,  // Normal
            MTL::VertexFormatFloat3}; // Tangent

        uint32_t offsets[inputCount] = {0, 0, 0, 0};
        uint32_t strides[inputCount];

        for (int inputIndex = 0; inputIndex < inputCount; inputIndex++)
        {
            strides[inputIndex] = BitsPerElement(formats[inputIndex]) / 8;
        }

        if (pOptionalVertexStrides)
        {
            memcpy(strides, pOptionalVertexStrides, inputCount * sizeof(uint32_t));
        }

        for (int inputIndex = 0; inputIndex < inputCount; inputIndex++)
        {
            MTL::VertexAttributeDescriptor* vertexAttribute = pVertexDescriptor->attributes()->object(inputIndex);
            vertexAttribute->setOffset(offsets[inputIndex]);
            vertexAttribute->setFormat(formats[inputIndex]);
            vertexAttribute->setBufferIndex(inputIndex);

            MTL::VertexBufferLayoutDescriptor* vertexBufferLayout = pVertexDescriptor->layouts()->object(inputIndex);
            vertexBufferLayout->setStride(strides[inputIndex]);
            vertexBufferLayout->setStepRate(1);
            vertexBufferLayout->setStepFunction(MTL::VertexStepFunctionPerVertex);
        }
    }
    else
    {
        assert(false && "CreateGraphicsPipeline2() - MTL::VertexDescriptor::alloc::init() failed");
    }

    NS::Error* pError = nullptr;
    {
        MTL::RenderPipelineDescriptor* pDesc = MTL::RenderPipelineDescriptor::alloc()->init();

        if (pDesc != nullptr)
        {
            pDesc->setVertexFunction(pVsShaderModule->Function.get());
            pDesc->setFragmentFunction(pFsShaderModule->Function.get());
            pDesc->setVertexDescriptor(pVertexDescriptor);
            pDesc->colorAttachments()->object(0)->setPixelFormat(rtvFormat);
            pDesc->setDepthAttachmentPixelFormat(dsvFormat);

            MTL::RenderPipelineState* pLocalPipelineState = pRenderer->Device->newRenderPipelineState(pDesc, &pError);
            if (pLocalPipelineState != nullptr)
            {
                pPipelineRenderState->State = NS::TransferPtr(pLocalPipelineState);
            }
            else
            {
                assert(false && "CreateGraphicsPipeline2() - MTL::Device::newRenderPipelineState() failed");
            }

            pDesc->release();
        }
        else
        {
            assert(false && "CreateGraphicsPipeline2() - MTL::RenderPipelineDescriptor::alloc()->init() failed");
        }
    }

    if (pError == nullptr)
    {
        MTL::DepthStencilDescriptor* pDepthStateDesc = MTL::DepthStencilDescriptor::alloc()->init();

        if (pDepthStateDesc != nullptr)
        {
            pDepthStateDesc->setDepthCompareFunction(MTL::CompareFunctionLess);
            pDepthStateDesc->setDepthWriteEnabled(true);

            MTL::DepthStencilState* pLocalDepthState = pRenderer->Device->newDepthStencilState(pDepthStateDesc);
            if (pLocalDepthState != nullptr)
            {
                pDepthStencilState->State = NS::TransferPtr(pLocalDepthState);
            }
            else
            {
                assert(false && "CreateGraphicsPipeline2() - MTL::Device::newDepthStencilState() failed");
            }

            pDepthStateDesc->release();
        }
    }

    pVertexDescriptor->release();

    return pError;
}

bool IsCompressed(MTL::PixelFormat fmt)
{
    switch (fmt)
    {
        case MTL::PixelFormatBC1_RGBA:
        case MTL::PixelFormatBC1_RGBA_sRGB:
        case MTL::PixelFormatPVRTC_RGB_2BPP:
        case MTL::PixelFormatPVRTC_RGB_2BPP_sRGB:
        case MTL::PixelFormatBC2_RGBA:
        case MTL::PixelFormatBC2_RGBA_sRGB:
        case MTL::PixelFormatBC3_RGBA:
        case MTL::PixelFormatBC3_RGBA_sRGB:
        case MTL::PixelFormatBC4_RUnorm:
        case MTL::PixelFormatBC4_RSnorm:
        case MTL::PixelFormatBC5_RGUnorm:
        case MTL::PixelFormatBC5_RGSnorm:
        case MTL::PixelFormatBC6H_RGBFloat:
        case MTL::PixelFormatBC6H_RGBUfloat:
        case MTL::PixelFormatBC7_RGBAUnorm:
        case MTL::PixelFormatBC7_RGBAUnorm_sRGB:
        case MTL::PixelFormatPVRTC_RGBA_2BPP:
        case MTL::PixelFormatPVRTC_RGBA_2BPP_sRGB:
        case MTL::PixelFormatEAC_R11Unorm:
        case MTL::PixelFormatEAC_R11Snorm:
        case MTL::PixelFormatPVRTC_RGB_4BPP:
        case MTL::PixelFormatPVRTC_RGB_4BPP_sRGB:
        case MTL::PixelFormatPVRTC_RGBA_4BPP:
        case MTL::PixelFormatPVRTC_RGBA_4BPP_sRGB:
        case MTL::PixelFormatEAC_RG11Unorm:
        case MTL::PixelFormatEAC_RG11Snorm:
        case MTL::PixelFormatETC2_RGB8:
        case MTL::PixelFormatETC2_RGB8_sRGB:
        case MTL::PixelFormatEAC_RGBA8:
        case MTL::PixelFormatEAC_RGBA8_sRGB:
        case MTL::PixelFormatETC2_RGB8A1:
        case MTL::PixelFormatETC2_RGB8A1_sRGB:
        case MTL::PixelFormatASTC_4x4_sRGB:
        case MTL::PixelFormatASTC_5x4_sRGB:
        case MTL::PixelFormatASTC_5x5_sRGB:
        case MTL::PixelFormatASTC_6x5_sRGB:
        case MTL::PixelFormatASTC_6x6_sRGB:
        case MTL::PixelFormatASTC_8x5_sRGB:
        case MTL::PixelFormatASTC_8x6_sRGB:
        case MTL::PixelFormatASTC_8x8_sRGB:
        case MTL::PixelFormatASTC_10x5_sRGB:
        case MTL::PixelFormatASTC_10x6_sRGB:
        case MTL::PixelFormatASTC_10x8_sRGB:
        case MTL::PixelFormatASTC_10x10_sRGB:
        case MTL::PixelFormatASTC_12x10_sRGB:
        case MTL::PixelFormatASTC_12x12_sRGB:
        case MTL::PixelFormatASTC_4x4_LDR:
        case MTL::PixelFormatASTC_5x4_LDR:
        case MTL::PixelFormatASTC_5x5_LDR:
        case MTL::PixelFormatASTC_6x5_LDR:
        case MTL::PixelFormatASTC_6x6_LDR:
        case MTL::PixelFormatASTC_8x5_LDR:
        case MTL::PixelFormatASTC_8x6_LDR:
        case MTL::PixelFormatASTC_8x8_LDR:
        case MTL::PixelFormatASTC_10x5_LDR:
        case MTL::PixelFormatASTC_10x6_LDR:
        case MTL::PixelFormatASTC_10x8_LDR:
        case MTL::PixelFormatASTC_10x10_LDR:
        case MTL::PixelFormatASTC_12x10_LDR:
        case MTL::PixelFormatASTC_12x12_LDR:
        case MTL::PixelFormatASTC_4x4_HDR:
        case MTL::PixelFormatASTC_5x4_HDR:
        case MTL::PixelFormatASTC_5x5_HDR:
        case MTL::PixelFormatASTC_6x5_HDR:
        case MTL::PixelFormatASTC_6x6_HDR:
        case MTL::PixelFormatASTC_8x5_HDR:
        case MTL::PixelFormatASTC_8x6_HDR:
        case MTL::PixelFormatASTC_8x8_HDR:
        case MTL::PixelFormatASTC_10x5_HDR:
        case MTL::PixelFormatASTC_10x6_HDR:
        case MTL::PixelFormatASTC_10x8_HDR:
        case MTL::PixelFormatASTC_10x10_HDR:
        case MTL::PixelFormatASTC_12x10_HDR:
        case MTL::PixelFormatASTC_12x12_HDR:
            return true;

        default:
            return false;
    }
}

uint32_t BitsPerPixel(MTL::PixelFormat fmt)
{
    switch (static_cast<int>(fmt))
    {
        case MTL::PixelFormatInvalid:
            return 0xFFFF;

        case MTL::PixelFormatBC1_RGBA:
        case MTL::PixelFormatBC1_RGBA_sRGB:
            return 4;

        case MTL::PixelFormatPVRTC_RGB_2BPP:
        case MTL::PixelFormatPVRTC_RGB_2BPP_sRGB:
            return 6;

        case MTL::PixelFormatA8Unorm:
        case MTL::PixelFormatR8Unorm:
        case MTL::PixelFormatR8Unorm_sRGB:
        case MTL::PixelFormatR8Snorm:
        case MTL::PixelFormatR8Uint:
        case MTL::PixelFormatR8Sint:
        case MTL::PixelFormatBC2_RGBA:
        case MTL::PixelFormatBC2_RGBA_sRGB:
        case MTL::PixelFormatBC3_RGBA:
        case MTL::PixelFormatBC3_RGBA_sRGB:
        case MTL::PixelFormatBC4_RUnorm:
        case MTL::PixelFormatBC4_RSnorm:
        case MTL::PixelFormatBC5_RGUnorm:
        case MTL::PixelFormatBC5_RGSnorm:
        case MTL::PixelFormatBC6H_RGBFloat:
        case MTL::PixelFormatBC6H_RGBUfloat:
        case MTL::PixelFormatBC7_RGBAUnorm:
        case MTL::PixelFormatBC7_RGBAUnorm_sRGB:
        case MTL::PixelFormatPVRTC_RGBA_2BPP:
        case MTL::PixelFormatPVRTC_RGBA_2BPP_sRGB:
        case MTL::PixelFormatEAC_R11Unorm:
        case MTL::PixelFormatEAC_R11Snorm:
        case MTL::PixelFormatStencil8:
            return 8;

        case MTL::PixelFormatPVRTC_RGB_4BPP:
        case MTL::PixelFormatPVRTC_RGB_4BPP_sRGB:
            return 12;

        case MTL::PixelFormatR16Unorm:
        case MTL::PixelFormatR16Snorm:
        case MTL::PixelFormatR16Uint:
        case MTL::PixelFormatR16Sint:
        case MTL::PixelFormatR16Float:
        case MTL::PixelFormatRG8Unorm:
        case MTL::PixelFormatRG8Unorm_sRGB:
        case MTL::PixelFormatRG8Snorm:
        case MTL::PixelFormatRG8Uint:
        case MTL::PixelFormatRG8Sint:
        case MTL::PixelFormatB5G6R5Unorm:
        case MTL::PixelFormatA1BGR5Unorm:
        case MTL::PixelFormatABGR4Unorm:
        case MTL::PixelFormatBGR5A1Unorm:
        case MTL::PixelFormatPVRTC_RGBA_4BPP:
        case MTL::PixelFormatPVRTC_RGBA_4BPP_sRGB:
        case MTL::PixelFormatDepth16Unorm:
        case MTL::PixelFormatEAC_RG11Unorm:
        case MTL::PixelFormatEAC_RG11Snorm:
            return 16;

        case MTL::PixelFormatETC2_RGB8:
        case MTL::PixelFormatETC2_RGB8_sRGB:
            return 24;

        case MTL::PixelFormatR32Uint:
        case MTL::PixelFormatR32Sint:
        case MTL::PixelFormatR32Float:
        case MTL::PixelFormatRG16Unorm:
        case MTL::PixelFormatRG16Snorm:
        case MTL::PixelFormatRG16Uint:
        case MTL::PixelFormatRG16Sint:
        case MTL::PixelFormatRG16Float:
        case MTL::PixelFormatRGBA8Unorm:
        case MTL::PixelFormatRGBA8Unorm_sRGB:
        case MTL::PixelFormatRGBA8Snorm:
        case MTL::PixelFormatRGBA8Uint:
        case MTL::PixelFormatRGBA8Sint:
        case MTL::PixelFormatBGRA8Unorm:
        case MTL::PixelFormatBGRA8Unorm_sRGB:
        case MTL::PixelFormatRGB10A2Unorm:
        case MTL::PixelFormatRGB10A2Uint:
        case MTL::PixelFormatRG11B10Float:
        case MTL::PixelFormatRGB9E5Float:
        case MTL::PixelFormatBGR10A2Unorm:
        case MTL::PixelFormatEAC_RGBA8:
        case MTL::PixelFormatEAC_RGBA8_sRGB:
        case MTL::PixelFormatDepth32Float:
        case MTL::PixelFormatDepth24Unorm_Stencil8:
        case MTL::PixelFormatX24_Stencil8:
        case MTL::PixelFormatBGR10_XR:
        case MTL::PixelFormatBGR10_XR_sRGB:
        case MTL::PixelFormatGBGR422:
        case MTL::PixelFormatBGRG422:
        case MTL::PixelFormatETC2_RGB8A1:
        case MTL::PixelFormatETC2_RGB8A1_sRGB:
            return 32;

        case MTL::PixelFormatDepth32Float_Stencil8:
        case MTL::PixelFormatX32_Stencil8:
            return 40;

        case MTL::PixelFormatRG32Uint:
        case MTL::PixelFormatRG32Sint:
        case MTL::PixelFormatRG32Float:
        case MTL::PixelFormatRGBA16Unorm:
        case MTL::PixelFormatRGBA16Snorm:
        case MTL::PixelFormatRGBA16Uint:
        case MTL::PixelFormatRGBA16Sint:
        case MTL::PixelFormatRGBA16Float:
        case MTL::PixelFormatBGRA10_XR:
        case MTL::PixelFormatBGRA10_XR_sRGB:
            return 64;

        case MTL::PixelFormatRGBA32Uint:
        case MTL::PixelFormatRGBA32Sint:
        case MTL::PixelFormatRGBA32Float:
        case MTL::PixelFormatASTC_4x4_sRGB:
        case MTL::PixelFormatASTC_5x4_sRGB:
        case MTL::PixelFormatASTC_5x5_sRGB:
        case MTL::PixelFormatASTC_6x5_sRGB:
        case MTL::PixelFormatASTC_6x6_sRGB:
        case MTL::PixelFormatASTC_8x5_sRGB:
        case MTL::PixelFormatASTC_8x6_sRGB:
        case MTL::PixelFormatASTC_8x8_sRGB:
        case MTL::PixelFormatASTC_10x5_sRGB:
        case MTL::PixelFormatASTC_10x6_sRGB:
        case MTL::PixelFormatASTC_10x8_sRGB:
        case MTL::PixelFormatASTC_10x10_sRGB:
        case MTL::PixelFormatASTC_12x10_sRGB:
        case MTL::PixelFormatASTC_12x12_sRGB:
        case MTL::PixelFormatASTC_4x4_LDR:
        case MTL::PixelFormatASTC_5x4_LDR:
        case MTL::PixelFormatASTC_5x5_LDR:
        case MTL::PixelFormatASTC_6x5_LDR:
        case MTL::PixelFormatASTC_6x6_LDR:
        case MTL::PixelFormatASTC_8x5_LDR:
        case MTL::PixelFormatASTC_8x6_LDR:
        case MTL::PixelFormatASTC_8x8_LDR:
        case MTL::PixelFormatASTC_10x5_LDR:
        case MTL::PixelFormatASTC_10x6_LDR:
        case MTL::PixelFormatASTC_10x8_LDR:
        case MTL::PixelFormatASTC_10x10_LDR:
        case MTL::PixelFormatASTC_12x10_LDR:
        case MTL::PixelFormatASTC_12x12_LDR:
        case MTL::PixelFormatASTC_4x4_HDR:
        case MTL::PixelFormatASTC_5x4_HDR:
        case MTL::PixelFormatASTC_5x5_HDR:
        case MTL::PixelFormatASTC_6x5_HDR:
        case MTL::PixelFormatASTC_6x6_HDR:
        case MTL::PixelFormatASTC_8x5_HDR:
        case MTL::PixelFormatASTC_8x6_HDR:
        case MTL::PixelFormatASTC_8x8_HDR:
        case MTL::PixelFormatASTC_10x5_HDR:
        case MTL::PixelFormatASTC_10x6_HDR:
        case MTL::PixelFormatASTC_10x8_HDR:
        case MTL::PixelFormatASTC_10x10_HDR:
        case MTL::PixelFormatASTC_12x10_HDR:
        case MTL::PixelFormatASTC_12x12_HDR:
            return 128;
    }
}

uint32_t BitsPerElement(MTL::VertexFormat fmt)
{
    switch (fmt)
    {
        case MTL::VertexFormatInvalid:
            return 0;

        case MTL::VertexFormatUChar:
        case MTL::VertexFormatChar:
        case MTL::VertexFormatUCharNormalized:
        case MTL::VertexFormatCharNormalized:
            return 8;

        case MTL::VertexFormatUChar2:
        case MTL::VertexFormatChar2:
        case MTL::VertexFormatUChar2Normalized:
        case MTL::VertexFormatChar2Normalized:
        case MTL::VertexFormatUShort:
        case MTL::VertexFormatShort:
        case MTL::VertexFormatUShortNormalized:
        case MTL::VertexFormatShortNormalized:
        case MTL::VertexFormatHalf:
            return 16;

        case MTL::VertexFormatUChar3:
        case MTL::VertexFormatChar3:
        case MTL::VertexFormatUChar3Normalized:
        case MTL::VertexFormatChar3Normalized:
            return 24;

        case MTL::VertexFormatUChar4:
        case MTL::VertexFormatChar4:
        case MTL::VertexFormatUChar4Normalized:
        case MTL::VertexFormatChar4Normalized:
        case MTL::VertexFormatUShort2:
        case MTL::VertexFormatShort2:
        case MTL::VertexFormatUShort2Normalized:
        case MTL::VertexFormatShort2Normalized:
        case MTL::VertexFormatHalf2:
        case MTL::VertexFormatFloat:
        case MTL::VertexFormatInt:
        case MTL::VertexFormatUInt:
        case MTL::VertexFormatInt1010102Normalized:
        case MTL::VertexFormatUInt1010102Normalized:
        case MTL::VertexFormatFloatRG11B10:
        case MTL::VertexFormatFloatRGB9E5:
            return 32;

        case MTL::VertexFormatShort3:
        case MTL::VertexFormatUShort3:
        case MTL::VertexFormatUShort3Normalized:
        case MTL::VertexFormatShort3Normalized:
        case MTL::VertexFormatHalf3:
            return 48;

        case MTL::VertexFormatUShort4:
        case MTL::VertexFormatShort4:
        case MTL::VertexFormatUShort4Normalized:
        case MTL::VertexFormatShort4Normalized:
        case MTL::VertexFormatFloat2:
        case MTL::VertexFormatHalf4:
        case MTL::VertexFormatInt2:
        case MTL::VertexFormatUInt2:
            return 64;

        case MTL::VertexFormatFloat3:
        case MTL::VertexFormatInt3:
        case MTL::VertexFormatUInt3:
            return 96;

        case MTL::VertexFormatFloat4:
        case MTL::VertexFormatInt4:
        case MTL::VertexFormatUInt4:
        case MTL::VertexFormatUChar4Normalized_BGRA:
            return 128;

        default:
            return 0;
    }
}
