/*
* All or portions of this file Copyright (c) Amazon.com, Inc. or its affiliates or
* its licensors.
*
* For complete copyright and license terms please see the LICENSE at the root of this
* distribution (the "License"). All use of this software is governed by the License,
* or, if provided, by the license below or the license accompanying this file. Do not
* remove or modify any license notices. This file is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*
*/
// Original file Copyright Crytek GMBH or its affiliates, used under license.

#include "StdAfx.h"

#if defined(FEATURE_SVO_GI)

#include "DriverD3D.h"
#include "I3DEngine.h"
#include "D3DPostProcess.h"
#include "D3D_SVO.h"
#include "D3DTiledShading.h"
#include "../Common/Include_HLSL_CPP_Shared.h"
#include "../Common/TypedConstantBuffer.h"

CSvoRenderer* CSvoRenderer::s_pInstance = 0;

CSvoRenderer::CSvoRenderer()
{
    InitCVarValues();

#ifdef FEATURE_SVO_GI_ALLOW_HQ
    m_pRT_AIR_MIN =
        m_pRT_AIR_MAX =
            m_pRT_AIR_SHAD =
                m_pRT_NID_0 = NULL;
#endif

    m_nTexStateTrilinear = CTexture::GetTexState(STexState(FILTER_TRILINEAR, true));
    m_nTexStateLinear = CTexture::GetTexState(STexState(FILTER_LINEAR, true));
    m_nTexStatePoint = CTexture::GetTexState(STexState(FILTER_POINT, true));
    m_nTexStateLinearWrap = CTexture::GetTexState(STexState(FILTER_LINEAR, false));

    m_pNoiseTex = CTexture::ForName("EngineAssets/Textures/noise.dds",  FT_DONT_STREAM, eTF_Unknown);
    m_pRsmNormlMap = m_pRsmColorMap = 0;
    m_pRsmPoolNor = m_pRsmPoolCol = 0;

    m_pShader = CShaderMan::s_ShaderSVOGI;

    ZeroStruct(m_arrNodesForUpdate);
    ZeroStruct(m_nCurPropagationPassID);
}

void SSvoTargetsSet::Release()
{
    SAFE_RELEASE(pRT_RGB_DEM_MIN_0);
    SAFE_RELEASE(pRT_ALD_DEM_MIN_0);
    SAFE_RELEASE(pRT_RGB_DEM_MAX_0);
    SAFE_RELEASE(pRT_ALD_DEM_MAX_0);
    SAFE_RELEASE(pRT_RGB_DEM_MIN_1);
    SAFE_RELEASE(pRT_ALD_DEM_MIN_1);
    SAFE_RELEASE(pRT_RGB_DEM_MAX_1);
    SAFE_RELEASE(pRT_ALD_DEM_MAX_1);
    SAFE_RELEASE(pRT_FIN_OUT_0);
    SAFE_RELEASE(pRT_FIN_OUT_1);
    SAFE_RELEASE(pRT_ALD_0);
    SAFE_RELEASE(pRT_ALD_1);
    SAFE_RELEASE(pRT_RGB_0);
    SAFE_RELEASE(pRT_RGB_1);
}

CSvoRenderer::~CSvoRenderer()
{
    m_tsDiff.Release();
    m_tsSpec.Release();

#ifdef FEATURE_SVO_GI_ALLOW_HQ
    SAFE_RELEASE(m_pRT_AIR_MIN);
    SAFE_RELEASE(m_pRT_AIR_MAX);
    SAFE_RELEASE(m_pRT_AIR_SHAD);
    SAFE_RELEASE(m_pRT_NID_0);
#endif

    SAFE_RELEASE(m_pRsmColorMap);
    SAFE_RELEASE(m_pRsmNormlMap);
    SAFE_RELEASE(m_pRsmPoolCol);
    SAFE_RELEASE(m_pRsmPoolNor);
}

CSvoRenderer* CSvoRenderer::GetInstance(bool bCheckAlloce)
{
    if (!s_pInstance && bCheckAlloce)
    {
        s_pInstance = new CSvoRenderer();
    }

    return s_pInstance;
}

void CSvoRenderer::Release()
{
    SAFE_DELETE(s_pInstance);
}

void CSvoRenderer::SetEditingHelper(const Sphere& sp)
{
    m_texInfo.helperInfo = sp;
}

void CSvoRenderer::UpdateCompute()
{
    InitCVarValues();

    if (!e_svoEnabled)
    {
        return;
    }

    UpdatePassConstantBuffer();

    if (!e_svoRender)
    {
        return;
    }

    static int nTI_Compute_FrameId = -1;
    if (nTI_Compute_FrameId == gRenDev->GetFrameID(false))
    {
        return;
    }
    nTI_Compute_FrameId = gRenDev->GetFrameID(false);

    if (!gEnv->p3DEngine->GetSvoStaticTextures(m_texInfo, &m_arrLightsStatic, &m_arrLightsDynamic))
    {
        return;
    }

    CD3D9Renderer* rd = gcpRendD3D;

    if (!m_pShader)
    {
        gcpRendD3D->m_cEF.mfRefreshSystemShader("Total_Illumination",   m_pShader);
    }

    m_arrNodesForUpdateIncr.Clear();
    m_arrNodesForUpdateNear.Clear();

#ifdef FEATURE_SVO_GI_ALLOW_HQ

    if (GetIntegratioMode())
    {
        gEnv->p3DEngine->GetSvoBricksForUpdate(m_arrNodeInfo, (float)0, &m_arrVerts);

        for (int n = 0; n < m_arrNodeInfo.Count(); n++)
        {
            m_arrNodesForUpdateIncr.Add(m_arrNodeInfo[n]);
        }

        m_arrNodeInfo.Clear();

        if (e_svoTI_DynLights)
        {
            gEnv->p3DEngine->GetSvoBricksForUpdate(m_arrNodeInfo, e_svoMinNodeSize, 0);

            for (int n = 0; n < m_arrNodeInfo.Count(); n++)
            {
                m_arrNodesForUpdateNear.Add(m_arrNodeInfo[n]);
            }
        }
    }

    {
        // get UAV access
        vp_RGB0.Init(m_texInfo.pTexRgb0);
        vp_RGB1.Init(m_texInfo.pTexRgb1);
        vp_DYNL.Init(m_texInfo.pTexDynl);
        vp_RGB2.Init(m_texInfo.pTexRgb2);
        vp_RGB3.Init(m_texInfo.pTexRgb3);
        vp_RGB4.Init(m_texInfo.pTexRgb4);
        vp_NORM.Init(m_texInfo.pTexNorm);
        vp_ALDI.Init(m_texInfo.pTexAldi);
        vp_OPAC.Init(m_texInfo.pTexOpac);
    }

    if ((!(e_svoTI_Active && e_svoTI_Apply) && !e_svoDVR) || !m_texInfo.bSvoReady || !GetIntegratioMode())
    {
        return;
    }

    // force sync shaders compiling
    int nPrevAsync = CRenderer::CV_r_shadersasynccompiling;
    CRenderer::CV_r_shadersasynccompiling = 0;

    // clear pass
    {
        PROFILE_LABEL_SCOPE("TI_INJECT_CLEAR");

        for (int nNodesForUpdateStartIndex = 0; nNodesForUpdateStartIndex < m_arrNodesForUpdateIncr.Count(); )
        {
            ExecuteComputeShader(m_pShader, "ComputeClearBricks", eCS_ClearBricks, &nNodesForUpdateStartIndex, 0, m_arrNodesForUpdateIncr);
        }
    }

    if (e_svoTI_Troposphere_Active && (e_svoTI_Troposphere_CloudGen_Freq || e_svoTI_Troposphere_Layer0_Dens || e_svoTI_Troposphere_Layer1_Dens))
    {
        PROFILE_LABEL_SCOPE("TI_INJECT_AIR");

        for (int nNodesForUpdateStartIndex = 0; nNodesForUpdateStartIndex < m_arrNodesForUpdateIncr.Count(); )
        {
            ExecuteComputeShader(m_pShader, "ComputeInjectAtmosphere", eCS_InjectAirOpacity, &nNodesForUpdateStartIndex, 0, m_arrNodesForUpdateIncr);
        }
    }

    {
        PROFILE_LABEL_SCOPE("TI_INJECT_LIGHT");

        for (int nNodesForUpdateStartIndex = 0; nNodesForUpdateStartIndex < m_arrNodesForUpdateIncr.Count(); )
        {
            ExecuteComputeShader(m_pShader, "ComputeDirectStaticLighting", eCS_InjectStaticLights, &nNodesForUpdateStartIndex, 0, m_arrNodesForUpdateIncr);
        }
    }

    if (e_svoTI_PropagationBooster || e_svoTI_InjectionMultiplier)
    {
        if (e_svoTI_NumberOfBounces > 1)
        {
            PROFILE_LABEL_SCOPE("TI_INJECT_REFL0");

            m_nCurPropagationPassID = 0;
            for (int nNodesForUpdateStartIndex = 0; nNodesForUpdateStartIndex < m_arrNodesForUpdateIncr.Count(); )
            {
                ExecuteComputeShader(m_pShader, "ComputePropagateLighting", eCS_PropagateLighting_1to2, &nNodesForUpdateStartIndex, 0, m_arrNodesForUpdateIncr);
            }
        }

        if (e_svoTI_NumberOfBounces > 2)
        {
            PROFILE_LABEL_SCOPE("TI_INJECT_REFL1");

            m_nCurPropagationPassID++;
            for (int nNodesForUpdateStartIndex = 0; nNodesForUpdateStartIndex < m_arrNodesForUpdateIncr.Count(); )
            {
                ExecuteComputeShader(m_pShader, "ComputePropagateLighting", eCS_PropagateLighting_2to3, &nNodesForUpdateStartIndex, 0, m_arrNodesForUpdateIncr);
            }
        }
    }

    static int nLightsDynamicCountPrevFrame = 0;

    if ((e_svoTI_DynLights && (m_arrLightsDynamic.Count() || nLightsDynamicCountPrevFrame)) || e_svoTI_SunRSMInject)
    {
        PROFILE_LABEL_SCOPE("TI_INJECT_DYNL");

        // TODO: cull not affected nodes

        for (int nNodesForUpdateStartIndex = 0; nNodesForUpdateStartIndex < m_arrNodesForUpdateNear.Count(); )
        {
            ExecuteComputeShader(m_pShader, "ComputeDirectDynamicLighting", eCS_InjectDynamicLights, &nNodesForUpdateStartIndex, 0, m_arrNodesForUpdateNear);
        }
    }

    nLightsDynamicCountPrevFrame = m_arrLightsDynamic.Count();

    CRenderer::CV_r_shadersasynccompiling = nPrevAsync;

#endif
}

