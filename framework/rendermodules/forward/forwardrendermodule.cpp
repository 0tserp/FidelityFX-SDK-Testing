// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "forwardrendermodule.h"

#include "core/framework.h"
#include "core/components/cameracomponent.h"
#include "core/components/meshcomponent.h"
#include "core/components/animationcomponent.h"
#include "core/scene.h"
#include "render/device.h"
#include "render/parameterset.h"
#include "render/pipelineobject.h"
#include "render/profiler.h"
#include "render/rasterview.h"
#include "render/resourceview.h"
#include "render/rootsignature.h"
#include "render/sampler.h"
#include "render/shaderbuilderhelper.h"
#include "shaders/surfacerendercommon.h"

#include <functional>

#include "lighting/lightingRenderModule.h"

using namespace cauldron;

void ForwardRenderModule::Init(const json& initData)
{
    m_GenerateMotionVectors = (GetFramework()->GetConfig()->MotionVectorGeneration == "ForwardRenderModule");
    m_VariableShading = initData.value("VariableShading", m_VariableShading);

    // Setup raster views for all targets
    m_pColorRenderTarget = GetFramework()->GetColorTargetForCallback(GetName());
    m_pMotionVector = m_GenerateMotionVectors ? GetFramework()->GetRenderTexture(L"MotionVectorTarget") : nullptr;
    m_pDepthTarget = GetFramework()->GetRenderTexture(L"DepthTarget");

    m_RasterViews.resize(3);
    m_RasterViews[0] = GetRasterViewAllocator()->RequestRasterView(m_pColorRenderTarget, ViewDimension::Texture2D);
    if (m_GenerateMotionVectors)
    {
        m_RasterViews[1] = GetRasterViewAllocator()->RequestRasterView(m_pMotionVector, ViewDimension::Texture2D);
    }
    m_RasterViews[2] = GetRasterViewAllocator()->RequestRasterView(m_pDepthTarget, ViewDimension::Texture2D);


    // Reserve space for the max number of supported textures (use a bindless approach to resource indexing)
    m_Textures.reserve(MAX_TEXTURES_COUNT / 2);

    // Reserve space for the max number of samplers
    m_Samplers.reserve(MAX_SAMPLERS_COUNT / 2);

    // Root signature
    RootSignatureDesc signatureDesc;
    signatureDesc.AddConstantBufferView(0, ShaderBindStage::VertexAndPixel, 1); // Frame Information
    signatureDesc.AddConstantBufferView(1, ShaderBindStage::VertexAndPixel, 1); // Instance Information
    signatureDesc.AddConstantBufferView(2, ShaderBindStage::Pixel, 1);          // Texture Indices
    signatureDesc.AddConstantBufferView(3, ShaderBindStage::Pixel, 1);          // scene lighting information
    signatureDesc.AddConstantBufferView(4, ShaderBindStage::Pixel, 1);          // IBL factor
    
    signatureDesc.AddTextureSRVSet(0, ShaderBindStage::Pixel, 500); // Texture resource array t0 - t499
    signatureDesc.AddTextureSRVSet(500, ShaderBindStage::Pixel, 1); // brdfTexture
    signatureDesc.AddTextureSRVSet(501, ShaderBindStage::Pixel, 1); // irradianceCube
    signatureDesc.AddTextureSRVSet(502, ShaderBindStage::Pixel, 1); // prefilteredCube
    signatureDesc.AddTextureSRVSet(503, ShaderBindStage::Pixel, MAX_SHADOW_MAP_TEXTURES_COUNT); // ShadowMapTextures

    // Create sampler set
    signatureDesc.AddSamplerSet(0, ShaderBindStage::Pixel, 10);

    //Create samplers for lighting
    std::vector<SamplerDesc> samplers;
    static bool s_InvertedDepth = GetConfig()->InvertedDepth;

    SamplerDesc comparisonSampler;
    comparisonSampler.Comparison = s_InvertedDepth ? ComparisonFunc::GreaterEqual : ComparisonFunc::LessEqual;
    comparisonSampler.Filter = FilterFunc::ComparisonMinMagLinearMipPoint;
    comparisonSampler.MaxAnisotropy = 1;
    samplers.clear();
    samplers.push_back(comparisonSampler);
    signatureDesc.AddStaticSamplers(13, ShaderBindStage::Pixel, static_cast<uint32_t>(samplers.size()), samplers.data());

    // Setup samplers for brdfTexture, irradianceCube and prefilteredCube
    {
        SamplerDesc prefilteredCubeSampler;
        prefilteredCubeSampler.AddressW = AddressMode::Wrap;
        prefilteredCubeSampler.Filter = FilterFunc::MinMagMipLinear;
        prefilteredCubeSampler.MaxAnisotropy = 1;

        samplers.clear();
        samplers.push_back(prefilteredCubeSampler);
        signatureDesc.AddStaticSamplers(10, ShaderBindStage::Pixel, static_cast<uint32_t>(samplers.size()), samplers.data());
        signatureDesc.AddStaticSamplers(12, ShaderBindStage::Pixel, static_cast<uint32_t>(samplers.size()), samplers.data());


        SamplerDesc irradianceCubeSampler;
        irradianceCubeSampler.Filter = FilterFunc::MinMagMipPoint;
        irradianceCubeSampler.AddressW = AddressMode::Wrap;
        irradianceCubeSampler.Filter = FilterFunc::MinMagMipPoint;
        irradianceCubeSampler.MaxAnisotropy = 1;
        samplers.clear();
        samplers.push_back(irradianceCubeSampler);
        signatureDesc.AddStaticSamplers(11, ShaderBindStage::Pixel, static_cast<uint32_t>(samplers.size()), samplers.data());
    }

    m_pRootSignature = RootSignature::CreateRootSignature(L"ForwardRenderPass_RootSignature", signatureDesc);

    // Create ParameterSet and assign the constant buffer parameters
    // We will add texture views as they are loaded
    m_pParameterSet = ParameterSet::CreateParameterSet(m_pRootSignature);
    m_pParameterSet->SetRootConstantBufferResource(GetDynamicBufferPool()->GetResource(), sizeof(SceneInformation), 0);
    m_pParameterSet->SetRootConstantBufferResource(GetDynamicBufferPool()->GetResource(), sizeof(InstanceInformation), 1);
    m_pParameterSet->SetRootConstantBufferResource(GetDynamicBufferPool()->GetResource(), sizeof(TextureIndices), 2);
    m_pParameterSet->SetRootConstantBufferResource(GetDynamicBufferPool()->GetResource(), sizeof(SceneLightingInformation), 3);
    m_pParameterSet->SetRootConstantBufferResource(GetDynamicBufferPool()->GetResource(), sizeof(LightingCBData), 4);

    ShadowMapResourcePool* pShadowMapResourcePool = GetFramework()->GetShadowMapResourcePool();
    for (uint32_t i = 0; i < pShadowMapResourcePool->GetRenderTargetCount(); ++i)
    {
        m_pParameterSet->SetTextureSRV(pShadowMapResourcePool->GetRenderTarget(i), ViewDimension::Texture2D, 503 + i);
    }

    
    // Register for content change updates
    GetContentManager()->AddContentListener(this);

    SetModuleReady(true);
}

