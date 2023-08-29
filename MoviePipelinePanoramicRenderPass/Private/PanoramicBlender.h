//Copyright MonsterGuoGuo. All Rights Reserved.2023
#pragma once

#include "MoviePipelineImagePassBase.h"
#include "MovieRenderPipelineDataTypes.h"

// Forward Declares
struct FImagePixelData;
class UMoviePipeline;

class FPanoramicBlender : public MoviePipeline::IMoviePipelineOutputMerger
{
public:
	FPanoramicBlender(TSharedPtr<MoviePipeline::IMoviePipelineOutputMerger> InOutputMerger, const FIntPoint InOutputResolution);
	~FPanoramicBlender();

public:
	virtual FMoviePipelineMergerOutputFrame& QueueOutputFrame_GameThread(const FMoviePipelineFrameOutputState& CachedOutputState) override;
	virtual void OnCompleteRenderPassDataAvailable_AnyThread(TUniquePtr<FImagePixelData>&& InData) override;
	virtual void OnSingleSampleDataAvailable_AnyThread(TUniquePtr<FImagePixelData>&& InData) override;
	virtual void AbandonOutstandingWork() override;
	virtual int32 GetNumOutstandingFrames() const override { return PendingData.Num(); }
	
private:
	struct FPanoramicBlendData
	{
		double BlendStartTime;			
		double BlendEndTime;			
		bool bFinished;					
		FIntPoint OutputBoundsMin;		
		FIntPoint OutputBoundsMax;		
		int32 PixelWidth;				
		int32 PixelHeight;				
		TArray<FLinearColor> Data;
		TArray<float> AlphaArray;
		int32 EyeIndex;					
		TSharedPtr<struct FPanoramicImagePixelDataPayload> OriginalDataPayload;
	};

	// Panoramic output frame
	struct FPanoramicOutputFrame:FMoviePipelineMergerOutputFrame
	{
		// Eye Index to Blend Data. Eye Index will be -1 when not using Stereo.
		TMap<int32, TArray<TSharedPtr<FPanoramicBlendData>>> BlendedData;

		// The total number of samples we have to wait for to finish blending before being 'done'.
		int32 NumSamplesTotal;

		// Linear color output isometric cylindrical Map (actually a panoramic array of color information)
		TArray<FLinearColor> OutputEquirectangularMap;
		// 透明通道
		TArray<float> AlphaArray;
	};

	/** Data that is expected but not fully available yet. */
	TMap<FMoviePipelineFrameOutputState, TSharedPtr<FPanoramicOutputFrame>> PendingData;
	/** Mutex that protects adding/updating/removing from PendingData */
	FCriticalSection GlobalQueueDataMutex;		
	FCriticalSection OutputDataMutex;
	
	// Output the dimensions of the isometric cylindrical map, which is actually the output
	FIntPoint OutputEquirectangularMapSize;
	
	// A weak pointer to the movie output merger of a movie pipeline
	TWeakPtr<MoviePipeline::IMoviePipelineOutputMerger> OutputMerger;
};