void CSvoRenderer::ExecuteComputeShader(CShader* pSH, const char* szTechFinalName, EComputeStages eRenderStage, int* pnNodesForUpdateStartIndex, int nObjPassId, PodArray<I3DEngine::SSvoNodeInfo>& arrNodesForUpdate)
{
#ifdef FEATURE_SVO_GI_ALLOW_HQ

    FUNCTION_PROFILER_RENDERER;

    CD3D9Renderer* const __restrict rd = gcpRendD3D;

    SetShaderFlags(true, false);

    SD3DPostEffectsUtils::ShBeginPass(pSH, szTechFinalName, FEF_DONTSETSTATES);

    if (!gRenDev->m_RP.m_pShader)
    {
        gEnv->pLog->LogWarning("%s: Technique not found: %s", __FUNC__, szTechFinalName);
        (*pnNodesForUpdateStartIndex) += 1000;
        return;
    }

    {
        static CCryNameR param("SVO_PropagationPass");
        Vec4 value((float)(m_nCurPropagationPassID), 0, 0, 0);
        m_pShader->FXSetCSFloat(param, (Vec4*)&value, 1);
    }

    {
        static CCryNameR parameterName6("SVO_FrameIdByte");
        Vec4 ttt((float)(rd->GetFrameID(false) & 255), (float)rd->GetFrameID(false), 3, 4);
        if (rd->GetActiveGPUCount() > 1)
        {
            ttt.x = 0;
        }
        m_pShader->FXSetCSFloat(parameterName6, (Vec4*)&ttt, 1);
    }

    {
        static CCryNameR parameterName6("SVO_CamPos");
        Vec4 ttt(gEnv->pSystem->GetViewCamera().GetPosition(), 0);
        m_pShader->FXSetCSFloat(parameterName6, (Vec4*)&ttt, 1);
    }

    {
        Matrix44A mViewProj;
        mViewProj = gcpRendD3D->m_ViewProjMatrix;
        mViewProj.Transpose();

        static CCryNameR paramName("g_mViewProj");
        m_pShader->FXSetCSFloat(paramName, alias_cast<Vec4*>(&mViewProj), 4);

        static CCryNameR paramNamePrev("g_mViewProjPrev");
        m_pShader->FXSetPSFloat(paramNamePrev, alias_cast<Vec4*>(&m_matViewProjPrev), 4);
    }

    {
        const CameraViewParameters& rc = gcpRendD3D->GetViewParameters();
        float zn = rc.fNear;
        float zf = rc.fFar;
        float hfov = gcpRendD3D->GetCamera().GetHorizontalFov();
        Vec4 f;
        f[0] = zf / (zf - zn);
        f[1] = zn / (zn - zf);
        f[2] = 1.0f / hfov;
        f[3] = 1.0f;
        static CCryNameR paramName("SVO_ProjRatio");
        m_pShader->FXSetCSFloat(paramName, alias_cast<Vec4*>(&f), 1);
    }

    {
        static CCryNameR nameSVO_PortalsPos("SVO_PortalsPos");
        SetShaderFloat(eHWSC_Compute, nameSVO_PortalsPos, (Vec4*)&m_texInfo.arrPortalsPos[0], SVO_MAX_PORTALS);
        static CCryNameR nameSVO_PortalsDir("SVO_PortalsDir");
        SetShaderFloat(eHWSC_Compute, nameSVO_PortalsDir, (Vec4*)&m_texInfo.arrPortalsDir[0], SVO_MAX_PORTALS);
    }

    {
        static CCryNameR nameSVO_Reserved("SVO_Reserved");
        Vec4 vReserved(e_svoTI_Reserved0, e_svoTI_Reserved1, e_svoTI_Reserved2, e_svoTI_Reserved3);
        SetShaderFloat(eHWSC_Compute, nameSVO_Reserved, (Vec4*)&vReserved, 1);
    }

    // setup SVO textures

    ID3D11DeviceContext* pDeviceCtx = &gcpRendD3D->GetDeviceContext();
    UINT UAVInitialCounts   = 0;

    if (eRenderStage == eCS_InjectAirOpacity)
    {
        SetupLightSources(m_arrLightsStatic, m_pShader, false);
        SetupNodesForUpdate(*pnNodesForUpdateStartIndex, arrNodesForUpdate);

        // update OPAC
        pDeviceCtx->CSSetUnorderedAccessViews(2, 1, &vp_RGB0.pUAV, &UAVInitialCounts);
        pDeviceCtx->CSSetUnorderedAccessViews(7, 1, &vp_OPAC.pUAV, &UAVInitialCounts);

        SetupSvoTexturesForRead(m_texInfo, eHWSC_Compute, 0, 1);

        // bind fail fix
        CTexture::GetByID(vp_RGB0.nTexId)->Unbind();
        CTexture::GetByID(vp_OPAC.nTexId)->Unbind();

        m_pNoiseTex->Apply(15, m_nTexStateLinearWrap, -1, -1, -1, eHWSC_Compute);
    }
    else if (eRenderStage == eCS_InjectStaticLights)
    {
        SetupLightSources(m_arrLightsStatic, m_pShader, false);
        SetupNodesForUpdate(*pnNodesForUpdateStartIndex, arrNodesForUpdate);

        // update RGB1
        pDeviceCtx->CSSetUnorderedAccessViews(2, 1, &vp_RGB0.pUAV, &UAVInitialCounts);
        pDeviceCtx->CSSetUnorderedAccessViews(7, 1, &vp_RGB1.pUAV, &UAVInitialCounts);
        pDeviceCtx->CSSetUnorderedAccessViews(5, 1, &vp_NORM.pUAV, &UAVInitialCounts);

        if (vp_DYNL.pUAV)
        {
            pDeviceCtx->CSSetUnorderedAccessViews(6, 1, &vp_DYNL.pUAV, &UAVInitialCounts);
        }

        SetupSvoTexturesForRead(m_texInfo, eHWSC_Compute, 0);

        SetupRsmSun(eHWSC_Compute);

        //  m_pNoiseTex->Apply(15, m_nTexStateLinearWrap, -1, -1, -1, eHWSC_Compute);
    }
    else if (eRenderStage == eCS_InjectDynamicLights)
    {
        BindTiledLights(m_arrLightsDynamic, eHWSC_Compute);
        SetupLightSources(m_arrLightsDynamic, m_pShader, false);
        SetupNodesForUpdate(*pnNodesForUpdateStartIndex, arrNodesForUpdate);

        // update RGB
        pDeviceCtx->CSSetUnorderedAccessViews(2, 1, &vp_RGB0.pUAV, &UAVInitialCounts);

        if (e_svoTI_NumberOfBounces == 1)
        {
            pDeviceCtx->CSSetUnorderedAccessViews(7, 1, &vp_RGB1.pUAV, &UAVInitialCounts);
        }
        if (e_svoTI_NumberOfBounces == 2)
        {
            pDeviceCtx->CSSetUnorderedAccessViews(7, 1, &vp_RGB2.pUAV, &UAVInitialCounts);
        }
        if (e_svoTI_NumberOfBounces == 3)
        {
            pDeviceCtx->CSSetUnorderedAccessViews(7, 1, &vp_RGB3.pUAV, &UAVInitialCounts);
        }

        pDeviceCtx->CSSetUnorderedAccessViews(5, 1, &vp_DYNL.pUAV, &UAVInitialCounts);

        SetupSvoTexturesForRead(m_texInfo, eHWSC_Compute, 0);

        m_pNoiseTex->Apply(15, m_nTexStateLinearWrap, -1, -1, -1, eHWSC_Compute);

        SetupRsmSun(eHWSC_Compute);
    }
    else if (eRenderStage == eCS_PropagateLighting_1to2)
    {
        SetupLightSources(m_arrLightsStatic, m_pShader, false);
        SetupNodesForUpdate(*pnNodesForUpdateStartIndex, arrNodesForUpdate);

        // update RGB2
        if (vp_RGB0.pUAV)
        {
            pDeviceCtx->CSSetUnorderedAccessViews(0, 1, &vp_RGB0.pUAV, &UAVInitialCounts);
        }
		if (vp_RGB1.pUAV)
		{
			pDeviceCtx->CSSetUnorderedAccessViews(1, 1, &vp_RGB1.pUAV, &UAVInitialCounts);
		}
        SetupSvoTexturesForRead(m_texInfo, eHWSC_Compute, 1); // input
        if (vp_RGB2.pUAV)
        {
            pDeviceCtx->CSSetUnorderedAccessViews(5, 1, &vp_RGB2.pUAV, &UAVInitialCounts);
        }
        if (vp_ALDI.pUAV)
        {
            pDeviceCtx->CSSetUnorderedAccessViews(6, 1, &vp_ALDI.pUAV, &UAVInitialCounts);
        }
        if (vp_DYNL.pUAV)
        {
            pDeviceCtx->CSSetUnorderedAccessViews(7, 1, &vp_DYNL.pUAV, &UAVInitialCounts);
        }
    }
    else if (eRenderStage == eCS_PropagateLighting_2to3)
    {
        SetupLightSources(m_arrLightsStatic, m_pShader, false);
        SetupNodesForUpdate(*pnNodesForUpdateStartIndex, arrNodesForUpdate);

        // update RGB3
        if (vp_RGB0.pUAV)
        {
            pDeviceCtx->CSSetUnorderedAccessViews(0, 1, &vp_RGB0.pUAV, &UAVInitialCounts);
        }
        if (vp_RGB1.pUAV)
        {
            pDeviceCtx->CSSetUnorderedAccessViews(1, 1, &vp_RGB1.pUAV, &UAVInitialCounts);
        }
        SetupSvoTexturesForRead(m_texInfo, eHWSC_Compute, 2); // input
        if (vp_RGB3.pUAV)
        {
            pDeviceCtx->CSSetUnorderedAccessViews(5, 1, &vp_RGB3.pUAV, &UAVInitialCounts);
        }
        if (vp_DYNL.pUAV)
        {
            pDeviceCtx->CSSetUnorderedAccessViews(7, 1, &vp_DYNL.pUAV, &UAVInitialCounts);
        }
    }
    else if (eRenderStage == eCS_ClearBricks)
    {
        SetupNodesForUpdate(*pnNodesForUpdateStartIndex, arrNodesForUpdate);

        if (vp_RGB4.pUAV)
        {
            pDeviceCtx->CSSetUnorderedAccessViews(3, 1, &vp_RGB4.pUAV, &UAVInitialCounts);
        }
        pDeviceCtx->CSSetUnorderedAccessViews(4, 1, &vp_OPAC.pUAV, &UAVInitialCounts);
        if (vp_RGB3.pUAV)
        {
            pDeviceCtx->CSSetUnorderedAccessViews(6, 1, &vp_RGB3.pUAV, &UAVInitialCounts);
        }
        pDeviceCtx->CSSetUnorderedAccessViews(7, 1, &vp_RGB1.pUAV, &UAVInitialCounts);
        pDeviceCtx->CSSetUnorderedAccessViews(5, 1, &vp_RGB2.pUAV, &UAVInitialCounts);
        pDeviceCtx->CSSetUnorderedAccessViews(0, 1, &vp_ALDI.pUAV, &UAVInitialCounts);
    }

    {
        rd->FX_Commit();

        uint32 nDispatchSizeX = e_svoDispatchX;
        uint32 nDispatchSizeY = e_svoDispatchY;

#ifdef CINEBOX_APP
        pDeviceCtx->Dispatch(nDispatchSizeX, nDispatchSizeY, 1);
#else
        rd->m_DevMan.Dispatch(nDispatchSizeX, nDispatchSizeY, 1);
#endif

        SD3DPostEffectsUtils::ShEndPass();
    }

    D3DUnorderedAccessView* pUAVNULL = NULL;

    pDeviceCtx->CSSetUnorderedAccessViews(7, 1, &pUAVNULL, &UAVInitialCounts);
    pDeviceCtx->CSSetUnorderedAccessViews(6, 1, &pUAVNULL, &UAVInitialCounts);
    pDeviceCtx->CSSetUnorderedAccessViews(5, 1, &pUAVNULL, &UAVInitialCounts);
    pDeviceCtx->CSSetUnorderedAccessViews(4, 1, &pUAVNULL, &UAVInitialCounts);
    pDeviceCtx->CSSetUnorderedAccessViews(3, 1, &pUAVNULL, &UAVInitialCounts);
    pDeviceCtx->CSSetUnorderedAccessViews(2, 1, &pUAVNULL, &UAVInitialCounts);
    pDeviceCtx->CSSetUnorderedAccessViews(1, 1, &pUAVNULL, &UAVInitialCounts);
    pDeviceCtx->CSSetUnorderedAccessViews(0, 1, &pUAVNULL, &UAVInitialCounts);

    CTexture::GetByID(vp_RGB0.nTexId)->Unbind();
    CTexture::GetByID(vp_RGB1.nTexId)->Unbind();
    CTexture::GetByID(vp_DYNL.nTexId)->Unbind();
    CTexture::GetByID(vp_RGB2.nTexId)->Unbind();
    CTexture::GetByID(vp_RGB3.nTexId)->Unbind();
    CTexture::GetByID(vp_RGB4.nTexId)->Unbind();
    CTexture::GetByID(vp_NORM.nTexId)->Unbind();
    CTexture::GetByID(vp_ALDI.nTexId)->Unbind();
    CTexture::GetByID(vp_OPAC.nTexId)->Unbind();

    gcpRendD3D->GetTiledShading().UnbindForwardShadingResources(eHWSC_Compute);

    pSH->FXEnd();

#endif
}

CTexture* CSvoRenderer::GetGBuffer(int nId) // simplify branch compatibility
{
    CTexture* pRes;

    if (nId == 0)
    {
        pRes = CTexture::s_ptexSceneDiffuse;
    }
    else if (nId == 1)
    {
        pRes = CTexture::s_ptexSceneNormalsMap;
    }
    else if (nId == 2)
    {
        pRes = CTexture::s_ptexSceneSpecular;
    }
    else
    {
        pRes = 0;
    }

    return pRes;
}