ForwardRenderModule::~ForwardRenderModule()
{
    GetContentManager()->RemoveContentListener(this);

    delete m_pRootSignature;
    delete m_pParameterSet;

    // Clear out raster views
    m_RasterViews.clear();

    // Release pipeline objects and clear all mappings
    for (auto& pipelineGroup : m_PipelineRenderGroups)
    {
        CauldronAssert(ASSERT_ERROR, pipelineGroup.m_RenderSurfaces.empty(), L"Not all pipeline surfaces have been removed. This ship is leaking.");
        delete pipelineGroup.m_Pipeline;
    }

    // Release samplers
    for (auto samplerIter : m_Samplers)
        delete samplerIter;
    m_Samplers.clear();

    m_PipelineRenderGroups.clear();
}

void ForwardRenderModule::Execute(double deltaTime, CommandList* pCmdList)
{
    GPUScopedProfileCapture forwardMarker(pCmdList, L"Forward");

    if(GetScene()->GetBRDFLutTexture())
    {
        m_pParameterSet->SetTextureSRV(GetScene()->GetBRDFLutTexture(), ViewDimension::Texture2D, 500);
    }
    if(GetScene()->GetIBLTexture(IBLTexture::Irradiance))
    {
        m_pParameterSet->SetTextureSRV(GetScene()->GetIBLTexture(IBLTexture::Irradiance), ViewDimension::TextureCube, 501);
    }
    if(GetScene()->GetIBLTexture(IBLTexture::Prefiltered))
    {
        m_pParameterSet->SetTextureSRV(GetScene()->GetIBLTexture(IBLTexture::Prefiltered), ViewDimension::TextureCube, 502);
    }

    if (GetScene()->GetScreenSpaceShadowTexture())
    {
        // Store screenSpaceShadowTexture at index 0 in the shadow maps array
        m_pParameterSet->SetTextureSRV(GetScene()->GetScreenSpaceShadowTexture(), ViewDimension::Texture2D, 503);
    }
    else
    {
        ShadowMapResourcePool* pShadowMapResourcePool = GetFramework()->GetShadowMapResourcePool();
        if (pShadowMapResourcePool->GetRenderTargetCount() > 0)
        {
            CauldronAssert(ASSERT_CRITICAL,
                           pShadowMapResourcePool->GetRenderTargetCount() <= MAX_SHADOW_MAP_TEXTURES_COUNT,
                           L"Lighting Render Module can only support up to %d shadow maps. There are currently %d shadow maps",
                           MAX_SHADOW_MAP_TEXTURES_COUNT,
                           pShadowMapResourcePool->GetRenderTargetCount());
            for (uint32_t i = 0; i < pShadowMapResourcePool->GetRenderTargetCount(); ++i)
            {
                m_pParameterSet->SetTextureSRV(pShadowMapResourcePool->GetRenderTarget(i), ViewDimension::Texture2D, 503 + i);
            }
        }
    }

    // Render modules expect resources coming in/going out to be in a shader read state
    std::vector<Barrier> barriers;
    barriers.push_back(Barrier::Transition(m_pColorRenderTarget->GetResource(),        ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource, ResourceState::RenderTargetResource));
    barriers.push_back(Barrier::Transition(m_pDepthTarget->GetResource(),               ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource, ResourceState::DepthWrite));
    if (m_GenerateMotionVectors)
    {
        barriers.push_back(Barrier::Transition(m_pMotionVector->GetResource(), ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource, ResourceState::RenderTargetResource));
    }
    ResourceBarrier(pCmdList, static_cast<uint32_t>(barriers.size()), barriers.data());

    // Do clears
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    ClearRenderTarget(pCmdList, &m_RasterViews[0]->GetResourceView(), clearColor);
    if (m_GenerateMotionVectors)
    {
        ClearRenderTarget(pCmdList, &m_RasterViews[1]->GetResourceView(), clearColor);
    }
    ClearDepthStencil(pCmdList, &m_RasterViews[2]->GetResourceView(), 0);

    // Bind raster resources
    BeginRaster(pCmdList, m_GenerateMotionVectors ? 2 : 1, m_RasterViews.data(), m_RasterViews[2], m_VariableShading ? GetDevice()->GetVRSInfo() : nullptr);

    // Update necessary scene frame information
    // Update necessary scene frame information
    BufferAddressInfo sceneBuffers[2];
    sceneBuffers[0] = GetDynamicBufferPool()->AllocConstantBuffer(sizeof(SceneInformation), reinterpret_cast<const void*>(&GetScene()->GetSceneInfo()));
    sceneBuffers[1] = GetDynamicBufferPool()->AllocConstantBuffer(sizeof(SceneLightingInformation), reinterpret_cast<const void*>(&GetScene()->GetSceneLightInfo()));
    m_pParameterSet->UpdateRootConstantBuffer(&sceneBuffers[0], 0);
    m_pParameterSet->UpdateRootConstantBuffer(&sceneBuffers[1], 3);

    // Allocate a dynamic constant buffers and set
    m_LightingConstantData.IBLFactor = GetScene()->GetIBLFactor();
    m_LightingConstantData.SpecularIBLFactor = GetScene()->GetSpecularIBLFactor();
    BufferAddressInfo bufferInfo  = GetDynamicBufferPool()->AllocConstantBuffer(sizeof(LightingCBData), &m_LightingConstantData);

    // Update constant buffers
    m_pParameterSet->UpdateRootConstantBuffer(&bufferInfo, 4);

    // Set viewport, scissor, primitive topology once and move on (set based on upscaler state)
    UpscalerState upscaleState = GetFramework()->GetUpscalingState();
    const ResolutionInfo& resInfo = GetFramework()->GetResolutionInfo();

    uint32_t width, height;
    if (upscaleState == UpscalerState::None || upscaleState == UpscalerState::PostUpscale)
    {
        width = resInfo.UpscaleWidth;
        height = resInfo.UpscaleHeight;
    }
    else
    {
        width = resInfo.RenderWidth;
        height = resInfo.RenderHeight;
    }

    SetViewportScissorRect(pCmdList, 0, 0, width, height, 0.f, 1.f);
    SetPrimitiveTopology(pCmdList, PrimitiveTopology::TriangleList);

    //Early instantiate to prevent realloc in loops.
    std::vector<BufferAddressInfo> vertexBuffers;
    std::vector<BufferAddressInfo> perObjectBufferInfos;
    std::vector<BufferAddressInfo> textureIndicesBufferInfos;
    // Render all surfaces by pipeline groupings
    {
        std::lock_guard<std::mutex> paramsLock(m_CriticalSection);  // Can't change parameter set data while we are updating/binding for render
        for (auto& pipelineGroup : m_PipelineRenderGroups)
        {
            // Set the pipeline to use for all render calls
            SetPipelineState(pCmdList, pipelineGroup.m_Pipeline);

            uint32_t activeCount = 0;

            for (auto& pipelineSurfaceInfo : pipelineGroup.m_RenderSurfaces)
                if (pipelineSurfaceInfo.pOwner->IsActive())
                    activeCount++;

            perObjectBufferInfos.clear();
            perObjectBufferInfos.resize(activeCount);
            GetDynamicBufferPool()->BatchAllocateConstantBuffer(sizeof(InstanceInformation), activeCount, perObjectBufferInfos.data());
            textureIndicesBufferInfos.clear();
            textureIndicesBufferInfos.resize(activeCount);
            GetDynamicBufferPool()->BatchAllocateConstantBuffer(sizeof(TextureIndices), activeCount, textureIndicesBufferInfos.data());
            uint32_t currentSurface = 0;


            for (auto& pipelineSurfaceInfo : pipelineGroup.m_RenderSurfaces)
            {
                // Make sure owner is active
                if (pipelineSurfaceInfo.pOwner->IsActive())
                {
                    // NOTE - We should enforce no scaling on transforms as we don't support scaled matrix transforms in the shader
                    InstanceInformation instanceInfo;
                    instanceInfo.WorldTransform = pipelineSurfaceInfo.pOwner->GetTransform();
                    instanceInfo.PrevWorldTransform = pipelineSurfaceInfo.pOwner->GetPrevTransform();

                    instanceInfo.MaterialInfo.EmissiveFactor = Vec4(0.0f, 0.0f, 0.0f, 0.0f);
                    instanceInfo.MaterialInfo.AlbedoFactor = Vec4(1.0f, 1.0f, 1.0f, 1.0f);
                    instanceInfo.MaterialInfo.PBRParams = Vec4(0.0f, 0.0f, 0.0f, 0.0f);

                    const Surface* pSurface = pipelineSurfaceInfo.pSurface;
                    const Material* pMaterial = pSurface->GetMaterial();

                    instanceInfo.MaterialInfo.AlphaCutoff = pMaterial->GetAlphaCutOff();

                    // update the perObjectConstantData
                    if (pMaterial->HasPBRInfo())
                    {
                        instanceInfo.MaterialInfo.EmissiveFactor = pMaterial->GetEmissiveColor();

                        Vec4 albedo = pMaterial->GetAlbedoColor();
                        instanceInfo.MaterialInfo.AlbedoFactor = albedo;

                        if (pMaterial->HasPBRMetalRough() || pMaterial->HasPBRSpecGloss())
                            instanceInfo.MaterialInfo.PBRParams = pMaterial->GetPBRInfo();
                    }

                    // Update root constants
                    BufferAddressInfo& perObjectBufferInfo = perObjectBufferInfos[currentSurface];
                    GetDynamicBufferPool()->InitializeConstantBuffer(perObjectBufferInfo, sizeof(InstanceInformation), &instanceInfo);

                    BufferAddressInfo& textureIndicesBufferInfo = textureIndicesBufferInfos[currentSurface];
                    GetDynamicBufferPool()->InitializeConstantBuffer(textureIndicesBufferInfo, sizeof(TextureIndices), &pipelineSurfaceInfo.TextureIndices);

                    currentSurface++;

                    m_pParameterSet->UpdateRootConstantBuffer(&perObjectBufferInfo, 1);
                    m_pParameterSet->UpdateRootConstantBuffer(&textureIndicesBufferInfo, 2);


                    // Bind for rendering
                    m_pParameterSet->Bind(pCmdList, pipelineGroup.m_Pipeline);

                    vertexBuffers.clear();
                    for (uint32_t attribute = 0; attribute < static_cast<uint32_t>(VertexAttributeType::Count); ++attribute)
                    {
                        // Check if the attribute is present
                        if (pipelineGroup.m_UsedAttributes & (0x1 << attribute))
                        {
                            vertexBuffers.emplace_back(pSurface->GetVertexBuffer(static_cast<VertexAttributeType>(attribute)).pBuffer->GetAddressInfo());
                        }
                    }

                    // Skeletal Animation
                    if (pipelineSurfaceInfo.pOwner->HasComponent(AnimationComponentMgr::Get()))
                    {
                        const auto& data = pipelineSurfaceInfo.pOwner->GetComponent<const AnimationComponent>(AnimationComponentMgr::Get())->GetData();

                        if (data->m_skinId != -1)
                        {
                            // Positions are stored at index 0
                            // Normals are stored at index 1

                            // Replace the vertices POSITION attribute with the Skinned POSITION attribute
                            // Replace the vertices NORMAL   attribute with the Skinned NORMAL   attribute
                            // Replace the vertices PREVIOUSPOSITION   attribute with the Skinned PREVIOUSPOSITION attribute
                            const uint32_t surfaceID = pSurface->GetSurfaceID();
                            vertexBuffers[0]     = data->m_skinnedPositions[surfaceID].pBuffer->GetAddressInfo();
                            vertexBuffers[1]     = data->m_skinnedNormals[surfaceID].pBuffer->GetAddressInfo();
                            vertexBuffers.back() = data->m_skinnedPreviousPosition[surfaceID].pBuffer->GetAddressInfo();
                        }
                    }

                    // Set vertex/index buffers
                    SetVertexBuffers(pCmdList, 0, static_cast<uint32_t>(vertexBuffers.size()), vertexBuffers.data());

                    BufferAddressInfo addressInfo = pSurface->GetIndexBuffer().pBuffer->GetAddressInfo();
                    SetIndexBuffer(pCmdList, &addressInfo);

                    // And draw
                    DrawIndexedInstanced(pCmdList, pSurface->GetIndexBuffer().Count);
                }
            }
        }
    }

    // Done drawing, unbind
    EndRaster(pCmdList, m_VariableShading ? GetDevice()->GetVRSInfo() : nullptr);

    // Render modules expect resources coming in/going out to be in a shader read state
    barriers.clear();
    barriers.push_back(Barrier::Transition(m_pColorRenderTarget->GetResource(),        ResourceState::RenderTargetResource,    ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource));
    barriers.push_back(Barrier::Transition(m_pDepthTarget->GetResource(),               ResourceState::DepthWrite,              ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource));
    ResourceBarrier(pCmdList, static_cast<uint32_t>(barriers.size()), barriers.data());
}

