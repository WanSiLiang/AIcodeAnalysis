// Copyright MonsterGuoGuo. All Rights Reserved.2023

#include "PanoramicPass.h"
#include "MoviePipelineOutputBase.h"
#include "Engine/TextureRenderTarget2D.h"
#include "SceneView.h"
#include "MovieRenderPipelineDataTypes.h"
#include "GameFramework/PlayerController.h"
#include "MoviePipelineRenderPass.h"
#include "EngineModule.h"
#include "Engine/World.h"
#include "Engine/TextureRenderTarget.h"
#include "MoviePipeline.h"
#include "MovieRenderOverlappedImage.h"
#include "MovieRenderPipelineCoreModule.h"
#include "ImagePixelData.h"
#include "MoviePipelineOutputBuilder.h"
#include "Containers/Array.h"
#include "FinalPostProcessSettings.h"
#include "MoviePipelineQueue.h"
#include "MoviePipelineAntiAliasingSetting.h"
#include "MoviePipelineOutputSetting.h"
#include "EngineUtils.h"
#include "ImageUtils.h"
#include "Math/Quat.h"
#include "PanoramicBlender.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PanoramicPass)

UPanoramicPass::UPanoramicPass() 
	: UMoviePipelineImagePassBase()
	, bAccumulatorIncludesAlpha(false)
	, bStereo(false)
	, NumHorizontalSteps(6)
	, NumVerticalSteps(3)
	, EyeSeparation(6.5f)
	, EyeConvergenceDistance(EyeSeparation * 30.f) //The focus distance between the eyes is 30 times the eye distance
	, bAllocateHistoryPerPane(true)
	, bHasWarnedSettings(false)
{
	// ID of the rendering pipeline
	PassIdentifier = FMoviePipelinePassIdentifier("Panoramic");
}

// The movie pipeline, here mainly provides three algorithms
namespace MoviePipeline
{
	namespace Panoramic
	{
		// Assign values within the interval (minimum, maximum, number of segments, whether the maximum is included)
		static TArray<float> HorizontalDistributeValuesInInterval(float InMin, float InMax, int32 InNumDivisions)
		{
			TArray<float> Results;
			float Delta = (InMax- InMin)/static_cast<float>(FMath::Max(InNumDivisions,1));
			float CurrentValue = InMin;
			// So here we're figuring out all the segment values
			for (int32 Index = 0; Index < InNumDivisions; Index++)
			{
				Results.Add(CurrentValue);

				CurrentValue += Delta;
			}
			return Results;
		};

		static TArray<float> VerticalDistributeValuesInInterval(float InMin,float InMax,int InNumDivisions)
		{
			TArray<float> Results;
			float Delta = (InMax- InMin)/static_cast<float>(FMath::Max(InNumDivisions,1));
			float CurrentValue =-InMin+Delta/2;
			for(int32 Index=0 ; Index < InNumDivisions ; Index++)
			{
				Results.Add(CurrentValue);

				CurrentValue += Delta;
			}
			return Results;
		}
		// Gets camera rotation for stereo rendering (position of output, rotation of output, panorama Pane, number of stereo, whether it is on previous position)
		void GetCameraOrientationForStereo(FVector& OutLocation, FRotator& OutRotation, const FPanoPane& InPane, const bool bInPrevPosition)
		{
			// Vertical segmentation
			const TArray<float> PitchValues = MoviePipeline::Panoramic::VerticalDistributeValuesInInterval(-90.0, 90, InPane.NumVerticalSteps);
			const TArray<float> YawValues = MoviePipeline::Panoramic::HorizontalDistributeValuesInInterval(0, 360, InPane.NumHorizontalSteps);

			/******************** The Angle in the panoramic ball ***********************/
			// Gets the vertical rotation Angle of the current Pane
			const float HorizontalRotationDeg = YawValues[InPane.HorizontalStepIndex];
			// Gets the vertical rotation Angle of the current Pane
			const float VerticalRotationDeg = PitchValues[InPane.VerticalStepIndex];

			/*************************** Calculates the absolute rotation of Pane with respect to world coordinates ********************/
			// Convert the horizontal rotation to Quat format, the Z-axis rotation value
			const FQuat HorizontalRotQuat = FQuat(FVector::UnitZ(), FMath::DegreesToRadians(HorizontalRotationDeg));
			// Convert the vertical rotation to Quat format, the rotation value of the Y-axis
			const FQuat VerticalRotQuat = FQuat(FVector::UnitY(), FMath::DegreesToRadians(VerticalRotationDeg));
			const FRotator SourceRot = bInPrevPosition ? InPane.PrevOriginalCameraRotation : InPane.OriginalCameraRotation;
			OutLocation = bInPrevPosition ? InPane.PrevOriginalCameraLocation : InPane.OriginalCameraLocation;
			FQuat RotationResult = FQuat(SourceRot) * HorizontalRotQuat * VerticalRotQuat;
			OutRotation = FRotator(RotationResult);
		}
	}
}