void CSvoRenderer::TropospherePass()
{
#ifdef FEATURE_SVO_GI_ALLOW_HQ

    if (m_texInfo.bSvoFreeze)
    {
        return;
    }

    const char* szTechFinalName = "RenderAtmosphere";

    gcpRendD3D->FX_ClearTarget(m_pRT_AIR_MIN, Clr_Transparent);
    gcpRendD3D->FX_ClearTarget(m_pRT_AIR_SHAD, Clr_Transparent);
    gcpRendD3D->FX_ClearTarget(m_pRT_AIR_MAX, Clr_Transparent);

    gcpRendD3D->FX_PushRenderTarget(0, m_pRT_AIR_MIN, NULL);
    gcpRendD3D->FX_PushRenderTarget(1, m_pRT_AIR_SHAD, NULL);
    gcpRendD3D->FX_PushRenderTarget(2, m_pRT_AIR_MAX, NULL);

    CD3D9Renderer* const __restrict rd = gcpRendD3D;

    if (m_texInfo.pTexTree)
    {
        SetShaderFlags(0);

        SD3DPostEffectsUtils::ShBeginPass(m_pShader, szTechFinalName, 0 /*FEF_DONTSETTEXTURES | FEF_DONTSETSTATES*/);

        if (!gRenDev->m_RP.m_pShader)
        {
            gEnv->pLog->LogWarning("Error: %s: Technique not found: %s", __FUNC__, szTechFinalName);
            return;
        }

        {
            Matrix44A matView;
            matView = rd->m_RP.m_TI[rd->m_RP.m_nProcessThreadID].m_cam.GetViewMatrix();
            Vec3 zAxis = matView.GetRow(1);
            matView.SetRow(1, -matView.GetRow(2));
            matView.SetRow(2, zAxis);
            float z = matView.m13;
            matView.m13 = -matView.m23;
            matView.m23 = z;

            SetupLightSources(m_arrLightsStatic, m_pShader, true);

            {
                static CCryNameR parameterName5("SVO_ReprojectionMatrix");
                m_pShader->FXSetPSFloat(parameterName5, (Vec4*)m_matReproj.GetData(), 3);
            }

            {
                static CCryNameR parameterName6("SVO_FrameIdByte");
                Vec4 ttt((float)(rd->GetFrameID(false) & 255), (float)rd->GetFrameID(false), 3, 4);
                if (rd->GetActiveGPUCount() > 1)
                {
                    ttt.x = 0;
                }
                m_pShader->FXSetPSFloat(parameterName6, (Vec4*)&ttt, 1);
            }

            {
                static CCryNameR parameterName6("SVO_CamPos");
                Vec4 ttt(gEnv->pSystem->GetViewCamera().GetPosition(), 0);
                m_pShader->FXSetPSFloat(parameterName6, (Vec4*)&ttt, 1);
            }

            {
                Matrix44A mViewProj;
                mViewProj = gcpRendD3D->m_ViewProjMatrix;
                mViewProj.Transpose();

                static CCryNameR paramName("g_mViewProj");
                m_pShader->FXSetPSFloat(paramName, alias_cast<Vec4*>(&mViewProj), 4);

                static CCryNameR paramNamePrev("g_mViewProjPrev");
                m_pShader->FXSetPSFloat(paramNamePrev, alias_cast<Vec4*>(&m_matViewProjPrev), 4);
            }

            {
                float fSizeRatioW = float(rd->GetWidth() / m_pRT_AIR_MIN->GetWidth());
                float fSizeRatioH = float(rd->GetHeight() / m_pRT_AIR_MIN->GetHeight());
                static CCryNameR parameterName6("SVO_TargetResScale");
                static int nPrevWidth = 0;
                Vec4 ttt(fSizeRatioW, fSizeRatioH, e_svoTI_TemporalFilteringBase,
                    (float)(nPrevWidth != m_pRT_AIR_MIN->GetWidth() || (rd->m_RP.m_nRendFlags & SHDF_CUBEMAPGEN) || (rd->GetActiveGPUCount() > 1)));
                m_pShader->FXSetPSFloat(parameterName6, (Vec4*)&ttt, 1);
                nPrevWidth = m_pRT_AIR_MIN->GetWidth();
            }

            {
                static CCryNameR parameterName6("SVO_helperInfo");
                m_pShader->FXSetPSFloat(parameterName6, (Vec4*)&m_texInfo.helperInfo, 1);
            }

            {
                Matrix44A matView;
                matView = gcpRendD3D->m_RP.m_TI[gcpRendD3D->m_RP.m_nProcessThreadID].m_cam.GetViewMatrix();
                Vec3 zAxis = matView.GetRow(1);
                matView.SetRow(1, -matView.GetRow(2));
                matView.SetRow(2, zAxis);
                float z = matView.m13;
                matView.m13 = -matView.m23;
                matView.m23 = z;
                static CCryNameR paramName2("TI_CameraMatrix");
                m_pShader->FXSetPSFloat(paramName2, (Vec4*)matView.GetData(), 3);
            }

            {
                Vec3 pvViewFrust[8];
                const CameraViewParameters& rc = gcpRendD3D->GetViewParameters();
                rc.CalcVerts(pvViewFrust);

                static CCryNameR parameterName0("SVO_FrustumVerticesCam0");
                static CCryNameR parameterName1("SVO_FrustumVerticesCam1");
                static CCryNameR parameterName2("SVO_FrustumVerticesCam2");
                static CCryNameR parameterName3("SVO_FrustumVerticesCam3");
                Vec4 ttt0(pvViewFrust[4] - rc.vOrigin, 0);
                Vec4 ttt1(pvViewFrust[5] - rc.vOrigin, 0);
                Vec4 ttt2(pvViewFrust[6] - rc.vOrigin, 0);
                Vec4 ttt3(pvViewFrust[7] - rc.vOrigin, 0);
                m_pShader->FXSetPSFloat(parameterName0, (Vec4*)&ttt0, 1);
                m_pShader->FXSetPSFloat(parameterName1, (Vec4*)&ttt1, 1);
                m_pShader->FXSetPSFloat(parameterName2, (Vec4*)&ttt2, 1);
                m_pShader->FXSetPSFloat(parameterName3, (Vec4*)&ttt3, 1);
            }
        }

        rd->FX_SetState(GS_NODEPTHTEST);

        SetupSvoTexturesForRead(m_texInfo, eHWSC_Pixel, e_svoTI_NumberOfBounces, 0, 0);

        CTexture::s_ptexZTarget->Apply(4, m_nTexStatePoint);

        GetGBuffer(1)->Apply(5, m_nTexStatePoint);

        GetGBuffer(2)->Apply(7, m_nTexStatePoint);

        GetGBuffer(0)->Apply(14, m_nTexStatePoint);

        rd->FX_Commit();

        SD3DPostEffectsUtils::DrawFullScreenTriWPOS(CTexture::s_ptexCurrentSceneDiffuseAccMap->GetWidth(), CTexture::s_ptexCurrentSceneDiffuseAccMap->GetHeight());

        SD3DPostEffectsUtils::ShEndPass();
    }

    gcpRendD3D->FX_PopRenderTarget(0);
    gcpRendD3D->FX_PopRenderTarget(1);
    gcpRendD3D->FX_PopRenderTarget(2);

#endif
}

void CSvoRenderer::ConeTracePass(SSvoTargetsSet* pTS)
{
    CheckAllocateRT(pTS == &m_tsSpec);

    if (!e_svoTI_Active || !e_svoTI_Apply || !e_svoRender || !m_pShader)
    {
        return;
    }

    const char* szTechFinalName = "ConeTracePass";

    if (m_texInfo.bSvoFreeze)
    {
        return;
    }

    CD3D9Renderer* const __restrict rd = gcpRendD3D;

    rd->FX_PushRenderTarget(0, pTS->pRT_ALD_0, NULL);
    rd->FX_PushRenderTarget(1, pTS->pRT_RGB_0, NULL);

    if (m_texInfo.pTexTree)
    {
        SetShaderFlags(pTS == &m_tsDiff);

        SD3DPostEffectsUtils::ShBeginPass(m_pShader, szTechFinalName, FEF_DONTSETTEXTURES /*| FEF_DONTSETSTATES*/);

        {
            Matrix44A matView;
            matView = rd->m_RP.m_TI[rd->m_RP.m_nProcessThreadID].m_cam.GetViewMatrix();
            Vec3 zAxis = matView.GetRow(1);
            matView.SetRow(1, -matView.GetRow(2));
            matView.SetRow(2, zAxis);
            float z = matView.m13;
            matView.m13 = -matView.m23;
            matView.m23 = z;

            SetupRsmSun(eHWSC_Pixel);

            static int nReprojFrameId = -1;
            if ((pTS == &m_tsDiff) && nReprojFrameId != rd->GetFrameID(false))
            {
                nReprojFrameId = rd->GetFrameID(false);

                Matrix44A matProj;
                const CCamera& cam = rd->m_RP.m_TI[rd->m_RP.m_nProcessThreadID].m_cam;
                mathMatrixPerspectiveFov(&matProj, cam.GetFov(), cam.GetProjRatio(), cam.GetNearPlane(), cam.GetFarPlane());
                static Matrix44A matPrevView = matView;
                static Matrix44A matPrevProj = matProj;
                rd->GetReprojectionMatrix(m_matReproj, matView, matProj, matPrevView, matPrevProj, cam.GetFarPlane());
                matPrevView = matView;
                matPrevProj = matProj;
            }

            {
                static CCryNameR parameterName5("SVO_ReprojectionMatrix");
                m_pShader->FXSetPSFloat(parameterName5, (Vec4*)m_matReproj.GetData(), 3);
            }

            {
                static CCryNameR parameterName6("SVO_FrameIdByte");
                Vec4 ttt((float)(rd->GetFrameID(false) & 255), (float)rd->GetFrameID(false), 3, 4);
                if (rd->GetActiveGPUCount() > 1)
                {
                    ttt.x = 0;
                }
                m_pShader->FXSetPSFloat(parameterName6, (Vec4*)&ttt, 1);
            }

            {
                static CCryNameR parameterName6("SVO_CamPos");
                Vec4 ttt(gEnv->pSystem->GetViewCamera().GetPosition(), 0);
                m_pShader->FXSetPSFloat(parameterName6, (Vec4*)&ttt, 1);
            }

            {
                Matrix44A mViewProj;
                mViewProj = gcpRendD3D->m_ViewProjMatrix;
                mViewProj.Transpose();

                static CCryNameR paramName("g_mViewProj");
                m_pShader->FXSetPSFloat(paramName, alias_cast<Vec4*>(&mViewProj), 4);

                static CCryNameR paramNamePrev("g_mViewProjPrev");
                m_pShader->FXSetPSFloat(paramNamePrev, alias_cast<Vec4*>(&m_matViewProjPrev), 4);
            }

            {
                float fSizeRatioW = float(rd->GetWidth() / rd->m_RTStack[0][rd->m_nRTStackLevel[0]].m_Width);
                float fSizeRatioH = float(rd->GetHeight() / rd->m_RTStack[0][rd->m_nRTStackLevel[0]].m_Height);
                static CCryNameR parameterName6("SVO_TargetResScale");
                static int nPrevWidth = 0;
                Vec4 ttt(fSizeRatioW, fSizeRatioH, e_svoTI_TemporalFilteringBase,
                    (float)(nPrevWidth != (pTS->pRT_ALD_0->GetWidth() + int(e_svoTI_SkyColorMultiplier > 0)) || (rd->m_RP.m_nRendFlags & SHDF_CUBEMAPGEN) || (rd->GetActiveGPUCount() > 1)));
                m_pShader->FXSetPSFloat(parameterName6, (Vec4*)&ttt, 1);
                nPrevWidth = (pTS->pRT_ALD_0->GetWidth() + int(e_svoTI_SkyColorMultiplier > 0));
            }

            {
                static CCryNameR parameterName6("SVO_helperInfo");
                m_pShader->FXSetPSFloat(parameterName6, (Vec4*)&m_texInfo.helperInfo, 1);
            }

            {
                Matrix44A matView;
                matView = gcpRendD3D->m_RP.m_TI[gcpRendD3D->m_RP.m_nProcessThreadID].m_cam.GetViewMatrix();
                Vec3 zAxis = matView.GetRow(1);
                matView.SetRow(1, -matView.GetRow(2));
                matView.SetRow(2, zAxis);
                float z = matView.m13;
                matView.m13 = -matView.m23;
                matView.m23 = z;
                static CCryNameR paramName2("TI_CameraMatrix");
                m_pShader->FXSetPSFloat(paramName2, (Vec4*)matView.GetData(), 3);
            }

            {
                Vec3 pvViewFrust[8];
                const CameraViewParameters& rc = gcpRendD3D->GetViewParameters();
                rc.CalcVerts(pvViewFrust);

                static CCryNameR parameterName0("SVO_FrustumVerticesCam0");
                static CCryNameR parameterName1("SVO_FrustumVerticesCam1");
                static CCryNameR parameterName2("SVO_FrustumVerticesCam2");
                static CCryNameR parameterName3("SVO_FrustumVerticesCam3");
                Vec4 ttt0(pvViewFrust[4] - rc.vOrigin, 0);
                Vec4 ttt1(pvViewFrust[5] - rc.vOrigin, 0);
                Vec4 ttt2(pvViewFrust[6] - rc.vOrigin, 0);
                Vec4 ttt3(pvViewFrust[7] - rc.vOrigin, 0);
                m_pShader->FXSetPSFloat(parameterName0, (Vec4*)&ttt0, 1);
                m_pShader->FXSetPSFloat(parameterName1, (Vec4*)&ttt1, 1);
                m_pShader->FXSetPSFloat(parameterName2, (Vec4*)&ttt2, 1);
                m_pShader->FXSetPSFloat(parameterName3, (Vec4*)&ttt3, 1);
            }

            {
                static CCryNameR nameSVO_PortalsPos("SVO_PortalsPos");
                SetShaderFloat(eHWSC_Pixel, nameSVO_PortalsPos, (Vec4*)&m_texInfo.arrPortalsPos[0], SVO_MAX_PORTALS);
                static CCryNameR nameSVO_PortalsDir("SVO_PortalsDir");
                SetShaderFloat(eHWSC_Pixel, nameSVO_PortalsDir, (Vec4*)&m_texInfo.arrPortalsDir[0], SVO_MAX_PORTALS);
            }

            {
                static CCryNameR nameSVO_Reserved("SVO_Reserved");
                Vec4 vReserved(e_svoTI_Reserved0, e_svoTI_Reserved1, e_svoTI_Reserved2, e_svoTI_Reserved3);
                SetShaderFloat(eHWSC_Pixel, nameSVO_Reserved, (Vec4*)&vReserved, 1);
            }

            if (pTS->pRT_ALD_1 && pTS->pRT_ALD_1)
            {
                static int nPrevWidth = 0;
                if (nPrevWidth != (pTS->pRT_ALD_1->GetWidth() + e_svoTI_Diffuse_Cache))
                {
                    CTexture::s_ptexWhite->Apply(10, m_nTexStateLinear);
                    CTexture::s_ptexWhite->Apply(11, m_nTexStateLinear);
                    nPrevWidth = pTS->pRT_ALD_1->GetWidth() + e_svoTI_Diffuse_Cache;
                }
                else
                {
                    pTS->pRT_ALD_1->Apply(10, m_nTexStateLinear);
                    pTS->pRT_RGB_1->Apply(11, m_nTexStateLinear);
                }
            }
        }

        rd->FX_SetState(GS_NODEPTHTEST);

        SetupSvoTexturesForRead(m_texInfo, eHWSC_Pixel, (e_svoTI_Active ? e_svoTI_NumberOfBounces : 0), 0, 0);

        CTexture::s_ptexZTarget->Apply(4, m_nTexStatePoint);

        {
            GetGBuffer(1)->Apply(5, m_nTexStatePoint);

            GetGBuffer(2)->Apply(7, m_nTexStatePoint);

            GetGBuffer(0)->Apply(14, m_nTexStatePoint);
        }

#ifdef FEATURE_SVO_GI_ALLOW_HQ
        if (m_texInfo.pTexIndA)
        {
            ((CTexture*)m_texInfo.pTexTexA)->Apply(8, m_nTexStateLinear);
            ((CTexture*)m_texInfo.pTexTriA)->Apply(9, m_nTexStatePoint);
            ((CTexture*)m_texInfo.pTexIndA)->Apply(13, m_nTexStatePoint);
        }
#endif

        if (!GetIntegratioMode() && e_svoTI_InjectionMultiplier && m_arrLightsDynamic.Count())
        {
            BindTiledLights(m_arrLightsDynamic, eHWSC_Pixel);
            SetupLightSources(m_arrLightsDynamic, m_pShader, true);
        }

        if (GetIntegratioMode() && e_svoTI_SSDepthTrace)
        {
            if (CTexture::s_ptexHDRTargetPrev->GetUpdateFrameID() > 1)
            {
                SD3DPostEffectsUtils::SetTexture(CTexture::s_ptexHDRTargetPrev, 12, FILTER_LINEAR, TADDR_BORDER);
            }
            else
            {
                SD3DPostEffectsUtils::SetTexture(CTexture::s_ptexBlack, 12, FILTER_LINEAR, TADDR_BORDER);
            }
        }

        {
            const bool setupCloudShadows = rd->m_bShadowsEnabled && rd->m_bCloudShadowsEnabled;
            if (setupCloudShadows)
            {
                // cloud shadow map
                CTexture* pCloudShadowTex(rd->GetCloudShadowTextureId() > 0 ? CTexture::GetByID(rd->GetCloudShadowTextureId()) : CTexture::s_ptexWhite);
                assert(pCloudShadowTex);

                STexState pTexStateLinearClamp;
                pTexStateLinearClamp.SetFilterMode(FILTER_LINEAR);
                pTexStateLinearClamp.SetClampMode(false, false, false);
                int nTexStateLinearClampID = CTexture::GetTexState(pTexStateLinearClamp);

                pCloudShadowTex->Apply(15, m_nTexStateLinearWrap);
            }
            else
            {
                CTexture::s_ptexWhite->Apply(15, m_nTexStateLinearWrap);
            }
        }

        rd->FX_Commit();

        SD3DPostEffectsUtils::DrawFullScreenTriWPOS(CTexture::s_ptexCurrentSceneDiffuseAccMap->GetWidth(), CTexture::s_ptexCurrentSceneDiffuseAccMap->GetHeight());

        gcpRendD3D->GetTiledShading().UnbindForwardShadingResources(eHWSC_Pixel);

        SD3DPostEffectsUtils::ShEndPass();
    }

    rd->FX_PopRenderTarget(0);
    rd->FX_PopRenderTarget(1);
}