void ForwardRenderModule::OnNewContentLoaded(ContentBlock* pContentBlock)
{
    MeshComponentMgr* pMeshComponentManager = MeshComponentMgr::Get();

    std::lock_guard<std::mutex> pipelineLock(m_CriticalSection);

    // For each new Mesh, create a SurfaceRenderingInfo that will map mesh/material information for more efficient rendering at run time
    for (auto* pEntityData : pContentBlock->EntityDataBlocks)
    {
        for (auto* pComponent : pEntityData->Components)
        {
            if (pComponent->GetManager() == pMeshComponentManager)
            {
                const Mesh* pMesh = reinterpret_cast<MeshComponent*>(pComponent)->GetData().pMesh;
                const size_t numSurfaces = pMesh->GetNumSurfaces();
                for (uint32_t i = 0; i < numSurfaces; ++i)
                {
                    const Surface* pSurface = pMesh->GetSurface(i);
                    const Material* pMaterial = pSurface->GetMaterial();

                    // Forward only handles opaques, so skip this surface if it's got any translucency
                    if (pSurface->HasTranslucency())
                        continue;

                    // Push surface render information
                    PipelineSurfaceRenderInfo surfaceRenderInfo;
                    surfaceRenderInfo.pOwner = pComponent->GetOwner();
                    surfaceRenderInfo.pSurface = pSurface;

                    int32_t samplerIndex;
                    if (pMaterial->HasPBRInfo())
                    {
                        surfaceRenderInfo.TextureIndices.AlbedoTextureIndex = AddTexture(pMaterial, TextureClass::Albedo, samplerIndex);
                        surfaceRenderInfo.TextureIndices.AlbedoSamplerIndex = samplerIndex;
                        if (pMaterial->HasPBRMetalRough())
                        {
                            surfaceRenderInfo.TextureIndices.MetalRoughSpecGlossTextureIndex = AddTexture(pMaterial, TextureClass::MetalRough, samplerIndex);
                            surfaceRenderInfo.TextureIndices.MetalRoughSpecGlossSamplerIndex = samplerIndex;
                        }
                        else if (pMaterial->HasPBRSpecGloss())
                        {
                            surfaceRenderInfo.TextureIndices.MetalRoughSpecGlossTextureIndex = AddTexture(pMaterial, TextureClass::SpecGloss, samplerIndex);
                            surfaceRenderInfo.TextureIndices.MetalRoughSpecGlossSamplerIndex = samplerIndex;
                        }
                    }

                    surfaceRenderInfo.TextureIndices.NormalTextureIndex = AddTexture(pMaterial, TextureClass::Normal, samplerIndex);
                    surfaceRenderInfo.TextureIndices.NormalSamplerIndex = samplerIndex;
                    surfaceRenderInfo.TextureIndices.EmissiveTextureIndex = AddTexture(pMaterial, TextureClass::Emissive, samplerIndex);
                    surfaceRenderInfo.TextureIndices.EmissiveSamplerIndex = samplerIndex;
                    surfaceRenderInfo.TextureIndices.OcclusionTextureIndex = AddTexture(pMaterial, TextureClass::Occlusion, samplerIndex);
                    surfaceRenderInfo.TextureIndices.OcclusionSamplerIndex = samplerIndex;

                    // Assign to the correct pipeline render group (will create a new pipeline group if needed)
                    m_PipelineRenderGroups[GetPipelinePermutationID(pSurface)].m_RenderSurfaces.push_back(surfaceRenderInfo);
                }
            }
        }
    }

    {
        // Update the parameter set with loaded texture entries
        CauldronAssert(ASSERT_CRITICAL, m_Textures.size() <= (MAX_TEXTURES_COUNT / 2), L"Too many textures.");
        for (uint32_t i = 0; i < m_Textures.size(); ++i)
            m_pParameterSet->SetTextureSRV(m_Textures[i].pTexture, ViewDimension::Texture2D, i);

        // Update sampler bindings as well
        CauldronAssert(ASSERT_CRITICAL, m_Samplers.size() <= MAX_SAMPLERS_COUNT, L"Too many samplers.");
        for (uint32_t i = 0; i < m_Samplers.size(); ++i)
            m_pParameterSet->SetSampler(m_Samplers[i], i);
    }
}