void UPanoramicPass::MoviePipelineRenderShowFlagOverride(FEngineShowFlags& OutShowFlag)
{
	// Panoramics can't support any of these.
	// Disable the black edge effect
	OutShowFlag.SetVignette(false);
	OutShowFlag.SetSceneColorFringe(false);
	// Disable physical material masks
	OutShowFlag.SetPhysicalMaterialMasks(false);

	/*if(bPathTracer)
	{
		OutShowFlag.SetPathTracing(true);
		OutViewModeIndex = EViewModeIndex::VMI_PathTracing;
	}*/
	// OutShowFlag.SetBloom(false); ToDo: Does bloom work?
}


void UPanoramicPass::SetupImpl(const MoviePipeline::FMoviePipelineRenderPassInitSettings& InPassInitSettings)
{
	Super::SetupImpl(InPassInitSettings);
	
	// This BackbufferResolution is the resolution of the whole picture

	const FIntPoint PaneResolution = GetPaneResolution(InPassInitSettings.BackbufferResolution);
	// Re-initialize the render target and surface queue
	GetOrCreateViewRenderTarget(PaneResolution);
	GetOrCreateSurfaceQueue(PaneResolution);
	
	int32 StereoMultiplier = bStereo ? 2 : 1;
	int32 NumPanes = NumHorizontalSteps * NumVerticalSteps;
	int32 NumPanoramicPanes = NumPanes * StereoMultiplier;
	if (bAllocateHistoryPerPane)
	{
		// Set total
		OptionalPaneViewStates.SetNum(NumPanoramicPanes);
		// Walk through the scene view state reference
		for (int32 Index = 0; Index < OptionalPaneViewStates.Num(); Index++)
		{
			OptionalPaneViewStates[Index].Allocate(InPassInitSettings.FeatureLevel);
		}
	}
	
	// We need one accumulator per pano tile if using accumulation.
	// Here is equivalent to use TAccumulatorPool created a FImageOverlappedAccumulator types of cumulative pool,
	// and the number for the () of the total view pane
	AccumulatorPool = MakeShared<TAccumulatorPool<FImageOverlappedAccumulator>, ESPMode::ThreadSafe>(NumPanoramicPanes);
	
	/**
	 * Create a class to blend the Panes of a panorama into a "columnar isometric" map.
	 * When all samples of a given panorama are fed into Blender and mixed,
	 * it will pass the data to the normal OutputBuilder.
	 * The latter does not know that we are sending it a complex hybrid image instead of a normal static image.
	 */
	PanoramicOutputBlender = MakeShared<FPanoramicBlender>(GetPipeline()->OutputBuilder, InPassInitSettings.BackbufferResolution);
	
	// Allocate an OCIO extension to do color grading if needed.
	OCIOSceneViewExtension = FSceneViewExtensions::NewExtension<FOpenColorIODisplayExtension>();
	// Whether you have a warning setting
	bHasWarnedSettings = false;
}


