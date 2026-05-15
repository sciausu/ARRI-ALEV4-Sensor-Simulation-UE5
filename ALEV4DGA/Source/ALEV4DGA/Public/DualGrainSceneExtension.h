#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include <atomic>

class FALEV4DGASceneExtension : public FSceneViewExtensionBase
{
public:
    FALEV4DGASceneExtension(const FAutoRegister& AutoRegister);

    virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override;
    virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
    virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}

    virtual void PrePostProcessPass_RenderThread(
        FRDGBuilder& GraphBuilder,
        const FSceneView& View,
        const FPostProcessingInputs& Inputs) override;

private:
    // Game thread writes, render thread reads
    std::atomic<bool> bEditorPilotingCineCam{ false };
};