void ForwardRenderModule::OnContentUnloaded(ContentBlock* pContentBlock)
{
    for (auto* pEntityData : pContentBlock->EntityDataBlocks)
    {
        for (auto* pComponent : pEntityData->Components)
        {
            if (pComponent->GetManager() == MeshComponentMgr::Get())
            {
                const Mesh* pMesh = reinterpret_cast<MeshComponent*>(pComponent)->GetData().pMesh;
                const Entity* pOwner = pComponent->GetOwner();

                const size_t numSurfaces = pMesh->GetNumSurfaces();
                for (uint32_t i = 0; i < numSurfaces; ++i)
                {
                    const Surface* pSurface = pMesh->GetSurface(i);

                    // We're going to be modifying the pipeline groups, so make sure no one else is using them
                    std::lock_guard<std::mutex> pipelineLock(m_CriticalSection);

                    // Find what list the surface is in
                    for (auto& pipelineGroup : m_PipelineRenderGroups)
                    {
                        bool surfaceFound = false;
                        for (auto& surfaceItr = pipelineGroup.m_RenderSurfaces.begin(); surfaceItr != pipelineGroup.m_RenderSurfaces.end(); ++surfaceItr)
                        {
                            if (surfaceItr->pOwner == pOwner && surfaceItr->pSurface == pSurface)
                            {
                                // Don't keep looking
                                surfaceFound = true;

                                // Remove the texture entries
                                RemoveTexture(surfaceItr->TextureIndices.AlbedoTextureIndex);
                                RemoveTexture(surfaceItr->TextureIndices.MetalRoughSpecGlossTextureIndex);
                                RemoveTexture(surfaceItr->TextureIndices.NormalTextureIndex);
                                RemoveTexture(surfaceItr->TextureIndices.EmissiveTextureIndex);
                                RemoveTexture(surfaceItr->TextureIndices.OcclusionTextureIndex);

                                // Remove it from the list
                                pipelineGroup.m_RenderSurfaces.erase(surfaceItr);
                                break;
                            }
                        }

                        // Don't need to check any other pipeline groups if we've already found it
                        if (surfaceFound)
                            break;
                    }
                }
            }
        }
    }
}