void CSvoRenderer::DrawPonts(PodArray<SVF_P3F_C4B_T2F>& arrVerts)
{
    SPostEffectsUtils::UpdateFrustumCorners();

    CVertexBuffer strip(arrVerts.GetElements(), eVF_P3F_C4B_T2F);

    gRenDev->DrawPrimitivesInternal(&strip, arrVerts.Count() / max(1, e_svoRender), eptPointList);
}

void CSvoRenderer::UpdateRender()
{
    int nPrevAsync = CRenderer::CV_r_shadersasynccompiling;
    if (gEnv->IsEditor())
    {
        CRenderer::CV_r_shadersasynccompiling = 0;
    }

#ifdef FEATURE_SVO_GI_ALLOW_HQ
    if (e_svoEnabled && e_svoTI_Active && e_svoTI_Apply && m_texInfo.bSvoReady && e_svoTI_Troposphere_Active && m_pRT_AIR_MIN)
    {
        PROFILE_LABEL_SCOPE("TI_GEN_AIR");

        if (e_svoTI_DiffuseAmplifier || e_svoTI_SpecularAmplifier || !e_svoTI_DiffuseBias)
        {
            TropospherePass();
        }
    }
#endif

    {
        PROFILE_LABEL_SCOPE("TI_GEN_DIFF");

        ConeTracePass(&m_tsDiff);
    }
    if (GetIntegratioMode() == 2 && e_svoTI_SpecularAmplifier)
    {
        PROFILE_LABEL_SCOPE("TI_GEN_SPEC");

        ConeTracePass(&m_tsSpec);
    }

    {
        PROFILE_LABEL_SCOPE("TI_DEMOSAIC_DIFF");

        DemosaicPass(&m_tsDiff);
    }
    if (GetIntegratioMode() == 2 && e_svoTI_SpecularAmplifier)
    {
        PROFILE_LABEL_SCOPE("TI_DEMOSAIC_SPEC");

        DemosaicPass(&m_tsSpec);
    }

    {
        PROFILE_LABEL_SCOPE("TI_UPSCALE_DIFF");

        UpScalePass(&m_tsDiff);
    }
    if (GetIntegratioMode() == 2 && e_svoTI_SpecularAmplifier)
    {
        PROFILE_LABEL_SCOPE("TI_UPSCALE_SPEC");

        UpScalePass(&m_tsSpec);
    }

    {
        m_matViewProjPrev = gcpRendD3D->m_ViewProjMatrix;
        m_matViewProjPrev.Transpose();
    }

    if (gEnv->IsEditor())
    {
        CRenderer::CV_r_shadersasynccompiling = nPrevAsync;
    }
}

void CSvoRenderer::DemosaicPass(SSvoTargetsSet* pTS)
{
    const char* szTechFinalName = "DemosaicPass";

    if (!e_svoTI_Active || !e_svoTI_Apply || !e_svoRender || !m_pShader)
    {
        return;
    }

    if (m_texInfo.bSvoFreeze)
    {
        return;
    }

    if (e_svoTI_Apply)
    { // SVO
        if (!pTS->pRT_ALD_0 || !pTS->pRT_ALD_0)
        {
            return;
        }

        CD3D9Renderer* const __restrict rd = gcpRendD3D;

        rd->FX_PushRenderTarget(0, pTS->pRT_RGB_DEM_MIN_0, NULL, -1, false, 1);
        rd->FX_PushRenderTarget(1, pTS->pRT_ALD_DEM_MIN_0, NULL, -1, false, 1);
        rd->FX_PushRenderTarget(2, pTS->pRT_RGB_DEM_MAX_0, NULL, -1, false, 1);
        rd->FX_PushRenderTarget(3, pTS->pRT_ALD_DEM_MAX_0, NULL, -1, false, 1);

        if (m_texInfo.pTexTree)
        {
            SetShaderFlags(pTS == &m_tsDiff);

            SD3DPostEffectsUtils::ShBeginPass(m_pShader, szTechFinalName, FEF_DONTSETTEXTURES);

            rd->FX_SetState(GS_NODEPTHTEST);

            //SetupSvoTexturesForRead(m_texInfo, eHWSC_Pixel, e_svoTI_NumberOfBounces);

            CTexture::s_ptexZTarget->Apply(4, m_nTexStatePoint);

            GetGBuffer(1)->Apply(5, m_nTexStatePoint);

            GetGBuffer(2)->Apply(7, m_nTexStatePoint);

            GetGBuffer(0)->Apply(14, m_nTexStatePoint);

            {
                static CCryNameR parameterName5("SVO_ReprojectionMatrix");
                m_pShader->FXSetPSFloat(parameterName5, (Vec4*)m_matReproj.GetData(), 3);
            }

            {
                float fSizeRatioW = float(rd->GetWidth() / rd->m_RTStack[0][rd->m_nRTStackLevel[0]].m_Width);
                float fSizeRatioH = float(rd->GetHeight() / rd->m_RTStack[0][rd->m_nRTStackLevel[0]].m_Height);
                static CCryNameR parameterName6("SVO_TargetResScale");
                static int nPrevWidth = 0;
                Vec4 ttt(fSizeRatioW, fSizeRatioH, e_svoTI_TemporalFilteringBase,
                    (float)(nPrevWidth != (pTS->pRT_ALD_0->GetWidth() + int(e_svoTI_SkyColorMultiplier > 0)) || (rd->m_RP.m_nRendFlags & SHDF_CUBEMAPGEN) || (rd->GetActiveGPUCount() > 1)));
                m_pShader->FXSetPSFloat(parameterName6, (Vec4*)&ttt, 1);
                nPrevWidth = (pTS->pRT_ALD_0->GetWidth() + int(e_svoTI_SkyColorMultiplier > 0));
            }

            {
                static CCryNameR parameterName6("SVO_FrameIdByte");
                Vec4 ttt((float)(rd->GetFrameID(false) & 255), (float)rd->GetFrameID(false), 3, 4);
                if (rd->GetActiveGPUCount() > 1)
                {
                    ttt.x = 0;
                }
                m_pShader->FXSetPSFloat(parameterName6, (Vec4*)&ttt, 1);
            }

            {
                static CCryNameR parameterName6("SVO_CamPos");
                Vec4 ttt(gEnv->pSystem->GetViewCamera().GetPosition(), 0);
                m_pShader->FXSetPSFloat(parameterName6, (Vec4*)&ttt, 1);
            }

            {
                Matrix44A mViewProj;
                mViewProj = gcpRendD3D->m_ViewProjMatrix;
                mViewProj.Transpose();

                static CCryNameR paramName("g_mViewProj");
                m_pShader->FXSetPSFloat(paramName, alias_cast<Vec4*>(&mViewProj), 4);

                static CCryNameR paramNamePrev("g_mViewProjPrev");
                m_pShader->FXSetPSFloat(paramNamePrev, alias_cast<Vec4*>(&m_matViewProjPrev), 4);
            }

            {
                Matrix44A matView;
                matView = gcpRendD3D->m_RP.m_TI[gcpRendD3D->m_RP.m_nProcessThreadID].m_cam.GetViewMatrix();
                Vec3 zAxis = matView.GetRow(1);
                matView.SetRow(1, -matView.GetRow(2));
                matView.SetRow(2, zAxis);
                float z = matView.m13;
                matView.m13 = -matView.m23;
                matView.m23 = z;
                static CCryNameR paramName2("TI_CameraMatrix");
                m_pShader->FXSetPSFloat(paramName2, (Vec4*)matView.GetData(), 3);
            }

            pTS->pRT_ALD_0->Apply(10, m_nTexStateLinear);
            pTS->pRT_RGB_0->Apply(11, m_nTexStateLinear);

            pTS->pRT_RGB_DEM_MIN_1->Apply(0, m_nTexStateLinear);
            pTS->pRT_ALD_DEM_MIN_1->Apply(1, m_nTexStateLinear);
            pTS->pRT_RGB_DEM_MAX_1->Apply(2, m_nTexStateLinear);
            pTS->pRT_ALD_DEM_MAX_1->Apply(3, m_nTexStateLinear);

            //CTexture::s_ptexSceneSpecularAccMap->Apply(15, m_nTexStateLinear);

            SD3DPostEffectsUtils::DrawFullScreenTriWPOS(CTexture::s_ptexCurrentSceneDiffuseAccMap->GetWidth(), CTexture::s_ptexCurrentSceneDiffuseAccMap->GetHeight());

            SD3DPostEffectsUtils::ShEndPass();
        }

        rd->FX_PopRenderTarget(0);
        rd->FX_PopRenderTarget(1);
        rd->FX_PopRenderTarget(2);
        rd->FX_PopRenderTarget(3);
    }
}