void UPanoramicPass::TeardownImpl()
{
	PanoramicOutputBlender.Reset();
	AccumulatorPool.Reset();
	for (int32 Index = 0; Index < OptionalPaneViewStates.Num(); Index++)
	{
		FSceneViewStateInterface* Ref = OptionalPaneViewStates[Index].GetReference();
		if (Ref)
		{
			Ref->ClearMIDPool();
		}
		OptionalPaneViewStates[Index].Destroy();
	}
	OptionalPaneViewStates.Reset();
	OptionalPaneViewStates.Empty();
	OCIOSceneViewExtension.Reset();
	OCIOSceneViewExtension = nullptr;
	Super::TeardownImpl();
}

// For object collection (memory collection) to GC
void UPanoramicPass::AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector)
{
	Super::AddReferencedObjects(InThis, Collector);
	UPanoramicPass& This = *CastChecked<UPanoramicPass>(InThis);
	
	for (int32 Index = 0; Index < This.OptionalPaneViewStates.Num(); Index++)
	{
		FSceneViewStateInterface* Ref = This.OptionalPaneViewStates[Index].GetReference();
		if (Ref)
		{
			
			Ref->AddReferencedObjects(Collector);
		}
	}
}

// This function is the interface that gets the view state of the scene according to the selected Pane.
// In fact, it returns (OptionalPaneViewStates), which gets the view state according to OptPayload
FSceneViewStateInterface* UPanoramicPass::GetSceneViewStateInterface(IViewCalcPayload* OptPayload)
{
	check(OptPayload);
	FPanoPane* PanoPane = (FPanoPane*)OptPayload;
	if (bAllocateHistoryPerPane)
	{
		return OptionalPaneViewStates[PanoPane->GetAbsoluteIndex()].GetReference();
	}
	return nullptr;
}


void UPanoramicPass::GatherOutputPassesImpl(TArray<FMoviePipelinePassIdentifier>& ExpectedRenderPasses)
{
	Super::GatherOutputPassesImpl(ExpectedRenderPasses);
}

void UPanoramicPass::AddViewExtensions(FSceneViewFamilyContext& InContext, FMoviePipelineRenderPassMetrics& InOutSampleState)
{
	
	if (InOutSampleState.OCIOConfiguration && InOutSampleState.OCIOConfiguration->bIsEnabled)
	{
		FOpenColorIODisplayConfiguration* OCIOConfigNew = const_cast<FMoviePipelineRenderPassMetrics&>(InOutSampleState).OCIOConfiguration;
		FOpenColorIODisplayConfiguration& OCIOConfigCurrent = OCIOSceneViewExtension->GetDisplayConfiguration();

		// We only need to set this once per render sequence.
		if (OCIOConfigNew->ColorConfiguration.ConfigurationSource && OCIOConfigNew->ColorConfiguration.ConfigurationSource != OCIOConfigCurrent.ColorConfiguration.ConfigurationSource)
		{
			OCIOSceneViewExtension->SetDisplayConfiguration(*OCIOConfigNew);
		}
		InContext.ViewExtensions.Add(OCIOSceneViewExtension.ToSharedRef());
	}
}

//The resolution of the Pane is obtained by the aspect ratio of the FOV,
//so that the world output is not related to the output height, keeping the original scale of the Pane screen
FIntPoint UPanoramicPass::GetPaneResolution(const FIntPoint& InSize) const
{
	float HorizontalFov;
	float VerticalFov;
	GetFieldOfView(HorizontalFov, VerticalFov);

	// Horizontal FoV is a proportion of the global horizontal resolution
	// ToDo: We might have to check which is higher, if numVerticalPanes > numHorizontalPanes this math might be backwards.
	float HorizontalRes = (HorizontalFov / 360.0f) * InSize.X;
	float Intermediate = FMath::Tan(FMath::DegreesToRadians(VerticalFov) * 0.5f) / FMath::Tan(FMath::DegreesToRadians(HorizontalFov) * 0.5f);
	float VerticalRes = HorizontalRes * Intermediate;
	return FIntPoint(FMath::CeilToInt(HorizontalRes), FMath::CeilToInt(VerticalRes));
}

