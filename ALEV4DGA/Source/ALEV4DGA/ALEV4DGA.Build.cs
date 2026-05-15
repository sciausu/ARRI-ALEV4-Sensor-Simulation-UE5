using UnrealBuildTool;

public class ALEV4DGA : ModuleRules
{
    public ALEV4DGA(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "CoreUObject",
            "Engine",
            "Renderer",
            "RenderCore",
            "RHI",
            "Projects",
            "CinematicCamera",
        });

        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.Add("UnrealEd");
            PrivateDependencyModuleNames.Add("LevelEditor");
        }

        PrivateIncludePaths.AddRange(new string[]
        {
            System.IO.Path.Combine(GetModuleDirectory("Renderer"), "Internal"),
        });
    }
}