void CSvoRenderer::SetupLightSources(PodArray<I3DEngine::SLightTI>& lightsTI, CShader* pShader, bool bPS)
{
    const int nLightGroupsNum = 2;

    static CCryNameR paramNamesLightPos[nLightGroupsNum] =
    {
        CCryNameR("SVO_LightPos0"),
        CCryNameR("SVO_LightPos1"),
    };
    static CCryNameR paramNamesLightDir[nLightGroupsNum] =
    {
        CCryNameR("SVO_LightDir0"),
        CCryNameR("SVO_LightDir1"),
    };
    static CCryNameR paramNamesLightCol[nLightGroupsNum] =
    {
        CCryNameR("SVO_LightCol0"),
        CCryNameR("SVO_LightCol1"),
    };

    for (int g = 0; g < nLightGroupsNum; g++)
    {
        Vec4 LightPos[4];
        Vec4 LightDir[4];
        Vec4 LightCol[4];

        for (int x = 0; x < 4; x++)
        {
            int nId = g * 4 + x;

            LightPos[x] = (nId < lightsTI.Count()) ? lightsTI[nId].vPosR : Vec4(0, 0, 0, 0);
            LightDir[x] = (nId < lightsTI.Count()) ? lightsTI[nId].vDirF : Vec4(0, 0, 0, 0);
            LightCol[x] = (nId < lightsTI.Count()) ? lightsTI[nId].vCol  : Vec4(0, 0, 0, 0);
        }

        if (bPS)
        {
            pShader->FXSetPSFloat(paramNamesLightPos[g], alias_cast<Vec4*>(&LightPos[0][0]), 4);
            pShader->FXSetPSFloat(paramNamesLightDir[g], alias_cast<Vec4*>(&LightDir[0][0]), 4);
            pShader->FXSetPSFloat(paramNamesLightCol[g], alias_cast<Vec4*>(&LightCol[0][0]), 4);
        }
        else
        { // CS
            pShader->FXSetCSFloat(paramNamesLightPos[g], alias_cast<Vec4*>(&LightPos[0][0]), 4);
            pShader->FXSetCSFloat(paramNamesLightDir[g], alias_cast<Vec4*>(&LightDir[0][0]), 4);
            pShader->FXSetCSFloat(paramNamesLightCol[g], alias_cast<Vec4*>(&LightCol[0][0]), 4);
        }
    }
}

void CSvoRenderer::SetupNodesForUpdate(int& nNodesForUpdateStartIndex, PodArray<I3DEngine::SSvoNodeInfo>& arrNodesForUpdate)
{
    static CCryNameR paramNames[SVO_MAX_NODE_GROUPS] =
    {
        CCryNameR("SVO_NodesForUpdate0"),
        CCryNameR("SVO_NodesForUpdate1"),
        CCryNameR("SVO_NodesForUpdate2"),
        CCryNameR("SVO_NodesForUpdate3"),
    };

    for (int g = 0; g < SVO_MAX_NODE_GROUPS; g++)
    {
        float matVal[4][4];

        for (int x = 0; x < 4; x++)
        {
            for (int y = 0; y < 4; y++)
            {
                int nId = nNodesForUpdateStartIndex + g * 16 + x * 4 + y;
                matVal[x][y] = 0.1f + ((nId < arrNodesForUpdate.Count()) ? arrNodesForUpdate[nId].nAtlasOffset : -2);

                if (nId < arrNodesForUpdate.Count())
                {
                    float fNodeSize = arrNodesForUpdate[nId].wsBox.GetSize().x;

                    if (e_svoDebug == 8)
                    {
                        gRenDev->GetIRenderAuxGeom()->DrawAABB(arrNodesForUpdate[nId].wsBox, 0, ColorF(fNodeSize == 4.f, fNodeSize == 8.f, fNodeSize == 16.f), eBBD_Faceted);
                    }
                }
            }
        }

        m_pShader->FXSetCSFloat(paramNames[g], alias_cast<Vec4*>(&matVal[0][0]), 4);
    }

    nNodesForUpdateStartIndex += 4 * 4 * SVO_MAX_NODE_GROUPS; // 128
}

void CSvoRenderer::SetupSvoTexturesForRead(I3DEngine::SSvoStaticTexInfo& texInfo, EHWShaderClass eShaderClass, int nStage, int nStageOpa, int nStageNorm)
{
    ((CTexture*)texInfo.pTexTree)->Apply(0, m_nTexStatePoint, -1, -1, -1, eShaderClass);

    CTexture::s_ptexBlack->Apply(1, m_nTexStateLinear, -1, -1, -1, eShaderClass);

#ifdef FEATURE_SVO_GI_ALLOW_HQ

    if (nStage == 0)
    {
        CTexture::GetByID(vp_RGB0.nTexId)->Apply(1, m_nTexStateLinear, -1, -1, -1, eShaderClass);
    }
    else if (nStage == 1)
    {
        CTexture::GetByID(vp_RGB1.nTexId)->Apply(1, m_nTexStateLinear, -1, -1, -1, eShaderClass);
    }
    else if (nStage == 2)
    {
        CTexture::GetByID(vp_RGB2.nTexId)->Apply(1, m_nTexStateLinear, -1, -1, -1, eShaderClass);
    }
    else if (nStage == 3)
    {
        CTexture::GetByID(vp_RGB3.nTexId)->Apply(1, m_nTexStateLinear, -1, -1, -1, eShaderClass);
    }
    else if (nStage == 4)
    {
        CTexture::GetByID(vp_RGB4.nTexId)->Apply(1, m_nTexStateLinear, -1, -1, -1, eShaderClass);
    }

    if (nStageNorm == 0)
    {
        CTexture::GetByID(vp_NORM.nTexId)->Apply(2, m_nTexStateLinear, -1, -1, -1, eShaderClass);
    }

#endif

    if (nStageOpa == 0)
    {
        ((CTexture*)texInfo.pTexOpac)->Apply(3, m_nTexStateLinear, -1, -1, -1, eShaderClass);
    }

#ifdef FEATURE_SVO_GI_ALLOW_HQ

    else if (nStageOpa == 1)
    {
        CTexture::GetByID(vp_RGB4.nTexId)->Apply(3, m_nTexStateLinear, -1, -1, -1, eShaderClass);
    }

    if (texInfo.pTexTris)
    {
        ((CTexture*)texInfo.pTexTris)->Apply(6, m_nTexStatePoint, -1, -1, -1, eShaderClass);
    }
    if (texInfo.pGlobalSpecCM)
    {
        ((CTexture*)texInfo.pGlobalSpecCM)->Apply(6,    m_nTexStateTrilinear, -1, -1, -1, eShaderClass);
    }

#endif
}