void UPanoramicPass::GetFieldOfView(float& OutHorizontal, float& OutVertical) const
{
	// This is the most irrational and wasteful of resources.
	OutHorizontal = HorzFieldOfView > 0 ? HorzFieldOfView:FMath::Min(360.0/NumHorizontalSteps*(1+OverlapPercentage*0.01),179);
	OutVertical   = VertFieldOfView > 0 ? VertFieldOfView:FMath::Min(180/(NumVerticalSteps)*(1+OverlapPercentage*0.01),179);
}

FSceneView* UPanoramicPass::GetSceneViewForSampleState(FSceneViewFamily* ViewFamily, FMoviePipelineRenderPassMetrics& InOutSampleState, IViewCalcPayload* OptPayload)
{
	//Super::GetSceneViewForSampleState(ViewFamily,InOutSampleState,OptPayload);
	check(OptPayload);
	FPanoPane* PanoPane = (FPanoPane*)OptPayload;
	
	APlayerController* LocalPlayerController = GetPipeline()->GetWorld()->GetFirstPlayerController();
	// We ignored the resolution to do our own rendering work because that would be our output resolution.
	int32 PaneSizeX = PanoPane->Resolution.X;
	int32 PaneSizeY = PanoPane->Resolution.Y;


	/********************************* Scene view initialization Initialization of the object *************************************/
	FSceneViewInitOptions ViewInitOptions;
	ViewInitOptions.ViewFamily = ViewFamily;
	// Origin of view
	ViewInitOptions.ViewOrigin = PanoPane->CameraLocation;
	ViewInitOptions.SetViewRectangle(FIntRect(FIntPoint(0, 0), FIntPoint(PaneSizeX, PaneSizeY)));
	// The rotation matrix of the view
	ViewInitOptions.ViewRotationMatrix = FInverseRotationMatrix(FRotator(PanoPane->CameraRotation));
	ViewInitOptions.ViewRotationMatrix = ViewInitOptions.ViewRotationMatrix * FMatrix(
		FPlane(0, 0, 1, 0),
		FPlane(1, 0, 0, 0),
		FPlane(0, 1, 0, 0),
		FPlane(0, 0, 0, 1));
	ViewInitOptions.ViewActor = LocalPlayerController ? LocalPlayerController->GetViewTarget() : nullptr;
	
	float ViewFOV = PanoPane->HorizontalFieldOfView;
	
	// ViewFOV = 2.0f * FMath::RadiansToDegrees(FMath::Atan((1.0f + InOutSampleState.OverscanPercentage) * FMath::Tan(FMath::DegreesToRadians(ViewFOV * 0.5f))));
	
	
	float DofSensorScale = 1.0f;
	
	/*************************************** Computed projection matrix ************************************************/
	{
		
		float MinZ = GNearClippingPlane;
		if (LocalPlayerController && LocalPlayerController->PlayerCameraManager)
		{
			float NearClipPlane = LocalPlayerController->PlayerCameraManager->GetCameraCacheView().PerspectiveNearClipPlane;
			MinZ = NearClipPlane > 0 ? NearClipPlane : MinZ;
		}
		// Near-cut plane of panoramic Pane = MinZ
		PanoPane->NearClippingPlane = MinZ;
		
		
		// Avoid zero ViewFOV's which cause divide by zero's in projection matrix
		const float MatrixFOV = FMath::Max(0.001f, ViewFOV) * (float)PI / 360.0f;
		static_assert((int32)ERHIZBuffer::IsInverted != 0, "ZBuffer should be inverted");

		float XAxisMultiplier = 1.f;
		float YAxisMultiplier = 1.f;
		if (PaneSizeX > PaneSizeY)		
		{
			// Viewport is wider than it is tall
			XAxisMultiplier = PaneSizeX / (float)PaneSizeY;
		}
		else
		{
			// Viewport is taller than wide
			YAxisMultiplier = PaneSizeY / (float)PaneSizeX;
		}
		
		FMatrix BaseProjMatrix = FReversedZPerspectiveMatrix(
				MatrixFOV,
				XAxisMultiplier,
				YAxisMultiplier,
				MinZ
			);
		ViewInitOptions.ProjectionMatrix = BaseProjMatrix;
	}

	ViewInitOptions.SceneViewStateInterface = GetSceneViewStateInterface(OptPayload);
	ViewInitOptions.FOV = ViewFOV;
	
	FSceneView* View = new FSceneView(ViewInitOptions);
	ViewFamily->Views.Add(View);
	View->ViewLocation = PanoPane->CameraLocation;
	View->ViewRotation = PanoPane->CameraRotation;
	// Overrides previous/current view transitions so that tile rendering does not use incorrect occlusion/motion blur information.
	View->PreviousViewTransform = FTransform(PanoPane->PrevCameraRotation, PanoPane->PrevCameraLocation);
	View->StartFinalPostprocessSettings(View->ViewLocation);
	BlendPostProcessSettings(View, InOutSampleState, OptPayload);
	
	View->FinalPostProcessSettings.DepthOfFieldSensorWidth *= DofSensorScale;

	// Modify the 'center' of the lens to be offset for high-res tiling, helps some effects (vignette) etc. still work.
	// ToDo: Highres Tiling support
	// View->LensPrincipalPointOffsetScale = CalculatePrinciplePointOffsetForTiling(InOutSampleState);
	
	View->EndFinalPostprocessSettings(ViewInitOptions);

	// ToDo: Re-inject metadata
	return View;
}


