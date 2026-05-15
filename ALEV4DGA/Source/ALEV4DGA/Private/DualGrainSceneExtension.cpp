#include "DualGrainSceneExtension.h"
#include "SceneView.h"
#include "RenderGraph.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "GlobalShader.h"
#include "PostProcess/PostProcessInputs.h"


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

void FALEV4DGASceneExtension::PrePostProcessPass_RenderThread(
    FRDGBuilder& GraphBuilder,
    const FSceneView& View,
    const FPostProcessingInputs& Inputs)
{
    checkSlow(IsInRenderingThread());

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