//////////////////////////////////////////////////////////////////////////
// Content loading helpers

uint32_t ForwardRenderModule::GetPipelinePermutationID(const Surface* pSurface) // uint32_t vertexAttributeFlags, const Material* pMaterial)
{
    // Forward shader should be optimized based on what the model provides
    //   - The used attributes are AT MOST what the model has
    //   - Material model (metallic+roughhness or specular+glossiness) is a material property but it doesn't guarantee that all the data is available
    //   - some textures can be missing, hence are not in the define list
    //   - when some textures are missing, texcoord attributes can be removed.
    //     - POSITION have to be present
    //     - NORMAL, TANGENT and COLOR# are always used if present
    //     - TEXCOORD# depends on which textures are using them. If there is no texture, they should be removed
    //     - PREVIOUSPOSITION for meshes that support skeletal animation

    uint32_t usedAttributes = VertexAttributeFlag_Position | VertexAttributeFlag_Normal | VertexAttributeFlag_Tangent | VertexAttributeFlag_Color0 | VertexAttributeFlag_Color1 | VertexAttributeFlag_PreviousPosition;

    // only keep the available attributes of the surface
    const uint32_t surfaceAttributes = pSurface->GetVertexAttributes();
    usedAttributes = usedAttributes & surfaceAttributes;

    DefineList defineList;
    const Material* pMaterial = pSurface->GetMaterial();

    // defines in the shaders

    // ID_normalTexCoord
    // ID_emissiveTexCoord
    // ID_occlusionTexCoord
    // ID_albedoTexCoord
    // ID_metallicRoughnessTexCoord

    // ID_normalTexture
    // ID_emissiveTexture
    // ID_occlusionTexture
    // ID_albedoTexture
    // ID_metallicRoughnessTexture

    if (m_GenerateMotionVectors)  
    {
        defineList.insert(std::make_pair(L"HAS_MOTION_VECTORS",    L"1"));
        defineList.insert(std::make_pair(L"HAS_MOTION_VECTORS_RT", L"3"));
    }

    if (pMaterial->HasPBRInfo())
    {
        if (pMaterial->HasPBRMetalRough())
        {
            defineList.insert(std::make_pair(L"MATERIAL_METALLICROUGHNESS", L""));
            AddTextureToDefineList(defineList, usedAttributes, surfaceAttributes, pMaterial, TextureClass::Albedo, L"ID_albedoTexture", L"ID_albedoTexCoord");
            AddTextureToDefineList(defineList, usedAttributes, surfaceAttributes, pMaterial, TextureClass::MetalRough, L"ID_metallicRoughnessTexture", L"ID_metallicRoughnessTexCoord");
        }
        else if (pMaterial->HasPBRSpecGloss())
        {
            defineList.insert(std::make_pair(L"MATERIAL_SPECULARGLOSSINESS", L""));
            AddTextureToDefineList(defineList, usedAttributes, surfaceAttributes, pMaterial, TextureClass::Albedo, L"ID_albedoTexture", L"ID_albedoTexCoord");
            AddTextureToDefineList(defineList, usedAttributes, surfaceAttributes, pMaterial, TextureClass::SpecGloss, L"ID_specularGlossinessTexture", L"ID_specularGlossinessTexCoord");
        }
    }
    AddTextureToDefineList(defineList, usedAttributes, surfaceAttributes, pMaterial, TextureClass::Normal, L"ID_normalTexture", L"ID_normalTexCoord");
    AddTextureToDefineList(defineList, usedAttributes, surfaceAttributes, pMaterial, TextureClass::Emissive, L"ID_emissiveTexture", L"ID_emissiveTexCoord");
    AddTextureToDefineList(defineList, usedAttributes, surfaceAttributes, pMaterial, TextureClass::Occlusion, L"ID_occlusionTexture", L"ID_occlusionTexCoord");

    if (pMaterial->HasDoubleSided())
        defineList.insert(std::make_pair(L"ID_doublesided", L""));

    if (pMaterial->GetBlendMode() == MaterialBlend::Mask)
        defineList.insert(std::make_pair(L"ID_alphaMask", L""));

    // Get the defines for attributes that make up the surface vertices
    Surface::GetVertexAttributeDefines(usedAttributes, defineList);

    // compute hash
    uint64_t hash = static_cast<uint64_t>(Hash(defineList, usedAttributes, pSurface));

    // See if we've already built this pipeline
    for (uint32_t i = 0; i < m_PipelineRenderGroups.size(); ++i)
    {
        if (m_PipelineRenderGroups[i].m_PipelineHash == hash)
            return i;
    }

    // If we didn't find the pipeline already, create a new one

    // Setup the pipeline object
    PipelineDesc psoDesc;
    psoDesc.SetRootSignature(m_pRootSignature);

    // Setup the shaders to build on the pipeline object
    psoDesc.AddShaderDesc(ShaderBuildDesc::Vertex(L"transformVS.hlsl", L"MainVS", ShaderModel::SM6_0, &defineList));
    psoDesc.AddShaderDesc(ShaderBuildDesc::Pixel (L"forwardps.hlsl", L"MainPS", ShaderModel::SM6_0, &defineList));

    // Setup remaining information and build
    psoDesc.AddPrimitiveTopology(PrimitiveTopologyType::Triangle);
    std::vector<ResourceFormat> rtFormats = {
        m_pColorRenderTarget->GetFormat() };
    if (m_GenerateMotionVectors)
    {
        rtFormats.push_back(m_pMotionVector->GetFormat());
    }
    psoDesc.AddRasterFormats(rtFormats, m_pDepthTarget->GetFormat());

    RasterDesc rasterDesc;
    rasterDesc.CullingMode = pMaterial->HasDoubleSided() ? CullMode::None : CullMode::Front;
    psoDesc.AddRasterStateDescription(&rasterDesc);

    // Set input layout
    std::vector<InputLayoutDesc> vertexAttributes;
    for (uint32_t attribute = 0; attribute < static_cast<uint32_t>(VertexAttributeType::Count); ++attribute)
    {
        // Check if the attribute is present
        if (usedAttributes & (0x1 << attribute))
            vertexAttributes.push_back(InputLayoutDesc(static_cast<VertexAttributeType>(attribute), pSurface->GetVertexBuffer(static_cast<VertexAttributeType>(attribute)).ResourceDataFormat, (uint32_t)vertexAttributes.size(), 0));
    }
    psoDesc.AddInputLayout(vertexAttributes);

    DepthDesc depthDesc;
    depthDesc.DepthEnable = true;
    depthDesc.StencilEnable = false;

    depthDesc.DepthWriteEnable = true;
    depthDesc.DepthFunc = ComparisonFunc::Less;

    psoDesc.AddDepthState(&depthDesc);

    PipelineObject* pPipelineObj = PipelineObject::CreatePipelineObject(L"ForwardRenderPass_PipelineObj", psoDesc);

    // Ok, this is a new pipeline, setup a new PipelineRenderGroup for it
    PipelineRenderGroup pipelineGroup;
    pipelineGroup.m_Pipeline = pPipelineObj;
    pipelineGroup.m_PipelineHash = hash;
    pipelineGroup.m_UsedAttributes = usedAttributes;
    m_PipelineRenderGroups.push_back(pipelineGroup);

    return static_cast<uint32_t>(m_PipelineRenderGroups.size() - 1);
}