FIntPoint UPanoramicPass::GetPayloadPaneResolution(const FIntPoint& InSize, IViewCalcPayload* OptPayload) const
{
	if (OptPayload)
	{
		FPanoPane* PanoPane = (FPanoPane*)OptPayload;
		return PanoPane->Resolution;
	}
	return InSize;
} 


TWeakObjectPtr<UTextureRenderTarget2D> UPanoramicPass::GetOrCreateViewRenderTarget(const FIntPoint& InSize, IViewCalcPayload* OptPayload)
{
	return Super::GetOrCreateViewRenderTarget(GetPayloadPaneResolution(InSize, OptPayload), OptPayload);
}


TSharedPtr<FMoviePipelineSurfaceQueue, ESPMode::ThreadSafe> UPanoramicPass::GetOrCreateSurfaceQueue(const FIntPoint& InSize, IViewCalcPayload* OptPayload)
{
	return Super::GetOrCreateSurfaceQueue(GetPayloadPaneResolution(InSize, OptPayload), OptPayload);
}

void UPanoramicPass::RenderSample_GameThreadImpl(const FMoviePipelineRenderPassMetrics& InSampleState)
{
	
	// Wait for a surface to be available to write to. This will stall the game thread while the RHI/Render Thread catch up.
	Super::RenderSample_GameThreadImpl(InSampleState);
	
	const FIntPoint PaneResolution = GetPaneResolution(InSampleState.BackbufferSize);
	
	/***************************************·* Pane information entry *****************************************/
	int32 NumEyeRenders = bStereo ? 2 : 1;
	// Number the eyes, so after adjusting it, you render the left eye and then the right eye
	for (int32 EyeLoopIndex = 0; EyeLoopIndex < NumEyeRenders; EyeLoopIndex++)
	{
		for(int32 VerticalStepIndex = 0; VerticalStepIndex < NumVerticalSteps; VerticalStepIndex++)
		{
			for(int32 HorizontalStepIndex = 0; HorizontalStepIndex < NumHorizontalSteps; HorizontalStepIndex++)
			{
				FMoviePipelineRenderPassMetrics InOutSampleState = InSampleState;
				FPanoPane Pane;
				{
				    // What I'm doing here is getting some information about the camera from sequnce (the position of the last frame, and the position of this frame)
					FVector OriginalSequenceLocation = InSampleState.FrameInfo.CurrViewLocation;
					FVector PrevOriginalSequenceLocation = InSampleState.FrameInfo.PrevViewLocation;
					FRotator OriginalSequenceRotation = InSampleState.FrameInfo.CurrViewRotation;
					FRotator PrevOriginalSequenceRotation = InSampleState.FrameInfo.PrevViewRotation;
					FTransform OriginalSequenceTransform = FTransform(OriginalSequenceRotation,OriginalSequenceLocation,FVector(1.f, 1.f, 1.f));
					FTransform  PrevOriginalSequenceTransform = FTransform(PrevOriginalSequenceRotation,PrevOriginalSequenceLocation,FVector(1.f, 1.f, 1.f));
					// Number of stereoscopic eyes (-1, 0, 1)
					int32 StereoIndex = bStereo ? EyeLoopIndex : -1;
					Pane.EyeIndex = StereoIndex;
					if(StereoIndex == -1)
					{
						Pane.OriginalCameraLocation =OriginalSequenceLocation;
						Pane.PrevOriginalCameraLocation = PrevOriginalSequenceLocation;
						Pane.OriginalCameraRotation = OriginalSequenceRotation;
						Pane.PrevOriginalCameraRotation = PrevOriginalSequenceRotation;
					}
					else
					{
						check(StereoIndex==0||StereoIndex==1);
						float EyeOffset = StereoIndex == 0 ? (EyeSeparation / 2.f) : (-EyeSeparation / 2.f);
						
						Pane.OriginalCameraLocation = OriginalSequenceTransform.TransformPosition(FVector(0.0f,EyeOffset,0.0f));
						Pane.PrevOriginalCameraLocation = PrevOriginalSequenceTransform.TransformPosition(FVector(0.0f,EyeOffset,0.0f));
						if(bEyeConvergenceDistance)
						{
							
							float EyeAngle = FMath::RadiansToDegrees(FMath::Atan(EyeOffset/EyeConvergenceDistance));
							//UE_LOG(LogMovieRenderPipeline,Warning,TEXT("angel:%f"),EyeAngle);
							Pane.OriginalCameraRotation = OriginalSequenceTransform.TransformRotation(FRotator(0.0f,EyeAngle,0.0f).Quaternion()).Rotator();
							Pane.PrevOriginalCameraRotation = PrevOriginalSequenceTransform.TransformRotation(FRotator(0.0f,EyeAngle,0.0f).Quaternion()).Rotator();
						}else
						{
							Pane.OriginalCameraRotation = OriginalSequenceRotation;
							Pane.PrevOriginalCameraRotation = PrevOriginalSequenceRotation;
						}
						
					}
					Pane.VerticalStepIndex = VerticalStepIndex;
					Pane.HorizontalStepIndex = HorizontalStepIndex;
					
					Pane.NumHorizontalSteps = NumHorizontalSteps;
					Pane.NumVerticalSteps = NumVerticalSteps;
					Pane.EyeSeparation = EyeSeparation;
					Pane.EyeConvergenceDistance = EyeConvergenceDistance;
					Pane.bIncludeAlpha = bAccumulatorIncludesAlpha;
					// Get the actual camera position and rotation for a specific Pane, this data from the global camera
					MoviePipeline::Panoramic::GetCameraOrientationForStereo(/*Out*/ Pane.PrevCameraLocation, /*Out*/ Pane.PrevCameraRotation, Pane,  /*bInPrevPos*/ true);
					MoviePipeline::Panoramic::GetCameraOrientationForStereo(/*Out*/ Pane.CameraLocation, /*Out*/ Pane.CameraRotation, Pane, /*bInPrevPos*/ false);
					GetFieldOfView(Pane.HorizontalFieldOfView, Pane.VerticalFieldOfView);
					
					// Copy the backbuffer size of our actual allocated texture into the Pane instead of using the global output resolution, which is the final image size.
					Pane.Resolution = PaneResolution;
				}
				// Create a family of views for this rendering. This will contain only one view to better fit our existing MRQ architecture.
				// Computing the view family requires computing the FSceneView itself, which is highly customized for panos. So we provide FPanoPlane to be passed as' raw 'data so we can use it when calculating personal views.
				TSharedPtr<FSceneViewFamilyContext> ViewFamily = CalculateViewFamily(InOutSampleState, &Pane);
				EAntiAliasingMethod AAMethod = ViewFamily->Views[0]->AntiAliasingMethod;
				const bool bRequiresHistory = (AAMethod == EAntiAliasingMethod::AAM_TemporalAA) || (AAMethod == EAntiAliasingMethod::AAM_TSR);
				if (!bAllocateHistoryPerPane && bRequiresHistory)
				{
					if (!bHasWarnedSettings)
					{
						bHasWarnedSettings = true;
						UE_LOG(LogMovieRenderPipeline, Warning, TEXT("Panoramic Renders do not support TAA without enabling bAllocateHistoryPerPane! Forcing AntiAliasing off."));
					}
					FSceneView* NonConstView = const_cast<FSceneView*>(ViewFamily->Views[0]);
					// Change the resist tooth mode to no anti-aliasing in the extraordinary view
					NonConstView->AntiAliasingMethod = EAntiAliasingMethod::AAM_None;
				}
				
				// Submit the view for rendering
				TWeakObjectPtr<UTextureRenderTarget2D> ViewRenderTarget = GetOrCreateViewRenderTarget(PaneResolution);
				check(ViewRenderTarget.IsValid());
				
				FRenderTarget* RenderTarget = ViewRenderTarget->GameThread_GetRenderTargetResource();
				check(RenderTarget);
				FCanvas Canvas = FCanvas(RenderTarget, nullptr, GetPipeline()->GetWorld(), ViewFamily->GetFeatureLevel(), FCanvas::CDM_DeferDrawing, 1.0f);
				//A message is sent from the game thread call to the rendering thread to render the family of views.
				GetRendererModule().BeginRenderingViewFamily(&Canvas, ViewFamily.Get());
				ScheduleReadbackAndAccumulation(InOutSampleState, Pane, Canvas);
			}
		}
	}
	
}