void CSvoRenderer::CheckAllocateRT(bool bSpecPass)
{
    int nWidth = gcpRendD3D->GetWidth();
    int nHeight = gcpRendD3D->GetHeight();

    int nVoxProxyViewportDim = e_svoVoxGenRes;

    int nResScaleBase = SATURATEB(e_svoTI_ResScaleBase + e_svoTI_LowSpecMode);
    int nDownscaleAir  = SATURATEB(e_svoTI_ResScaleAir  + e_svoTI_LowSpecMode);

    int nInW = (nWidth  / nResScaleBase);
    int nInH = (nHeight / nResScaleBase);
    int nAirW = (nWidth  / nDownscaleAir);
    int nAirH = (nHeight / nDownscaleAir);

    if (!bSpecPass)
    {
        CheckCreateUpdateRT(m_tsDiff.pRT_ALD_0, nInW, nInH, eTF_R16G16B16A16F, eTT_2D,  FT_STATE_CLAMP, "SVO_DIFF_ALD");
        CheckCreateUpdateRT(m_tsDiff.pRT_ALD_1, nInW, nInH, eTF_R16G16B16A16F, eTT_2D,  FT_STATE_CLAMP, "SV1_DIFF_ALD");
        CheckCreateUpdateRT(m_tsDiff.pRT_RGB_0, nInW, nInH, eTF_R16G16B16A16F, eTT_2D,  FT_STATE_CLAMP, "SVO_DIFF_RGB");
        CheckCreateUpdateRT(m_tsDiff.pRT_RGB_1, nInW, nInH, eTF_R16G16B16A16F, eTT_2D,  FT_STATE_CLAMP, "SV1_DIFF_RGB");

        CheckCreateUpdateRT(m_tsSpec.pRT_ALD_0, nInW, nInH, eTF_R16G16B16A16F, eTT_2D,  FT_STATE_CLAMP, "SVO_SPEC_ALD");
        CheckCreateUpdateRT(m_tsSpec.pRT_ALD_1, nInW, nInH, eTF_R16G16B16A16F, eTT_2D,  FT_STATE_CLAMP, "SV1_SPEC_ALD");
        CheckCreateUpdateRT(m_tsSpec.pRT_RGB_0, nInW, nInH, eTF_R16G16B16A16F, eTT_2D,  FT_STATE_CLAMP, "SVO_SPEC_RGB");
        CheckCreateUpdateRT(m_tsSpec.pRT_RGB_1, nInW, nInH, eTF_R16G16B16A16F, eTT_2D,  FT_STATE_CLAMP, "SV1_SPEC_RGB");

#ifdef FEATURE_SVO_GI_ALLOW_HQ
        if (e_svoTI_Troposphere_Active)
        {
            CheckCreateUpdateRT(m_pRT_NID_0, nInW, nInH, eTF_R16G16B16A16F, eTT_2D,  FT_STATE_CLAMP, "SVO_NID");
            CheckCreateUpdateRT(m_pRT_AIR_MIN, nAirW, nAirH, eTF_R16G16B16A16F, eTT_2D,  FT_STATE_CLAMP, "SVO_AIR_MIN");
            CheckCreateUpdateRT(m_pRT_AIR_MAX, nAirW, nAirH, eTF_R16G16B16A16F, eTT_2D,  FT_STATE_CLAMP, "SVO_AIR_MAX");
            CheckCreateUpdateRT(m_pRT_AIR_SHAD, nAirW, nAirH, eTF_R16G16B16A16F, eTT_2D,  FT_STATE_CLAMP, "SVO_AIR_SHAD");
        }
#endif

        CheckCreateUpdateRT(m_tsDiff.pRT_RGB_DEM_MIN_0, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_DIFF_FIN_RGB_MIN");
        CheckCreateUpdateRT(m_tsDiff.pRT_ALD_DEM_MIN_0, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_DIFF_FIN_ALD_MIN");
        CheckCreateUpdateRT(m_tsDiff.pRT_RGB_DEM_MAX_0, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_DIFF_FIN_RGB_MAX");
        CheckCreateUpdateRT(m_tsDiff.pRT_ALD_DEM_MAX_0, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_DIFF_FIN_ALD_MAX");
        CheckCreateUpdateRT(m_tsDiff.pRT_RGB_DEM_MIN_1, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SV1_DIFF_FIN_RGB_MIN");
        CheckCreateUpdateRT(m_tsDiff.pRT_ALD_DEM_MIN_1, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SV1_DIFF_FIN_ALD_MIN");
        CheckCreateUpdateRT(m_tsDiff.pRT_RGB_DEM_MAX_1, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SV1_DIFF_FIN_RGB_MAX");
        CheckCreateUpdateRT(m_tsDiff.pRT_ALD_DEM_MAX_1, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SV1_DIFF_FIN_ALD_MAX");

        CheckCreateUpdateRT(m_tsDiff.pRT_FIN_OUT_0, nWidth, nHeight, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_FIN_DIFF_OUT");
        CheckCreateUpdateRT(m_tsDiff.pRT_FIN_OUT_1, nWidth, nHeight, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SV1_FIN_DIFF_OUT");

        CheckCreateUpdateRT(m_tsSpec.pRT_RGB_DEM_MIN_0, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_SPEC_FIN_RGB_MIN");
        CheckCreateUpdateRT(m_tsSpec.pRT_ALD_DEM_MIN_0, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_SPEC_FIN_ALD_MIN");
        CheckCreateUpdateRT(m_tsSpec.pRT_RGB_DEM_MAX_0, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_SPEC_FIN_RGB_MAX");
        CheckCreateUpdateRT(m_tsSpec.pRT_ALD_DEM_MAX_0, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_SPEC_FIN_ALD_MAX");
        CheckCreateUpdateRT(m_tsSpec.pRT_RGB_DEM_MIN_1, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SV1_SPEC_FIN_RGB_MIN");
        CheckCreateUpdateRT(m_tsSpec.pRT_ALD_DEM_MIN_1, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SV1_SPEC_FIN_ALD_MIN");
        CheckCreateUpdateRT(m_tsSpec.pRT_RGB_DEM_MAX_1, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SV1_SPEC_FIN_RGB_MAX");
        CheckCreateUpdateRT(m_tsSpec.pRT_ALD_DEM_MAX_1, nInW, nInH, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SV1_SPEC_FIN_ALD_MAX");

        CheckCreateUpdateRT(m_tsSpec.pRT_FIN_OUT_0, nWidth, nHeight, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SVO_FIN_SPEC_OUT");
        CheckCreateUpdateRT(m_tsSpec.pRT_FIN_OUT_1, nWidth, nHeight, eTF_R16G16B16A16F, eTT_2D, FT_STATE_CLAMP, "SV1_FIN_SPEC_OUT");

        // swap ping-pong RT
        std::swap(m_tsDiff.pRT_ALD_0, m_tsDiff.pRT_ALD_1);
        std::swap(m_tsDiff.pRT_RGB_0, m_tsDiff.pRT_RGB_1);
        std::swap(m_tsDiff.pRT_RGB_DEM_MIN_0, m_tsDiff.pRT_RGB_DEM_MIN_1);
        std::swap(m_tsDiff.pRT_ALD_DEM_MIN_0, m_tsDiff.pRT_ALD_DEM_MIN_1);
        std::swap(m_tsDiff.pRT_RGB_DEM_MAX_0, m_tsDiff.pRT_RGB_DEM_MAX_1);
        std::swap(m_tsDiff.pRT_ALD_DEM_MAX_0, m_tsDiff.pRT_ALD_DEM_MAX_1);
        std::swap(m_tsDiff.pRT_FIN_OUT_0, m_tsDiff.pRT_FIN_OUT_1);

        std::swap(m_tsSpec.pRT_ALD_0, m_tsSpec.pRT_ALD_1);
        std::swap(m_tsSpec.pRT_RGB_0, m_tsSpec.pRT_RGB_1);
        std::swap(m_tsSpec.pRT_RGB_DEM_MIN_0, m_tsSpec.pRT_RGB_DEM_MIN_1);
        std::swap(m_tsSpec.pRT_ALD_DEM_MIN_0, m_tsSpec.pRT_ALD_DEM_MIN_1);
        std::swap(m_tsSpec.pRT_RGB_DEM_MAX_0, m_tsSpec.pRT_RGB_DEM_MAX_1);
        std::swap(m_tsSpec.pRT_ALD_DEM_MAX_0, m_tsSpec.pRT_ALD_DEM_MAX_1);
        std::swap(m_tsSpec.pRT_FIN_OUT_0, m_tsSpec.pRT_FIN_OUT_1);
    }
}

bool CSvoRenderer::IsShaderItemUsedForVoxelization(SShaderItem& rShaderItem, IRenderNode* pRN)
{
    CShader* pS = (CShader*)rShaderItem.m_pShader;
    CShaderResources* pR = (CShaderResources*)rShaderItem.m_pShaderResources;

    // skip some objects marked by level designer
    //  if(pRN && pRN->IsRenderNode() && pRN->GetIntegrationType())
    //  return false;

    // skip transparent meshes except decals
    //  if((pR->Opacity() != 1.f) && !(pS->GetFlags()&EF_DECAL))
    //  return false;

    // skip windows
    if (pS && pS->GetShaderType() == eST_Glass)
    {
        return false;
    }

    // skip water
    if (pS && pS->GetShaderType() == eST_Water)
    {
        return false;
    }

    // skip outdoor vegetations
    //  (fog not working) if(pS->GetShaderType() == eST_Vegetation && pRN->IsRenderNode() && !pRN->GetEntityVisArea())
    //  return false;

    return true;
}

namespace
{
    const float ACTIVATION_MODE_DISABLED = -1.0f;

    float GetActivationModeAsFloat()
    {
        auto* instance = CSvoRenderer::GetInstance();

        float modeAsFloat = ACTIVATION_MODE_DISABLED;
        int32 modeAsInt = instance->GetIntegratioMode();

        if (instance->IsActive())
        {
            if (modeAsInt == 0 && instance->GetUseLightProbes())
            { // AO modulates diffuse and specular
                modeAsFloat = 0.0f;
            }
            else if (modeAsInt <= 1)
            { // GI replaces diffuse and modulates specular
                modeAsFloat = 1.0f;
            }
            else if (modeAsInt == 2)
            { // GI replaces diffuse and specular
                modeAsFloat = 2.0f;
            }
        }

        return modeAsFloat;
    }
}

void CSvoRenderer::UpdatePassConstantBuffer()
{
    CSvoRenderer* svoRenderer = CSvoRenderer::GetInstance();

    CTypedConstantBuffer<HLSL_PerPassConstantBuffer_Svo> cb(m_PassConstantBuffer);

    // SvoParams4 is used by forward tiled shaders even if GI is disabled, it contains info about GI mode and also is GI active or not
    cb->PerPass_SvoParams4 = Vec4(0.0f, -1.0f, 0.0f, 0.0f);

    if (svoRenderer)
    {
        cb->PerPass_SvoTreeSettings0 = Vec4(
            (float)gEnv->p3DEngine->GetTerrainSize(),
            svoRenderer->e_svoMinNodeSize,
            (float)svoRenderer->m_texInfo.nBrickSize,
            (float)svoRenderer->e_svoVoxelPoolResolution);

        cb->PerPass_SvoTreeSettings1 = Vec4(
            svoRenderer->e_svoMaxNodeSize,
            svoRenderer->e_svoTI_RT_MaxDist,
            svoRenderer->e_svoTI_Troposphere_CloudGen_FreqStep,
            svoRenderer->e_svoTI_Troposphere_Density);

        cb->PerPass_SvoTreeSettings2 = Vec4(
            svoRenderer->e_svoTI_Troposphere_Ground_Height,
            svoRenderer->e_svoTI_Troposphere_Layer0_Height,
            svoRenderer->e_svoTI_Troposphere_Layer1_Height,
            svoRenderer->e_svoTI_Troposphere_CloudGen_Height);

        cb->PerPass_SvoTreeSettings3 = Vec4(
            svoRenderer->e_svoTI_Troposphere_CloudGen_Freq,
            svoRenderer->e_svoTI_Troposphere_CloudGen_Scale,
            svoRenderer->e_svoTI_Troposphere_CloudGenTurb_Freq,
            svoRenderer->e_svoTI_Troposphere_CloudGenTurb_Scale);

        cb->PerPass_SvoTreeSettings4 = Vec4(
            svoRenderer->e_svoTI_Troposphere_Layer0_Rand,
            svoRenderer->e_svoTI_Troposphere_Layer1_Rand,
            svoRenderer->e_svoTI_Troposphere_Layer0_Dens,
            svoRenderer->e_svoTI_Troposphere_Layer1_Dens);

        cb->PerPass_SvoTreeSettings5 = Vec4(
            svoRenderer->e_svoTI_PortalsInject,
            svoRenderer->e_svoTI_PortalsDeform,
            svoRenderer->m_texInfo.vSkyColorTop.x,
            svoRenderer->m_texInfo.vSkyColorTop.y);

        cb->PerPass_SvoParams0 = Vec4(
            svoRenderer->e_svoTI_Shadow_Sev,
            svoRenderer->e_svoTI_Diffuse_Spr,
            svoRenderer->e_svoTI_DiffuseBias,
            0.0f);

        cb->PerPass_SvoParams1 = Vec4(
            1.f / (svoRenderer->e_svoTI_DiffuseConeWidth + 0.00001f),
            svoRenderer->e_svoTI_ConeMaxLength,
            1.f / max(svoRenderer->e_svoTI_SSDepthTrace, 0.01f),
            (svoRenderer->m_texInfo.bSvoReady && svoRenderer->e_svoTI_NumberOfBounces) ? svoRenderer->e_svoTI_DiffuseAmplifier : 0);

        cb->PerPass_SvoParams2 = Vec4(
            svoRenderer->e_svoTI_InjectionMultiplier,
            1.f / max(svoRenderer->e_svoTI_PropagationBooster, 0.001f),
            svoRenderer->e_svoTI_Saturation,
            svoRenderer->e_svoTI_TranslucentBrightness);

        cb->PerPass_SvoParams3 = Vec4(
            svoRenderer->e_svoTI_Specular_Sev,
            svoRenderer->m_texInfo.vSkyColorTop.z,
            svoRenderer->e_svoTI_Troposphere_Brightness * svoRenderer->e_svoTI_Troposphere_Active,
            (svoRenderer->m_texInfo.bSvoReady && svoRenderer->e_svoTI_NumberOfBounces) ? svoRenderer->e_svoTI_SpecularAmplifier : 0);

        // LOD & Sky color
        {
            cb->PerPass_SvoParams4.x = 1.f - svoRenderer->e_svoTI_HighGlossOcclusion;
            cb->PerPass_SvoParams4.y = GetActivationModeAsFloat();
            cb->PerPass_SvoParams4.z = (float)svoRenderer->e_svoDVR;
            cb->PerPass_SvoParams4.w = svoRenderer->e_svoTI_SkyColorMultiplier;
        }

        cb->PerPass_SvoParams5 = Vec4(
            svoRenderer->e_svoTI_EmissiveMultiplier,
            0.0f,
            svoRenderer->e_svoTI_NumberOfBounces - 1.f,
            svoRenderer->m_texInfo.fGlobalSpecCM_Mult);

        cb->PerPass_SvoParams6 = Vec4(
            svoRenderer->e_svoTI_PointLightsMultiplier,
            gEnv->IsEditing() ? 0 : (svoRenderer->e_svoTI_TemporalFilteringMinDistance / gcpRendD3D->GetViewParameters().fFar),
            0.0f,
            0.0f);
    }

    m_PassConstantBuffer = cb.GetDeviceConstantBuffer();
    cb.CopyToDevice();

    auto& devManager = gcpRendD3D->m_DevMan;
    devManager.BindConstantBuffer(eHWSC_Compute, m_PassConstantBuffer.get(), eConstantBufferShaderSlot_PerPass);
    devManager.BindConstantBuffer(eHWSC_Pixel, m_PassConstantBuffer.get(), eConstantBufferShaderSlot_PerPass);
}

Vec4 CSvoRenderer::GetDisabledPerFrameShaderParameters()
{
    Vec4 params;
    params.x = 0.0f;
    params.y = ACTIVATION_MODE_DISABLED;
    params.z = 0.0f;
    params.z = 0.0f;
    return params;
}

Vec4 CSvoRenderer::GetPerFrameShaderParameters() const
{
    Vec4 params;
    params.x = 1.f - CSvoRenderer::GetInstance()->e_svoTI_HighGlossOcclusion;
    params.y = GetActivationModeAsFloat();
    params.z = 0.0f;
    params.w = 0.0f;
    return params;
}

void CSvoRenderer::DebugDrawStats(const RPProfilerStats* pBasicStats, float& ypos, const float ystep, float xposms)
{
    ColorF color = Col_Yellow;
    const EDrawTextFlags  txtFlags = (EDrawTextFlags)(eDrawText_2D | eDrawText_800x600 | eDrawText_FixedSize | eDrawText_Monospace);

#define SVO_Draw2dLabel(labelName)                                                                                        \
    gRenDev->Draw2dLabel(60, ypos += ystep, 2, &color.r, false, (const char*)(((const char*)(#labelName)) + 10));         \
    if (pBasicStats[labelName].gpuTimeMax > 0.01) {                                                                       \
        gRenDev->Draw2dLabelEx(xposms, ypos, 2, color, txtFlags, "%5.2f Aver=%5.2f Max=%5.2f",                            \
            pBasicStats[labelName].gpuTime, pBasicStats[labelName].gpuTimeSmoothed, pBasicStats[labelName].gpuTimeMax); } \
    else{                                                                                                                 \
        gRenDev->Draw2dLabelEx(xposms, ypos, 2, color, txtFlags, "%5.2f", pBasicStats[labelName].gpuTime); }              \

    SVO_Draw2dLabel(eRPPSTATS_TI_INJECT_CLEAR);
    SVO_Draw2dLabel(eRPPSTATS_TI_VOXELIZE);
    SVO_Draw2dLabel(eRPPSTATS_TI_INJECT_AIR);
    SVO_Draw2dLabel(eRPPSTATS_TI_INJECT_LIGHT);
    SVO_Draw2dLabel(eRPPSTATS_TI_INJECT_REFL0);
    SVO_Draw2dLabel(eRPPSTATS_TI_INJECT_REFL1);
    SVO_Draw2dLabel(eRPPSTATS_TI_INJECT_DYNL);
    SVO_Draw2dLabel(eRPPSTATS_TI_NID_DIFF);
    SVO_Draw2dLabel(eRPPSTATS_TI_GEN_DIFF);
    SVO_Draw2dLabel(eRPPSTATS_TI_GEN_SPEC);
    SVO_Draw2dLabel(eRPPSTATS_TI_GEN_AIR);
    SVO_Draw2dLabel(eRPPSTATS_TI_DEMOSAIC_DIFF);
    SVO_Draw2dLabel(eRPPSTATS_TI_DEMOSAIC_SPEC);
    SVO_Draw2dLabel(eRPPSTATS_TI_UPSCALE_DIFF);
    SVO_Draw2dLabel(eRPPSTATS_TI_UPSCALE_SPEC);
}

void CSvoRenderer::SetShaderFlags(bool bDiffuseMode, bool bPixelShader)
{
    if (e_svoTI_LowSpecMode > 0) // simplify shaders
    {
        gRenDev->m_RP.m_FlagsShader_RT |= g_HWSR_MaskBit[HWSR_SAMPLE0];
    }
    else
    {
        gRenDev->m_RP.m_FlagsShader_RT &= ~g_HWSR_MaskBit[HWSR_SAMPLE0];
    }

#ifdef FEATURE_SVO_GI_ALLOW_HQ
    if (m_texInfo.pGlobalSpecCM && GetIntegratioMode()) // use global env CM
    {
        gRenDev->m_RP.m_FlagsShader_RT |= g_HWSR_MaskBit[HWSR_SAMPLE1];
    }
    else
#endif
    gRenDev->m_RP.m_FlagsShader_RT &= ~g_HWSR_MaskBit[HWSR_SAMPLE1];

    if (e_svoTI_Troposphere_Active) // compute air lighting as well
    {
        gRenDev->m_RP.m_FlagsShader_RT |= g_HWSR_MaskBit[HWSR_SAMPLE2];
    }
    else
    {
        gRenDev->m_RP.m_FlagsShader_RT &= ~g_HWSR_MaskBit[HWSR_SAMPLE2];
    }

    if (e_svoTI_Diffuse_Cache) // use pre-baked lighting
    {
        gRenDev->m_RP.m_FlagsShader_RT |= g_HWSR_MaskBit[HWSR_SAMPLE3];
    }
    else
    {
        gRenDev->m_RP.m_FlagsShader_RT &= ~g_HWSR_MaskBit[HWSR_SAMPLE3];
    }

    if (bDiffuseMode) // diffuse or specular rendering
    {
        gRenDev->m_RP.m_FlagsShader_RT |= g_HWSR_MaskBit[HWSR_SAMPLE4];
    }
    else
    {
        gRenDev->m_RP.m_FlagsShader_RT &= ~g_HWSR_MaskBit[HWSR_SAMPLE4];
    }

    if (GetIntegratioMode()) // ignore colors and normals for AO only mode
    {
        gRenDev->m_RP.m_FlagsShader_RT |= g_HWSR_MaskBit[HWSR_SAMPLE5];
    }
    else
    {
        gRenDev->m_RP.m_FlagsShader_RT &= ~g_HWSR_MaskBit[HWSR_SAMPLE5];
    }

    if (e_svoTI_HalfresKernel) // smaller kernel - less de-mosaic work
    {
        gRenDev->m_RP.m_FlagsShader_RT |= g_HWSR_MaskBit[HWSR_HW_PCF_COMPARE];
    }
    else
    {
        gRenDev->m_RP.m_FlagsShader_RT &= ~g_HWSR_MaskBit[HWSR_HW_PCF_COMPARE];
    }

    if (bPixelShader && !GetIntegratioMode() && e_svoTI_InjectionMultiplier) // read sun light and shadow map during final cone tracing
    {
        gRenDev->m_RP.m_FlagsShader_RT |= g_HWSR_MaskBit[HWSR_LIGHT_TEX_PROJ];
    }
    else
    {
        gRenDev->m_RP.m_FlagsShader_RT &= ~g_HWSR_MaskBit[HWSR_LIGHT_TEX_PROJ];
    }

    if (bPixelShader && !GetIntegratioMode() && e_svoTI_InjectionMultiplier && m_arrLightsDynamic.Count()) // read sun light and shadow map during final cone tracing
    {
        gRenDev->m_RP.m_FlagsShader_RT |= g_HWSR_MaskBit[HWSR_POINT_LIGHT];
    }
    else
    {
        gRenDev->m_RP.m_FlagsShader_RT &= ~g_HWSR_MaskBit[HWSR_POINT_LIGHT];
    }

    if (bPixelShader && e_svoTI_SSDepthTrace) // SS depth trace
    {
        gRenDev->m_RP.m_FlagsShader_RT |= g_HWSR_MaskBit[HWSR_BLEND_WITH_TERRAIN_COLOR];
    }
    else
    {
        gRenDev->m_RP.m_FlagsShader_RT &= ~g_HWSR_MaskBit[HWSR_BLEND_WITH_TERRAIN_COLOR];
    }
}

int CSvoRenderer::GetIntegratioMode()
{
    return e_svoTI_IntegrationMode;
}

int CSvoRenderer::GetIntegratioMode(bool& bSpecTracingInUse)
{
    bSpecTracingInUse = (e_svoTI_SpecularAmplifier != 0);
    return e_svoTI_IntegrationMode;
}

bool CSvoRenderer::IsActive()
{
    static ICVar* e_GI          = gEnv->pConsole->GetCVar("e_GI");
    return e_GI->GetIVal() && CSvoRenderer::s_pInstance && CSvoRenderer::s_pInstance->e_svoTI_Apply;
}

void CSvoRenderer::InitCVarValues()
{
#define INIT_SVO_CVAR(_type, _var)                                 \
    if (strstr(#_type, "float")) {                                 \
        _var = (_type)gEnv->pConsole->GetCVar(#_var)->GetFVal(); } \
    else{                                                          \
        _var = (_type)gEnv->pConsole->GetCVar(#_var)->GetIVal(); } \

    INIT_ALL_SVO_CVARS;
#undef INIT_SVO_CVAR
}

bool CSvoRenderer::SetSamplers(int nCustomID, EHWShaderClass eSHClass, int nTUnit, int nTState, int nTexMaterialSlot, int nSUnit)
{
    CSvoRenderer* pSR = CSvoRenderer::GetInstance();

    if (!pSR)
    {
        return false;
    }

    switch (nCustomID)
    {
    case TO_SVOTREE:
    case TO_SVOTRIS:
    case TO_SVOGLCM:
    case TO_SVORGBS:
    case TO_SVONORM:
    case TO_SVOOPAC:
    {
        CTexture* pTex = CTexture::s_ptexBlack;

        if (pSR->m_texInfo.pTexTree)
        {
            if (nCustomID == TO_SVOTREE)
            {
                nCustomID = pSR->m_texInfo.pTexTree->GetTextureID();
            }
#ifdef FEATURE_SVO_GI_ALLOW_HQ
            if (nCustomID == TO_SVOTRIS)
            {
                nCustomID = pSR->m_texInfo.pTexTris->GetTextureID();
            }
            if (nCustomID == TO_SVOGLCM)
            {
                nCustomID = pSR->m_texInfo.pGlobalSpecCM->GetTextureID();
            }
            if (nCustomID == TO_SVORGBS)
            {
                if (pSR->e_svoDVR == 3 && eSHClass == eHWSC_Pixel && pSR->m_texInfo.pTexRgb1)
                {
                    nCustomID = pSR->m_texInfo.pTexRgb1->GetTextureID();
                }
                else if (pSR->e_svoDVR == 4 && eSHClass == eHWSC_Pixel  && pSR->m_texInfo.pTexRgb2)
                {
                    nCustomID = pSR->m_texInfo.pTexRgb2->GetTextureID();
                }
                else if (pSR->e_svoDVR == 5 && eSHClass == eHWSC_Pixel && pSR->m_texInfo.pTexRgb3)
                {
                    nCustomID = pSR->m_texInfo.pTexRgb3->GetTextureID();
                }
                else if (pSR->e_svoDVR == 6 && eSHClass == eHWSC_Pixel && pSR->m_texInfo.pTexRgb4)
                {
                    nCustomID = pSR->m_texInfo.pTexRgb4->GetTextureID();
                }
                else if (pSR->e_svoDVR == 7 && eSHClass == eHWSC_Pixel && pSR->m_texInfo.pTexAldi)
                {
                    nCustomID = pSR->m_texInfo.pTexAldi->GetTextureID();
                }
                else if (pSR->e_svoDVR == 8 && eSHClass == eHWSC_Pixel && pSR->m_texInfo.pTexDynl)
                {
                    nCustomID = pSR->m_texInfo.pTexDynl->GetTextureID();
                }
                else if (pSR->m_texInfo.pTexRgb0)
                {
                    nCustomID = pSR->m_texInfo.pTexRgb0->GetTextureID();
                }
            }
            if (nCustomID == TO_SVONORM)
            {
                nCustomID = pSR->m_texInfo.pTexNorm->GetTextureID();
            }
#endif
            if (nCustomID == TO_SVOOPAC)
            {
                nCustomID = pSR->m_texInfo.pTexOpac->GetTextureID();
            }

            if (nCustomID > 0)
            {
                pTex = CTexture::GetByID(nCustomID);
            }
        }

        pTex->Apply(nTUnit, nTState, nTexMaterialSlot, nSUnit, -1, eSHClass);

        return true;
    }
    }

    return false;
}

CTexture* CSvoRenderer::GetTroposphereMinRT()
{
#ifdef FEATURE_SVO_GI_ALLOW_HQ
    if (m_pRT_AIR_MIN && m_pRT_AIR_MIN && ((m_pRT_AIR_MIN)->m_nUpdateFrameID > (gRenDev->GetFrameID(false) - 4)))
    {
        return m_pRT_AIR_MIN;
    }
#endif
    return NULL;
}

CTexture* CSvoRenderer::GetTroposphereMaxRT()
{
#ifdef FEATURE_SVO_GI_ALLOW_HQ
    if (m_pRT_AIR_MAX && m_pRT_AIR_MAX && ((m_pRT_AIR_MAX)->m_nUpdateFrameID > (gRenDev->GetFrameID(false) - 4)))
    {
        return m_pRT_AIR_MAX;
    }
#endif
    return NULL;
}

CTexture* CSvoRenderer::GetTroposphereShadRT()
{
#ifdef FEATURE_SVO_GI_ALLOW_HQ
    if (m_pRT_AIR_SHAD && m_pRT_AIR_SHAD && ((m_pRT_AIR_SHAD)->m_nUpdateFrameID > (gRenDev->GetFrameID(false) - 4)))
    {
        return m_pRT_AIR_SHAD;
    }
#endif
    return NULL;
}

CTexture* CSvoRenderer::GetDiffuseFinRT()
{
    return m_tsDiff.pRT_FIN_OUT_0;
}

CTexture* CSvoRenderer::GetSpecularFinRT()
{
    return m_tsSpec.pRT_FIN_OUT_0;
}

void CSvoRenderer::UpScalePass(SSvoTargetsSet* pTS)
{

    const char* szTechFinalName = "UpScalePass";

    if (!e_svoTI_Active || !e_svoTI_Apply || !e_svoRender || !m_pShader)
    {
        return;
    }

    CD3D9Renderer* const __restrict rd = gcpRendD3D;

    rd->FX_PushRenderTarget(0, pTS->pRT_FIN_OUT_0, NULL, -1, false, 1);

    SetShaderFlags(pTS == &m_tsDiff);

    SD3DPostEffectsUtils::ShBeginPass(m_pShader, szTechFinalName, FEF_DONTSETTEXTURES);

    if (!gRenDev->m_RP.m_pShader)
    {
        gEnv->pLog->LogWarning("Error: %s: Technique not found: %s", __FUNC__, szTechFinalName);
        rd->FX_PopRenderTarget(0);
        return;
    }

    CTexture::s_ptexZTarget->Apply(4, m_nTexStatePoint);
    GetGBuffer(1)->Apply(5, m_nTexStatePoint);
    GetGBuffer(2)->Apply(7, m_nTexStatePoint);
    GetGBuffer(0)->Apply(14, m_nTexStatePoint);

    pTS->pRT_ALD_DEM_MIN_0->Apply(10, m_nTexStatePoint);
    pTS->pRT_RGB_DEM_MIN_0->Apply(11, m_nTexStatePoint);
    pTS->pRT_ALD_DEM_MAX_0->Apply(12, m_nTexStatePoint);
    pTS->pRT_RGB_DEM_MAX_0->Apply(13, m_nTexStatePoint);

    pTS->pRT_FIN_OUT_1->Apply(9, m_nTexStatePoint);

    if (pTS == &m_tsSpec && m_tsDiff.pRT_FIN_OUT_0)
    {
        m_tsDiff.pRT_FIN_OUT_0->Apply(15, m_nTexStatePoint);
    }
    else
    {
        CTexture::s_ptexBlack->Apply(15, m_nTexStatePoint);
    }

    {
        static CCryNameR parameterName6("SVO_SrcPixSize");
        Vec4 ttt(0, 0, 0, 0);
        ttt.x = 1.f / float(pTS->pRT_ALD_DEM_MIN_0->GetWidth());
        ttt.y = 1.f / float(pTS->pRT_ALD_DEM_MIN_0->GetHeight());
        m_pShader->FXSetPSFloat(parameterName6, (Vec4*)&ttt, 1);
    }

    {
        static CCryNameR parameterName5("SVO_ReprojectionMatrix");
        m_pShader->FXSetPSFloat(parameterName5, (Vec4*)m_matReproj.GetData(), 3);
    }

    {
        float fSizeRatioW = float(rd->GetWidth() / rd->m_RTStack[0][rd->m_nRTStackLevel[0]].m_Width);
        float fSizeRatioH = float(rd->GetHeight() / rd->m_RTStack[0][rd->m_nRTStackLevel[0]].m_Height);
        static CCryNameR parameterName6("SVO_TargetResScale");
        static int nPrevWidth = 0;
        Vec4 ttt(fSizeRatioW, fSizeRatioH, e_svoTI_TemporalFilteringBase,
            (float)(nPrevWidth != (pTS->pRT_ALD_0->GetWidth() + int(e_svoTI_SkyColorMultiplier > 0)) || (rd->m_RP.m_nRendFlags & SHDF_CUBEMAPGEN) || (rd->GetActiveGPUCount() > 1)));
        m_pShader->FXSetPSFloat(parameterName6, (Vec4*)&ttt, 1);
        nPrevWidth = (pTS->pRT_ALD_0->GetWidth() + int(e_svoTI_SkyColorMultiplier > 0));
    }

    {
        Matrix44A mViewProj;
        mViewProj = gcpRendD3D->m_ViewProjMatrix;
        mViewProj.Transpose();

        static CCryNameR paramName("g_mViewProj");
        m_pShader->FXSetPSFloat(paramName, alias_cast<Vec4*>(&mViewProj), 4);

        static CCryNameR paramNamePrev("g_mViewProjPrev");
        m_pShader->FXSetPSFloat(paramNamePrev, alias_cast<Vec4*>(&m_matViewProjPrev), 4);
    }

    SD3DPostEffectsUtils::DrawFullScreenTriWPOS(CTexture::s_ptexCurrentSceneDiffuseAccMap->GetWidth(), CTexture::s_ptexCurrentSceneDiffuseAccMap->GetHeight());

    SD3DPostEffectsUtils::ShEndPass();

    rd->FX_PopRenderTarget(0);
}

void CSvoRenderer::SetupRsmSun(const EHWShaderClass eShClass)
{
    CD3D9Renderer* const __restrict rd = gcpRendD3D;

    int nLightID = 0;

    threadID m_nThreadID = gcpRendD3D->m_RP.m_nProcessThreadID;
    int m_nRecurseLevel = SRendItem::m_RecurseLevel[m_nThreadID];

    int nDLights = rd->m_RP.m_DLights[m_nThreadID][m_nRecurseLevel].Num();
    int nFrustumIdx = nLightID;// + nDLights;

    int nStartIdx = SRendItem::m_StartFrust[m_nThreadID][nFrustumIdx];
    int nEndIdx = SRendItem::m_EndFrust[m_nThreadID][nFrustumIdx];

    static CCryNameR lightProjParamName("SVO_RsmSunShadowProj");
    static CCryNameR rsmSunColParameterName("SVO_RsmSunCol");
    static CCryNameR rsmSunDirParameterName("SVO_RsmSunDir");
    Matrix44A shadowMat;
    shadowMat.SetIdentity();

    int nFrIdx = 0;
    for (nFrIdx = nStartIdx; nFrIdx < nEndIdx; nFrIdx++)
    {
        ShadowMapFrustum& firstFrustum = rd->m_RP.m_SMFrustums[m_nThreadID][m_nRecurseLevel][nFrIdx];
        if (firstFrustum.nShadowMapLod == e_svoTI_GsmCascadeLod)
        {
            break;
        }
    }

    if ((nEndIdx > nFrIdx) && GetRsmColorMap(rd->m_RP.m_SMFrustums[m_nThreadID][m_nRecurseLevel][nFrIdx]))
    {
        ShadowMapFrustum& firstFrustum = rd->m_RP.m_SMFrustums[m_nThreadID][m_nRecurseLevel][nFrIdx];
        rd->ConfigShadowTexgen(0, &firstFrustum, 0);

        if (firstFrustum.bUseShadowsPool)
        {
            STexState TS;
            TS.SetFilterMode(FILTER_POINT);
            TS.SetClampMode(TADDR_CLAMP, TADDR_CLAMP, TADDR_CLAMP);
            TS.m_bSRGBLookup = false;
            CTexture::s_ptexRT_ShadowPool->Apply(12, CTexture::GetTexState(TS), EFTT_UNKNOWN, -1, -1, eShClass);
        }
        else
        {
            firstFrustum.pDepthTex->Apply(12, m_nTexStatePoint, EFTT_UNKNOWN, -1, -1, eShClass);
            GetRsmColorMap(firstFrustum)->Apply(13, m_nTexStatePoint, EFTT_UNKNOWN, -1, -1, eShClass);
            GetRsmNormlMap(firstFrustum)->Apply(9, m_nTexStatePoint, EFTT_UNKNOWN, -1, -1, eShClass);
        }

        // set up shadow matrix
        shadowMat = gRenDev->m_TempMatrices[0][0];
        const Vec4 vEye(gRenDev->GetViewParameters().vOrigin, 0.f);
        Vec4 vecTranslation(vEye.Dot((Vec4&)shadowMat.m00), vEye.Dot((Vec4&)shadowMat.m10), vEye.Dot((Vec4&)shadowMat.m20), vEye.Dot((Vec4&)shadowMat.m30));
        shadowMat.m03 += vecTranslation.x;
        shadowMat.m13 += vecTranslation.y;
        shadowMat.m23 += vecTranslation.z;
        shadowMat.m33 += vecTranslation.w;
        (Vec4&)shadowMat.m20 *= gRenDev->m_cEF.m_TempVecs[2].x;
        SetShaderFloat(eShClass, lightProjParamName, alias_cast<Vec4*>(&shadowMat), 4);

        Vec4 ttt(gEnv->p3DEngine->GetSunColor(), e_svoTI_InjectionMultiplier);
        SetShaderFloat(eShClass, rsmSunColParameterName, (Vec4*)&ttt, 1);
        Vec4 ttt2(gEnv->p3DEngine->GetSunDirNormalized(), (float)e_svoTI_SunRSMInject);
        SetShaderFloat(eShClass, rsmSunDirParameterName, (Vec4*)&ttt2, 1);
    }
    else
    {
        CTexture::s_ptexBlack->Apply(12, m_nTexStatePoint, EFTT_UNKNOWN, -1, -1, eShClass);
        CTexture::s_ptexBlack->Apply(13, m_nTexStatePoint, EFTT_UNKNOWN, -1, -1, eShClass);
        CTexture::s_ptexBlack->Apply(9, m_nTexStatePoint, EFTT_UNKNOWN, -1, -1, eShClass);
        SetShaderFloat(eShClass, lightProjParamName, alias_cast<Vec4*>(&shadowMat), 4);

        Vec4 ttt(0, 0, 0, 0);
        SetShaderFloat(eShClass, rsmSunColParameterName, (Vec4*)&ttt, 1);
        Vec4 ttt2(0, 0, 0, 0);
        SetShaderFloat(eShClass, rsmSunDirParameterName, (Vec4*)&ttt2, 1);
    }
}

ISvoRenderer* CD3D9Renderer::GetISvoRenderer()
{
    return CSvoRenderer::GetInstance(true);
}

void CSvoRenderer::SetShaderFloat(const EHWShaderClass eShClass, const CCryNameR& NameParam, const Vec4* fParams, int nParams)
{
    if (eShClass == eHWSC_Pixel)
    {
        m_pShader->FXSetPSFloat(NameParam, fParams, nParams);
    }
    else if (eShClass == eHWSC_Compute)
    {
        m_pShader->FXSetCSFloat(NameParam, fParams, nParams);
    }
    else if (eShClass == eHWSC_Vertex)
    {
        m_pShader->FXSetVSFloat(NameParam, fParams, nParams);
    }
}

void CSvoRenderer::BindTiledLights(PodArray<I3DEngine::SLightTI>& lightsTI, EHWShaderClass shaderType)
{
    gcpRendD3D->GetTiledShading().BindForwardShadingResources(NULL, shaderType);

    STiledLightShadeInfo* tiledLightShadeInfo = gcpRendD3D->GetTiledShading().GetTiledLightShadeInfo();

    for (int l = 0; l < lightsTI.Count(); l++)
    {
        I3DEngine::SLightTI& svoLight = lightsTI[l];

        if (!svoLight.vDirF.w)
        {
            continue;
        }

        const int tlTypeRegularProjector = 6;

        Vec4 worldViewPos = Vec4 (gcpRendD3D->GetViewParameters().vOrigin, 0);

        for (uint32 lightIdx = 0; lightIdx <= 255 && tiledLightShadeInfo[lightIdx].posRad != Vec4(0, 0, 0, 0); ++lightIdx)
        {
            if ((tiledLightShadeInfo[lightIdx].lightType == tlTypeRegularProjector) && svoLight.vPosR.IsEquivalent(tiledLightShadeInfo[lightIdx].posRad + worldViewPos, .5f))
            {
                if (svoLight.vCol.w > 0)
                {
                    svoLight.vCol.w = ((float)lightIdx + 100);
                }
                else
                {
                    svoLight.vCol.w = -((float)lightIdx + 100);
                }
            }
        }
    }
}

CTexture* CSvoRenderer::GetRsmColorMap(ShadowMapFrustum& rFr, bool bCheckUpdate)
{
    if (IsActive() && CSvoRenderer::GetInstance()->e_svoTI_GsmShiftBack && (rFr.nShadowMapLod == CSvoRenderer::GetInstance()->e_svoTI_GsmCascadeLod) && CSvoRenderer::GetInstance()->e_svoTI_InjectionMultiplier)
    {
        if (bCheckUpdate)
        {
            CSvoRenderer::GetInstance()->CheckCreateUpdateRT(CSvoRenderer::GetInstance()->m_pRsmColorMap, rFr.nShadowMapSize, rFr.nShadowMapSize, eTF_R8G8B8A8, eTT_2D,  FT_STATE_CLAMP, "SVO_SUN_RSM_COLOR");
        }

        return CSvoRenderer::GetInstance()->m_pRsmColorMap;
    }

    if (IsActive() && rFr.bUseShadowsPool && CSvoRenderer::GetInstance()->e_svoTI_InjectionMultiplier && rFr.m_Flags & DLF_USE_FOR_SVOGI)
    {
        if (bCheckUpdate)
        {
            CSvoRenderer::GetInstance()->CheckCreateUpdateRT(CSvoRenderer::GetInstance()->m_pRsmPoolCol, gcpRendD3D->m_nShadowPoolWidth, gcpRendD3D->m_nShadowPoolHeight, eTF_R8G8B8A8, eTT_2D,  FT_STATE_CLAMP, "SVO_PRJ_RSM_COLOR");
        }

        return CSvoRenderer::GetInstance()->m_pRsmPoolCol;
    }

    return NULL;
}

CTexture* CSvoRenderer::GetRsmNormlMap(ShadowMapFrustum& rFr, bool bCheckUpdate)
{
    if (IsActive() && CSvoRenderer::GetInstance()->e_svoTI_GsmShiftBack && (rFr.nShadowMapLod == CSvoRenderer::GetInstance()->e_svoTI_GsmCascadeLod) && CSvoRenderer::GetInstance()->e_svoTI_InjectionMultiplier)
    {
        if (bCheckUpdate)
        {
            CSvoRenderer::GetInstance()->CheckCreateUpdateRT(CSvoRenderer::GetInstance()->m_pRsmNormlMap, rFr.nShadowMapSize, rFr.nShadowMapSize, eTF_R8G8B8A8, eTT_2D,  FT_STATE_CLAMP, "SVO_SUN_RSM_NORMAL");
        }

        return CSvoRenderer::GetInstance()->m_pRsmNormlMap;
    }

    if (IsActive() && rFr.bUseShadowsPool && CSvoRenderer::GetInstance()->e_svoTI_InjectionMultiplier && rFr.m_Flags & DLF_USE_FOR_SVOGI)
    {
        if (bCheckUpdate)
        {
            CSvoRenderer::GetInstance()->CheckCreateUpdateRT(CSvoRenderer::GetInstance()->m_pRsmPoolNor, gcpRendD3D->m_nShadowPoolWidth, gcpRendD3D->m_nShadowPoolHeight, eTF_R8G8B8A8, eTT_2D,  FT_STATE_CLAMP, "SVO_PRJ_RSM_NORMAL");
        }

        return CSvoRenderer::GetInstance()->m_pRsmPoolNor;
    }

    return NULL;
}

void CSvoRenderer::CheckCreateUpdateRT(CTexture*& pTex, int nWidth, int nHeight, ETEX_Format eTF, ETEX_Type eTT, int nTexFlags, const char* szName)
{
    if ((!pTex) || (pTex->GetWidth() != nWidth) || (pTex->GetHeight() != nHeight) || (pTex->GetTextureDstFormat() != eTF))
    {
        SAFE_RELEASE(pTex);

        char szNameEx[256] = "";
        sprintf_s(szNameEx, "%s_%d_%d", szName, nWidth, nHeight); // workaround for RT management bug

        SD3DPostEffectsUtils::CreateRenderTarget(szNameEx, pTex, nWidth, nHeight, Clr_Unknown, false, false, eTF);

        pTex->DisableMgpuSync();

        //iLog->Log("Realloc RT %dx%d, %s, %s", nWidth, nHeight, CTexture::NameForTextureFormat(eTF), szName);
    }
}

#ifdef FEATURE_SVO_GI_ALLOW_HQ

void CSvoRenderer::SVoxPool::Init(ITexture* _pTex)
{
    CTexture* pTex = (CTexture*)_pTex;

    if (pTex)
    {
        pUAV = (D3DUnorderedAccessView*)((CTexture*)pTex)->GetDeviceUAV();
        nTexId = pTex->GetTextureID();
    }
}

#endif

#endif
