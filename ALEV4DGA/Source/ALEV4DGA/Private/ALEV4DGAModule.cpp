#include "ALEV4DGAModule.h"
#include "DualGrainSceneExtension.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"
#include "SceneViewExtension.h"

#define LOCTEXT_NAMESPACE "FALEV4DGAModule"

// Keeps our scene extension alive for the lifetime of the module
static TSharedPtr<FALEV4DGASceneExtension, ESPMode::ThreadSafe> DGASceneExtension;

void FALEV4DGAModule::StartupModule()
{
    // Map our plugin's Shaders folder to the virtual path /Plugin/ALEV4DGA
    FString ShaderDirectory = FPaths::Combine(
        IPluginManager::Get().FindPlugin(TEXT("ALEV4DGA"))->GetBaseDir(),
        TEXT("Shaders")
    );

    AddShaderSourceDirectoryMapping(
        TEXT("/Plugin/ALEV4DGA"),
        ShaderDirectory
    );

    // Defer scene extension creation until after the engine is fully initialised
    // Scene extensions need the renderer ready before they can register
    FCoreDelegates::OnPostEngineInit.AddLambda([]()
        {
            DGASceneExtension = FSceneViewExtensions::NewExtension<FALEV4DGASceneExtension>();
        });
}

void FALEV4DGAModule::ShutdownModule()
{
    DGASceneExtension.Reset();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FALEV4DGAModule, ALEV4DGA)