#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"


class FALEV4DGASceneExtension : public FSceneViewExtensionBase
{
public:
    FALEV4DGASceneExtension(const FAutoRegister& AutoRegister);

    // Required overrides from FSceneViewExtensionBase
    virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
    virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
    virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}

    // Our actual hook into the render pipeline
    virtual void PrePostProcessPass_RenderThread(
        FRDGBuilder& GraphBuilder,
        const FSceneView& View,
        const FPostProcessingInputs& Inputs) override;
};