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
// ----------------------------------------------------------------
// Shader parameter struct
// Must match parameters declared in DualGrainArchitecture.usf
// ----------------------------------------------------------------
BEGIN_SHADER_PARAMETER_STRUCT(FDualGrainArchitectureParameters, )
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
    SHADER_PARAMETER_SAMPLER(SamplerState, SceneColorSampler)
    SHADER_PARAMETER(float, HighGainAmount)
    SHADER_PARAMETER(float, LowGainAmount)
    SHADER_PARAMETER(float, GrainIntensity)
    SHADER_PARAMETER(FVector2f, ViewSize)
    SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputTexture)
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
    PassParameters->HighGainAmount = 4.0f;
    PassParameters->LowGainAmount = 1.0f;
    PassParameters->GrainIntensity = 0.5f;
    PassParameters->ViewSize = FVector2f(OutputSize.X, OutputSize.Y);
    PassParameters->OutputTexture = GraphBuilder.CreateUAV(OutputTexture);

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