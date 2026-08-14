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
    TEXT("High-gain path multiplier (shadow path).\n")
    TEXT("Both paths are normalised to a common scale, so this sets where the\n")
    TEXT("high-gain read clips (FullWell / HighGain), not overall brightness. Default: 4.0"),
    ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarALEV4DGALowGain(
    TEXT("r.ALEV4DGA.LowGain"),
    1.0f,
    TEXT("Low-gain path multiplier (highlight path).\n")
    TEXT("Sets the highlight clip point of the merged image: FullWell / LowGain. Default: 1.0"),
    ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarALEV4DGAShadowThreshold(
    TEXT("r.ALEV4DGA.ShadowThreshold"),
    0.18f,
    TEXT("Luminance below which the mask is fully on the high-gain path.\n")
    TEXT("0.18 corresponds to 18%% middle grey reflectance. Default: 0.18"),
    ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarALEV4DGAHighlightThreshold(
    TEXT("r.ALEV4DGA.HighlightThreshold"),
    0.5f,
    TEXT("Luminance above which the mask is fully on the low-gain path.\n")
    TEXT("Must be greater than ShadowThreshold. Default: 0.5"),
    ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarALEV4DGAFullWell(
    TEXT("r.ALEV4DGA.FullWell"),
    460.0f,
    TEXT("Simulated ADC full-scale in amplified units. Each gain path clips at\n")
    TEXT("FullWell / gain in scene-linear units. 460 puts the low-gain clip\n")
    TEXT("~11.3 stops above mid grey, just inside LogC4's encodable max. Default: 460.0"),
    ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarALEV4DGALogC4Enabled(
    TEXT("r.ALEV4DGA.LogC4"),
    1,
    TEXT("Apply ARRI LogC4 transfer function after the dual-gain merge.\n")
    TEXT("  0: Disabled - output stays in linear (wide-range)\n")
    TEXT("  1: Enabled (default) - output is LogC4-encoded"),
    ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarALEV4DGADiagLuminance(
    TEXT("r.ALEV4DGA.DiagLuminance"),
    0,
    TEXT("Diagnostic mode: replaces DGA output with a false-colour luminance heatmap.\n")
    TEXT("  0: Disabled (default)\n")
    TEXT("  1: Show scene luminance as false colour\n")
    TEXT("     Blue=dark, Green=0.18 middle grey, Yellow=highlights, Red=clipped"),
    ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarALEV4DGANoiseScale(
    TEXT("r.ALEV4DGA.NoiseScale"), 1.0f,
    TEXT("Master scale for simulated sensor noise (0 = off, 1 = measured ALEXA 35 level)."),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarALEV4DGANoiseReadSigma(
    TEXT("r.ALEV4DGA.NoiseReadSigma"), 0.0026f,
    TEXT("Read-noise floor, scene-referred linear. Measured from ALEXA 35 LogC4 footage."),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarALEV4DGANoiseShotK(
    TEXT("r.ALEV4DGA.NoiseShotK"), 0.00103f,
    TEXT("Shot-noise coefficient k in sigma=sqrt(read^2 + k*signal). Measured."),
    ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarALEV4DGASpotMeter(
    TEXT("r.ALEV4DGA.SpotMeter"), 0,
    TEXT("Draw a centre spot-meter reticle (green swatch = 18%% grey = correct exposure).\n")
    TEXT("  0: Disabled (default)\n")
    TEXT("  1: Show centre spot meter"),
    ECVF_RenderThreadSafe);

// ----------------------------------------------------------------
// Shader parameter struct
// Must match parameters declared in DualGrainArchitecture.usf
// ----------------------------------------------------------------
BEGIN_SHADER_PARAMETER_STRUCT(FDualGrainArchitectureParameters, )
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
    SHADER_PARAMETER_SAMPLER(SamplerState, SceneColorSampler)
    SHADER_PARAMETER(float, HighGainAmount)
    SHADER_PARAMETER(float, LowGainAmount)
    SHADER_PARAMETER(float, ShadowThreshold)
    SHADER_PARAMETER(float, HighlightThreshold)
    SHADER_PARAMETER(float, FullWell)
    SHADER_PARAMETER(uint32, LogC4Enabled)
    SHADER_PARAMETER(FVector2f, ViewSize)
    SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputTexture)
    SHADER_PARAMETER(uint32, DiagLuminance)
    SHADER_PARAMETER(float, NoiseScale)
    SHADER_PARAMETER(float, NoiseReadSigma)
    SHADER_PARAMETER(float, NoiseShotK)
    SHADER_PARAMETER(uint32, NoiseFrameIndex)
    SHADER_PARAMETER(uint32, SpotMeter)
    SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
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

    PassParameters->SceneColorTexture = SceneColorTexture;
    PassParameters->SceneColorSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
    PassParameters->HighGainAmount = CVarALEV4DGAHighGain.GetValueOnRenderThread();
    PassParameters->LowGainAmount = CVarALEV4DGALowGain.GetValueOnRenderThread();
    PassParameters->ShadowThreshold = CVarALEV4DGAShadowThreshold.GetValueOnRenderThread();
    PassParameters->HighlightThreshold = CVarALEV4DGAHighlightThreshold.GetValueOnRenderThread();
    PassParameters->FullWell = CVarALEV4DGAFullWell.GetValueOnRenderThread();
    PassParameters->ViewSize = FVector2f(OutputSize.X, OutputSize.Y);
    PassParameters->OutputTexture = GraphBuilder.CreateUAV(OutputTexture);
    PassParameters->LogC4Enabled = CVarALEV4DGALogC4Enabled.GetValueOnRenderThread() != 0 ? 1u : 0u;
    PassParameters->DiagLuminance = CVarALEV4DGADiagLuminance.GetValueOnRenderThread() != 0 ? 1u : 0u;
    PassParameters->View = View.ViewUniformBuffer;
    PassParameters->NoiseScale = CVarALEV4DGANoiseScale.GetValueOnRenderThread();
    PassParameters->NoiseReadSigma = CVarALEV4DGANoiseReadSigma.GetValueOnRenderThread();
    PassParameters->NoiseShotK = CVarALEV4DGANoiseShotK.GetValueOnRenderThread();
    PassParameters->NoiseFrameIndex = (uint32)GFrameNumberRenderThread;
    PassParameters->SpotMeter = CVarALEV4DGASpotMeter.GetValueOnRenderThread() != 0 ? 1u : 0u;

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
