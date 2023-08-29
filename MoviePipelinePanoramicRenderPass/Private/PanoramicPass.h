//Copyright MonsterGuoGuo. All Rights Reserved.2023
#pragma once

#include "MoviePipelineImagePassBase.h"
#include "OpenColorIODisplayExtension.h"
#include "PanoramicPass.generated.h"

class UTextureRenderTarget2D;
struct FImageOverlappedAccumulator;
class FSceneViewFamily;
class FSceneView;
struct FAccumulatorPool;

struct FPanoPane : public UMoviePipelineImagePassBase::IViewCalcPayload
{
	// The camera location as defined by the actual sequence, consistent for all panes.
	FVector OriginalCameraLocation;
	// The camera location last frame, used to ensure camera motion vectors are right.
	FVector PrevOriginalCameraLocation;
	// The camera rotation as defined by the actual sequence
	FRotator OriginalCameraRotation;
	// The camera rotation last frame, used to ensure camera motion vectors are right.
	FRotator PrevOriginalCameraRotation;
	// The near clip plane distance from the camera.
	float NearClippingPlane;

	// How far apart are the eyes (total) for stereo?
	float EyeSeparation;			
	float EyeConvergenceDistance;	

	// The horizontal field of view this pane was rendered with
	float HorizontalFieldOfView;	
	float VerticalFieldOfView;		
	// resolution
	FIntPoint Resolution;

	// The actual rendering location for this pane, offset by the stereo eye if needed.
	FVector CameraLocation;				
	FVector PrevCameraLocation;			
	FRotator CameraRotation;			
	FRotator PrevCameraRotation;		

	// How many horizontal segments are there total.
	int32 NumHorizontalSteps;
	int32 NumVerticalSteps;
	
	// Which horizontal segment are we?
	int32 HorizontalStepIndex;
	// Which vertical segment are we?
	int32 VerticalStepIndex;

	// When indexing into arrays of Panes, which index is this?
	int32 GetAbsoluteIndex() const
	{
		const int32 EyeOffset = EyeIndex == -1 ? 0 : EyeIndex;
		if(EyeOffset == 0)
		{
			return (VerticalStepIndex*NumHorizontalSteps)+HorizontalStepIndex;
		}
		return (NumVerticalSteps+VerticalStepIndex)*NumHorizontalSteps+HorizontalStepIndex;
	}

	// -1 if no stereo, 0 left eye, 1 right eye.
	int32 EyeIndex;
	
	bool bIncludeAlpha;
};

// Panoramic image data load
struct FPanoramicImagePixelDataPayload : public FImagePixelDataPayload
{
	virtual TSharedRef<FImagePixelDataPayload> Copy() const override
	{
		return MakeShared<FPanoramicImagePixelDataPayload>(*this);
	}

	virtual FIntPoint GetAccumulatorSize() const override
	{
		return Pane.Resolution;
	}

	virtual FIntPoint GetOverlapPaddedSize() const override
	{
		//FIntPoint Result = FIntPoint(Pane.Resolution*Pane.OverlapPercentage);
		const FIntPoint Result = Pane.Resolution;
		return Result;
	}
	
	virtual bool GetOverlapPaddedSizeIsValid(const FIntPoint InRawSize) const override
	{
		// Panoramic images don't support any additional padding/overlap.
		//FIntPoint Result = FIntPoint(Pane.Resolution*Pane.OverlapPercentage);
		const FIntPoint Result = Pane.Resolution;
		return InRawSize == Result;
	}

	virtual void GetWeightFunctionParams(MoviePipeline::FTileWeight1D& WeightFunctionX, MoviePipeline::FTileWeight1D& WeightFunctionY) const override
	{
		WeightFunctionX.InitHelper(0, Pane.Resolution.X, 0);
		WeightFunctionY.InitHelper(0, Pane.Resolution.Y, 0);
	}

	FPanoPane Pane;
};

// Generate a panorama (which may be stereoscopic, stored up and down on the final page) in a cylindrical isometric projection space. 
// Each rendering is a traditional 2D rendering, and they are mixed behind them. 
// For each horizontal step, we render a lot of vertical steps. 
// Each of these renderings is called "Pane" to avoid confusion with high-resolution "Tiles", which is used only in 2D.

