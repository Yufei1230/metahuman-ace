using UnrealBuildTool;

public class ACEConversation : ModuleRules
{
    public ACEConversation(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.Add("UnrealEd"); // Actual PIE lifecycle regression test only.
        }

        PublicDependencyModuleNames.AddRange(
            new[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "AudioCaptureCore"
            }
        );

        PrivateDependencyModuleNames.AddRange(
            new[]
            {
                "Json",
                "Projects",
                "WebSockets",
                "AudioCapture", "AudioPlatformConfiguration", "Slate", "SlateCore", "InputCore"
            }
        );
    }
}
