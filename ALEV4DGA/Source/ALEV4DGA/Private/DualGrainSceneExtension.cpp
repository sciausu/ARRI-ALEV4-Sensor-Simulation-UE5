#include "DualGrainSceneExtension.h"
#include "SceneView.h"
#include "RenderGraph.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "GlobalShader.h"
#include "PostProcess/PostProcessInputs.h"
#include "CineCameraActor.h"

#if WITH_EDITOR
#include "Editor.h"
#include "LevelEditorViewport.h"
#endif

// Console variable to toggle the simulation on/off at runtime
static TAutoConsoleVariable<int32> CVarALEV4DGAEnabled(
    TEXT("r.ALEV4DGA.Enabled"),
    1,
    TEXT("Enable the ARRI ALEV4 DGA simulation.\n")
    TEXT("  0: Disabled\n")
    TEXT("  1: Enabled (default)"),
    ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarALEV4DGAHighGain(
    TEXT("r.ALEV4DGA.HighGain"),
    4.0f,
    TEXT("High-gain path amplification (shadow path).\n")
    TEXT("Both paths are normalised to a common photometric scale, so this\n")
    TEXT("does NOT brighten the image; it sets where the high-gain read\n")
    TEXT("saturates (FullWell / HighGain in scene units). Default: 4.0"),
    ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarALEV4DGALowGain(
    TEXT("r.ALEV4DGA.LowGain"),
    1.0f,
    TEXT("Low-gain path amplification (highlight path).\n")
    TEXT("Sets the highlight clip point of the merged image:\n")
    TEXT("FullWell / LowGain in scene units. Default: 1.0"),
    ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarALEV4DGAShadowThreshold(
    TEXT("r.ALEV4DGA.ShadowThreshold"),
    0.18f,
    TEXT("TRUE scene luminance below which the merge is fully on the\n")
    TEXT("high-gain path (pre-exposure is removed before this is applied).\n")
    TEXT("0.18 corresponds to 18%% middle grey reflectance. Default: 0.18"),
    ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarALEV4DGAHighlightThreshold(
    TEXT("r.ALEV4DGA.HighlightThreshold"),
    0.5f,
    TEXT("TRUE scene luminance above which the merge is fully on the\n")
    TEXT("low-gain path. Must be greater than ShadowThreshold. Default: 0.5"),
    ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarALEV4DGAExposureOffset(
    TEXT("r.ALEV4DGA.ExposureOffset"),
    0.0f,
    TEXT("Test exposure offset in stops, applied to true scene light before\n")
    TEXT("the simulation. Use this (not the gain CVars) to push exposure up\n")
    TEXT("or down while testing highlight retention. Default: 0.0"),
    ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarALEV4DGAFullWell(
    TEXT("r.ALEV4DGA.FullWell"),
    460.0f,
    TEXT("Simulated ADC full-scale in amplified units. Each gain path clips\n")
    TEXT("at FullWell / gain in scene-linear units (mid grey = 0.18).\n")
    TEXT("Default 460 puts low-gain clip ~11.3 stops above mid grey, just\n")
    TEXT("inside the LogC4 encodable maximum (~469.8). Default: 460.0"),
    ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarALEV4DGALogC4Enabled(
    TEXT("r.ALEV4DGA.LogC4"),
    1,
    TEXT("Apply ARRI LogC4 transfer function after the dual-gain merge.\n")
    TEXT("  0: Disabled -- output stays in linear (wide-range)\n")
    TEXT("  1: Enabled (default) -- output is LogC4-encoded"),
    ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarALEV4DGADiagLuminance(
    TEXT("r.ALEV4DGA.DiagLuminance"),
    0,
    TEXT("Diagnostic mode: ARRI-convention exposure false colour, banded on\n")
    TEXT("the LogC4 signal of TRUE scene luminance (pre-exposure removed).\n")
    TEXT("  0: Disabled (default)\n")
    TEXT("  1: Enabled\n")
    TEXT("     Red=clipped, Yellow=within 1 stop of clip (decrease exposure)\n")
    TEXT("     Pink=+1 stop skin tone, Green=18%% mid grey +/-1/4 stop (correct)\n")
    TEXT("     Grey=normal range, Blue=deep shadow, Purple=noise floor\n")
    TEXT("     (increase exposure)"),
    ECVF_RenderThreadSafe
);

// ----------------------------------------------------------------
// Shader parameter struct
// Must match parameters declared in DualGrainArchitecture.usf
// ----------------------------------------------------------------
BEGIN_SHADER_PARAMETER_STRUCT(FDualGrainArchitectureParameters, )
    SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
    SHADER_PARAMETER_SAMPLER(SamplerState, SceneColorSampler)
    SHADER_PARAMETER(float, HighGainAmount)
    SHADER_PARAMETER(float, LowGainAmount)
    SHADER_PARAMETER(float, ShadowThreshold)
    SHADER_PARAMETER(float, HighlightThreshold)
    SHADER_PARAMETER(float, ExposureOffset)
    SHADER_PARAMETER(float, FullWell)
    SHADER_PARAMETER(uint32, LogC4Enabled)
    SHADER_PARAMETER(FVector2f, ViewSize)
    SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputTexture)
    SHADER_PARAMETER(uint32, DiagLuminance)
END_SHADER_PARAMETER_STRUCT()

// ----------------------------------------------------------------
// C++ representation of our compute shader
// ----------------------------------------------------------------
class FDualGrainArchitectureCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FDualGrainArchitectureCS);
    SHADER_USE_PARAMETER_STRUCT(FDualGrainArchitectureCS, FGlobalShader);
    using FParameters = FDualGrainArchitectureParameters;

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return true;
    }
};

IMPLEMENT_GLOBAL_SHADER(
    FDualGrainArchitectureCS,
    "/Plugin/ALEV4DGA/DualGrainArchitecture.usf",
    "DualGrainArchitectureCS",
    SF_Compute
);

// ----------------------------------------------------------------
// Scene View Extension implementation
// ----------------------------------------------------------------
FALEV4DGASceneExtension::FALEV4DGASceneExtension(const FAutoRegister& AutoRegister)
    : FSceneViewExtensionBase(AutoRegister)
{
}
void FALEV4DGASceneExtension::SetupViewFamily(FSceneViewFamily& InViewFamily)
{
    // Runs on game thread once per frame
    bool bIsPiloting = false;

#if WITH_EDITOR
    if (GCurrentLevelEditingViewportClient)
    {
        const AActor* PilotedActor = GCurrentLevelEditingViewportClient->GetActorLock().GetLockedActor();
        bIsPiloting = PilotedActor && PilotedActor->IsA<ACineCameraActor>();
    }
#endif

    bEditorPilotingCineCam.store(bIsPiloting);
}


void FALEV4DGASceneExtension::PrePostProcessPass_RenderThread(
    FRDGBuilder& GraphBuilder,
    const FSceneView& View,
    const FPostProcessingInputs& Inputs)
{
    checkSlow(IsInRenderingThread());

    if (CVarALEV4DGAEnabled.GetValueOnRenderThread() == 0)
    {
        return;
    }

    // Check if this is a Cine Camera view by either path:
    // 1. Runtime ViewActor (works for PIE and MRQ)
    // 2. Editor viewport piloting state (works in editor)
    const AActor* ViewActor = View.ViewActor.Get();
    const bool bIsCineCameraView = (ViewActor && ViewActor->IsA<ACineCameraActor>())
        || bEditorPilotingCineCam.load();


    if (!bIsCineCameraView)
    {
        return;
    }

    // Get the scene colour texture from the post-process inputs
    FRDGTextureRef SceneColorTexture = (*Inputs.SceneTextures)->SceneColorTexture;
    if (!SceneColorTexture)
    {
        return;
    }

    FIntPoint OutputSize = SceneColorTexture->Desc.Extent;

    // Create an output texture matching scene colour
    FRDGTextureDesc OutputDesc = FRDGTextureDesc::Create2D(
        OutputSize,
        SceneColorTexture->Desc.Format,
        FClearValueBinding::Black,
        TexCreate_ShaderResource | TexCreate_UAV
    );

    FRDGTextureRef OutputTexture = GraphBuilder.CreateTexture(
        OutputDesc,
        TEXT("DGA_Output")
    );

    // Set up shader parameters
    FDualGrainArchitectureParameters* PassParameters =
        GraphBuilder.AllocParameters<FDualGrainArchitectureParameters>();

    // Bind the view uniform buffer so the shader can read View.PreExposure /
    // View.OneOverPreExposure. UE stores scene colour pre-exposed (scaled by
    // last frame's auto-exposure to fit FP16); the shader must remove that
    // scale before applying any physically-anchored constant (0.18 mid grey,
    // thresholds, LogC4 curve) and re-apply it on write.
    PassParameters->View = View.ViewUniformBuffer;

    PassParameters->SceneColorTexture = SceneColorTexture;
    PassParameters->SceneColorSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
    PassParameters->HighGainAmount = CVarALEV4DGAHighGain.GetValueOnRenderThread();
    PassParameters->LowGainAmount = CVarALEV4DGALowGain.GetValueOnRenderThread();
    PassParameters->ShadowThreshold = CVarALEV4DGAShadowThreshold.GetValueOnRenderThread();
    PassParameters->HighlightThreshold = CVarALEV4DGAHighlightThreshold.GetValueOnRenderThread();
    PassParameters->ExposureOffset = CVarALEV4DGAExposureOffset.GetValueOnRenderThread();
    PassParameters->FullWell = CVarALEV4DGAFullWell.GetValueOnRenderThread();
    PassParameters->ViewSize = FVector2f(OutputSize.X, OutputSize.Y);
    PassParameters->OutputTexture = GraphBuilder.CreateUAV(OutputTexture);
    PassParameters->LogC4Enabled = CVarALEV4DGALogC4Enabled.GetValueOnRenderThread() != 0 ? 1u : 0u;
    PassParameters->DiagLuminance = CVarALEV4DGADiagLuminance.GetValueOnRenderThread() != 0 ? 1u : 0u;

    TShaderMapRef<FDualGrainArchitectureCS> ComputeShader(
        GetGlobalShaderMap(GMaxRHIFeatureLevel)
    );

    FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(
        OutputSize,
        FIntPoint(8, 8)
    );

    // Dispatch the compute shader - reads SceneColor, writes to OutputTexture
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("DualGrainArchitecture"),
        ERDGPassFlags::Compute,
        ComputeShader,
        PassParameters,
        GroupCount
    );

    // Copy our result back over the scene colour buffer
    AddCopyTexturePass(GraphBuilder, OutputTexture, SceneColorTexture);
}