UCLASS(BlueprintType)
class UPanoramicPass : public UMoviePipelineImagePassBase
{
	GENERATED_BODY()

public:
	UPanoramicPass();
	
protected:
	// UMoviePipelineRenderPass API
	virtual void SetupImpl(const MoviePipeline::FMoviePipelineRenderPassInitSettings& InPassInitSettings) override;
	virtual void RenderSample_GameThreadImpl(const FMoviePipelineRenderPassMetrics& InSampleState) override;
	virtual void TeardownImpl() override;
#if WITH_EDITOR
	virtual FText GetDisplayText() const override { return NSLOCTEXT("MovieRenderPipeline", "PanoramicRenderPassSetting_DisplayName", "Panoramic MRQ"); }
#endif
	virtual void MoviePipelineRenderShowFlagOverride(FEngineShowFlags& OutShowFlag) override;
	virtual void GatherOutputPassesImpl(TArray<FMoviePipelinePassIdentifier>& ExpectedRenderPasses) override;
	virtual bool IsAntiAliasingSupported() const { return true; }
	virtual int32 GetOutputFileSortingOrder() const override { return 1; }
	virtual bool IsAlphaInTonemapperRequiredImpl() const override { return false; }
	virtual FSceneViewStateInterface* GetSceneViewStateInterface(IViewCalcPayload* OptPayload) override;
	virtual void AddViewExtensions(FSceneViewFamilyContext& InContext, FMoviePipelineRenderPassMetrics& InOutSampleState) override;
	virtual bool IsAutoExposureAllowed(const FMoviePipelineRenderPassMetrics& InSampleState) const override { return false; }
	virtual FSceneView* GetSceneViewForSampleState(FSceneViewFamily* ViewFamily, FMoviePipelineRenderPassMetrics& InOutSampleState, IViewCalcPayload* OptPayload = nullptr) override;
	virtual TWeakObjectPtr<UTextureRenderTarget2D> GetOrCreateViewRenderTarget(const FIntPoint& InSize, IViewCalcPayload* OptPayload = nullptr) override;
	virtual TSharedPtr<FMoviePipelineSurfaceQueue, ESPMode::ThreadSafe> GetOrCreateSurfaceQueue(const FIntPoint& InSize, IViewCalcPayload* OptPayload = nullptr) override;
	
	static void AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector);
	
	void ScheduleReadbackAndAccumulation(const FMoviePipelineRenderPassMetrics& InSampleState, const FPanoPane& InPane, FCanvas& InCanvas);
	void GetFieldOfView(float& OutHorizontal, float& OutVertical) const;
	FIntPoint GetPaneResolution(const FIntPoint& InSize) const;
	FIntPoint GetPayloadPaneResolution(const FIntPoint& InSize, IViewCalcPayload* OptPayload) const;
public:

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings")
	bool bAccumulatorIncludesAlpha;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings")
	bool bStereo;
	
	/** More horizontal steps will have better horizontal smoothness, but too many horizontal partitions will consume more performance */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panoramic Settings", meta = (UIMin = "4", ClampMin = "4",ClampMax="30"))
	int32 NumHorizontalSteps;
	/** More horizontal steps will have better Vertical smoothness, but too many Vertical partitions will consume more performance */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panoramic Settings", meta = (UIMin = "2", ClampMin = "2",ClampMax="12"))
	int32 NumVerticalSteps;

	/** A higher percentage of overlap will have a smoother effect, when more invalid pixels will be produced.*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panoramic Settings",meta = (UIMin = "10", ClampMin = "10",ClampMax="100"))
	int32 OverlapPercentage=50;
	
	

	/** Advance used only. Allows you to override the Horizontal Field of View (if not zero). Can cause crashes or incomplete panoramas.*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Panoramic Settings")
	float HorzFieldOfView = 0.f;

	/** Advance used only. Allows you to override the Vertical Field of View (if not zero). Can cause crashes or incomplete panoramas.*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Panoramic Settings")
	float VertFieldOfView = 0.f;

	/** When output stereo panorama, it is used to adjust the distance between the eyes */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Panoramic Settings")
	float EyeSeparation;

	/** Enable or not EyeConvergenceDistance*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Panoramic Settings",DisplayName="Enable Eye Convergence Distance")
	bool bEyeConvergenceDistance=true;
	/** When output stereo panorama, it is Used to set the focusing distance of the eyes */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Panoramic Settings")
	float EyeConvergenceDistance;
	
	
	
	/**
	* Should we store the history of each rendered scene?
	* This can consume a lot of memory with a lot of rendering,
	* but makes TAA and other history-based effects (de-noising, etc.) work.
	* In practice, the scene lumen is also affected by this parameter
	*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Panoramic Settings")
	bool bAllocateHistoryPerPane=false;

protected:
	// Shared pointer of the accumulation pool
	TSharedPtr<FAccumulatorPool, ESPMode::ThreadSafe> AccumulatorPool;
	// ToDo: One per high-res tile per pano-pane?
	// Reference to scene view state Optional Pane view state
	TArray<FSceneViewStateReference> OptionalPaneViewStates;
	// The life cycle of the SceneViewExtension is only during the rendering process, and its destruction is part of the TearDown
	TSharedPtr<FOpenColorIODisplayExtension, ESPMode::ThreadSafe> OCIOSceneViewExtension;
	// Panorama outputs a shared pointer to the mixed object
	TSharedPtr<MoviePipeline::IMoviePipelineOutputMerger> PanoramicOutputBlender;
	
	bool bHasWarnedSettings;
	
};