// Add texture index info and return the index to the texture in the texture array
int32_t ForwardRenderModule::AddTexture(const Material* pMaterial, const TextureClass textureClass, int32_t& textureSamplerIndex)
{
    const cauldron::TextureInfo* pTextureInfo = pMaterial->GetTextureInfo(textureClass);
    if (pTextureInfo != nullptr)
    {
        // Check if the texture's sampler is already one we have, and if not add it
        for (textureSamplerIndex = 0; textureSamplerIndex < m_Samplers.size(); ++textureSamplerIndex)
        {
            if (m_Samplers[textureSamplerIndex]->GetDesc() == pTextureInfo->TexSamplerDesc)
                break; // found
        }

        // If we didn't find the sampler, add it
        if (textureSamplerIndex == m_Samplers.size())
        {
            Sampler* pSampler = Sampler::CreateSampler(L"ForwardSampler", pTextureInfo->TexSamplerDesc);
            CauldronAssert(ASSERT_WARNING, pSampler, L"Could not create sampler for loaded content %ls", pTextureInfo->pTexture->GetDesc().Name.c_str());
            m_Samplers.push_back(pSampler);
        }

        // Find a slot for the texture
        int32_t firstFreeIndex = -1;
        for (int32_t i = 0; i < m_Textures.size(); ++i)
        {
            BoundTexture& boundTexture = m_Textures[i];

            // If this texture is already mapped, bump it's reference count
            if (pTextureInfo->pTexture == boundTexture.pTexture)
            {
                boundTexture.count += 1;
                return i;
            }

            // Try to re-use an existing entry that was released
            else if (firstFreeIndex < 0  && boundTexture.count == 0)
            {
                firstFreeIndex = i;
            }
        }

        // Texture wasn't found
        BoundTexture b = { pTextureInfo->pTexture, 1 };
        if (firstFreeIndex < 0)
        {
            m_Textures.push_back(b);
            return static_cast<int32_t>(m_Textures.size()) - 1;
        }
        else
        {
            m_Textures[firstFreeIndex] = b;
            return firstFreeIndex;
        }
    }
    return -1;
}

void ForwardRenderModule::RemoveTexture(int32_t index)
{
    if (index >= 0)
    {
        m_Textures[index].count -= 1;
        if (m_Textures[index].count == 0)
        {
            m_Textures[index].pTexture = nullptr;
        }
    }
}