void UPanoramicPass::ScheduleReadbackAndAccumulation(const FMoviePipelineRenderPassMetrics& InSampleState, const FPanoPane& InPane, FCanvas& InCanvas)
{
	// First check the sample status to see if the result needs to be discarded
	if (InSampleState.bDiscardResult)
	{
		return;
	}
	
	// We have a pool of accumulators - we do multithreaded accumulations on the task graph, and for each frame,
	// the task has previous samples as pre-requirements to maintain the order of the accumulations.
	// However, each accumulator can only process one frame at a time, so we created a pool of accumulators to work concurrently.
	// This requires a limit, as large accumulations (16k) can take up a lot of system RAM.
	TSharedPtr<FAccumulatorPool::FAccumulatorInstance, ESPMode::ThreadSafe> SampleAccumulator;
	{
		SCOPE_CYCLE_COUNTER(STAT_MoviePipeline_WaitForAvailableAccumulator);
		// Generate a unique PassIdentifier for the Panorama pane.
		FMoviePipelinePassIdentifier PanePassIdentifier = FMoviePipelinePassIdentifier(FString::Printf(TEXT("%s_%d_x%d_y%d"), *PassIdentifier.Name,InPane.EyeIndex, InPane.HorizontalStepIndex, InPane.VerticalStepIndex));
		SampleAccumulator = AccumulatorPool->BlockAndGetAccumulator_GameThread(InSampleState.OutputState.OutputFrameNumber, PanePassIdentifier);
	}
	
	TSharedRef<FPanoramicImagePixelDataPayload, ESPMode::ThreadSafe> FramePayload = MakeShared<FPanoramicImagePixelDataPayload, ESPMode::ThreadSafe>();

	
	FramePayload->PassIdentifier = PassIdentifier;
	FramePayload->SampleState = InSampleState;
	FramePayload->SortingOrder = GetOutputFileSortingOrder();
	FramePayload->Pane = InPane;
	
	if (FramePayload->Pane.EyeIndex >= 0)
	{
		FramePayload->Debug_OverrideFilename = FString::Printf(TEXT("/%s_SS_%d_TS_%d_TileX_%d_TileY_%d_PaneX_%d_PaneY_%d_Eye_%d.%d.exr"),
			*FramePayload->PassIdentifier.Name, FramePayload->SampleState.SpatialSampleIndex, FramePayload->SampleState.TemporalSampleIndex,
			FramePayload->SampleState.TileIndexes.X, FramePayload->SampleState.TileIndexes.Y, FramePayload->Pane.HorizontalStepIndex,
			FramePayload->Pane.VerticalStepIndex, FramePayload->Pane.EyeIndex, FramePayload->SampleState.OutputState.OutputFrameNumber);
	}
	else
	{
		FramePayload->Debug_OverrideFilename = FString::Printf(TEXT("/%s_SS_%d_TS_%d_TileX_%d_TileY_%d_PaneX_%d_PaneY_%d.%d.exr"),
			*FramePayload->PassIdentifier.Name, FramePayload->SampleState.SpatialSampleIndex, FramePayload->SampleState.TemporalSampleIndex,
			FramePayload->SampleState.TileIndexes.X, FramePayload->SampleState.TileIndexes.Y, FramePayload->Pane.HorizontalStepIndex,
			FramePayload->Pane.VerticalStepIndex, FramePayload->SampleState.OutputState.OutputFrameNumber);
	}
	
	TSharedPtr<FMoviePipelineSurfaceQueue, ESPMode::ThreadSafe> LocalSurfaceQueue = GetOrCreateSurfaceQueue(InSampleState.BackbufferSize, (IViewCalcPayload*)(&FramePayload->Pane));

	// Image sample cumulative parameters for the rendering pipeline
	MoviePipeline::FImageSampleAccumulationArgs AccumulationArgs;
	{
		AccumulationArgs.OutputMerger = PanoramicOutputBlender;
		AccumulationArgs.ImageAccumulator = StaticCastSharedPtr<FImageOverlappedAccumulator>(SampleAccumulator->Accumulator);
		AccumulationArgs.bAccumulateAlpha = bAccumulatorIncludesAlpha;
	}
	
	auto Callback = [this, FramePayload, AccumulationArgs, SampleAccumulator](TUniquePtr<FImagePixelData>&& InPixelData)
	{
		bool bFinalSample = FramePayload->IsLastTile() && FramePayload->IsLastTemporalSample();
		FMoviePipelineBackgroundAccumulateTask Task;
		Task.LastCompletionEvent = SampleAccumulator->TaskPrereq;
		FGraphEventRef Event = Task.Execute([PixelData = MoveTemp(InPixelData), AccumulationArgs, bFinalSample, SampleAccumulator]() mutable
		{
			// Enqueue a encode for this frame onto our worker thread.
			MoviePipeline::AccumulateSample_TaskThread(MoveTemp(PixelData), AccumulationArgs);
			if (bFinalSample)
			{
				// Final sample has now been executed, break the pre-req chain and free the accumulator for reuse.
				SampleAccumulator->bIsActive = false;
				SampleAccumulator->TaskPrereq = nullptr;
			}
		});
		SampleAccumulator->TaskPrereq = Event;
		this->OutstandingTasks.Add(Event);
	};
	
	FRenderTarget* RenderTarget = InCanvas.GetRenderTarget();
	
	ENQUEUE_RENDER_COMMAND(CanvasRenderTargetResolveCommand)(
		[LocalSurfaceQueue, FramePayload, Callback, RenderTarget](FRHICommandListImmediate& RHICmdList) mutable
		{
			// Enqueue a encode for this frame onto our worker thread.
			LocalSurfaceQueue->OnRenderTargetReady_RenderThread(RenderTarget->GetRenderTargetTexture(), FramePayload, MoveTemp(Callback));
		});
	//InCanvas.Flush_GameThread();
}
