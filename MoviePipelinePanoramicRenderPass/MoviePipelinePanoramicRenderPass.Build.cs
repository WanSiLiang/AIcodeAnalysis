//  Copyright MonsterGuoGuo. All Rights Reserved.2023

using UnrealBuildTool;

public class MoviePipelinePanoramicRenderPass : ModuleRules
{
	public MoviePipelinePanoramicRenderPass(ReadOnlyTargetRules Target) : base(Target)
	{
		PrivateDependencyModuleNames.AddRange(
			new string[] {
				"Core",
				"CoreUObject",
				"Engine",
				"Json"
			}
		);

		PublicDependencyModuleNames.AddRange(
			new string[] {
				"MovieRenderPipelineCore",
				"MovieRenderPipelineRenderPasses",
				"RenderCore",
                "RHI",
                "OpenColorIO",
			}
		);
	}
}
