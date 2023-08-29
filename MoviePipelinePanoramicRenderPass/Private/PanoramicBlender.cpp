// Copyright MonsterGuoGuo. All Rights Reserved.2023
#include "PanoramicBlender.h"
#include "PanoramicPass.h"
#include "Async/ParallelFor.h"
// Constructor (fill in output combiner, fill in output resolution)
FPanoramicBlender::FPanoramicBlender(TSharedPtr<MoviePipeline::IMoviePipelineOutputMerger> InOutputMerger, const FIntPoint InOutputResolution)
	: OutputMerger(InOutputMerger)
{
	OutputEquirectangularMapSize = InOutputResolution;
}
/**************************** Color mapping *************************/
// Color linear interpolation, make the picture more soft
static FLinearColor GetColorBilinearFiltered(const FImagePixelData* InSampleData, const FVector2D& InSamplePixelCoords, bool& OutClipped, bool bIncludeAlpha = false)
{
	// Pixel coordinates assume that 0.5, 0.5 is the center of the pixel, so we subtract half to make it indexable.
	const FVector2D PixelCoordinateIndex = InSamplePixelCoords - 0.5f;
	
	// Get surrounding pixels indices
	FIntPoint LowerLeftPixelIndex = FIntPoint(FMath::RoundToInt(PixelCoordinateIndex.X), FMath::RoundToInt(PixelCoordinateIndex.Y));
	FIntPoint LowerRightPixelIndex = FIntPoint(LowerLeftPixelIndex + FIntPoint(1, 0));
	FIntPoint UpperLeftPixelIndex = FIntPoint(LowerLeftPixelIndex + FIntPoint(0, 1));
	FIntPoint UpperRightPixelIndex = FIntPoint(LowerLeftPixelIndex + FIntPoint(1, 1));
	// Clamp pixels indices to pixels array bounds.
	// ToDo: Is this needed? Should we handle wrap around for the bottom right pixel? What gives
	auto ClampPixelCoords = [&](FIntPoint& InOutPixelCoords, const FIntPoint& InArraySize)
	{
		if (InOutPixelCoords.X > InArraySize.X - 1 ||
			InOutPixelCoords.Y > InArraySize.Y - 1 ||
			InOutPixelCoords.X < 0 ||
			InOutPixelCoords.Y < 0)
		{
			OutClipped = true;
		}
		InOutPixelCoords = FIntPoint(FMath::Clamp(InOutPixelCoords.X, 0, InArraySize.X - 1), FMath::Clamp(InOutPixelCoords.Y, 0, InArraySize.Y - 1));
	};
	
	ClampPixelCoords(LowerLeftPixelIndex, InSampleData->GetSize());
	ClampPixelCoords(LowerRightPixelIndex, InSampleData->GetSize());
	ClampPixelCoords(UpperLeftPixelIndex, InSampleData->GetSize());
	ClampPixelCoords(UpperRightPixelIndex, InSampleData->GetSize());

	// Fetch the colors for the four pixels. We convert to FLinearColor here so that our accumulation
	// is done in linear space with enough precision. The samples are probably in F16 color right now.
	FLinearColor LowerLeftPixelColor;
	FLinearColor LowerRightPixelColor;
	FLinearColor UpperLeftPixelColor;
	FLinearColor UpperRightPixelColor;
	
	int64 SizeInBytes = 0;
	const void* SrcRawDataPtr = nullptr;
	InSampleData->GetRawData(SrcRawDataPtr, SizeInBytes);

	switch (InSampleData->GetType())
	{
		case EImagePixelType::Float16:
		{
			const FFloat16Color* ColorDataF16 = static_cast<const FFloat16Color*>(SrcRawDataPtr);
			LowerLeftPixelColor = FLinearColor(ColorDataF16[LowerLeftPixelIndex.X + (LowerLeftPixelIndex.Y * InSampleData->GetSize().X)]);
			LowerRightPixelColor = FLinearColor(ColorDataF16[LowerRightPixelIndex.X + (LowerRightPixelIndex.Y * InSampleData->GetSize().X)]);
			UpperLeftPixelColor = FLinearColor(ColorDataF16[UpperLeftPixelIndex.X + (UpperLeftPixelIndex.Y * InSampleData->GetSize().X)]);
			UpperRightPixelColor = FLinearColor(ColorDataF16[UpperRightPixelIndex.X + (UpperRightPixelIndex.Y * InSampleData->GetSize().X)]);
		}
		break;
		case EImagePixelType::Float32:
		{
			const FLinearColor* ColorDataF32 = static_cast<const FLinearColor*>(SrcRawDataPtr);
			LowerLeftPixelColor = ColorDataF32[LowerLeftPixelIndex.X + (LowerLeftPixelIndex.Y * InSampleData->GetSize().X)];
			LowerRightPixelColor = ColorDataF32[LowerRightPixelIndex.X + (LowerRightPixelIndex.Y * InSampleData->GetSize().X)];
			UpperLeftPixelColor = ColorDataF32[UpperLeftPixelIndex.X + (UpperLeftPixelIndex.Y * InSampleData->GetSize().X)];
			UpperRightPixelColor = ColorDataF32[UpperRightPixelIndex.X + (UpperRightPixelIndex.Y * InSampleData->GetSize().X)];
		}
		break;
		default:
		// Not implemented
			check(0);
	}

	// Interpolate between the 4 pixels based on the exact sub-pixel offset of the incoming coordinate (which may not be centered)
	const float FracX = FMath::Frac(InSamplePixelCoords.X);
	const float FracY = FMath::Frac(InSamplePixelCoords.Y);
	FLinearColor InterpolatedPixelColor = FMath::Lerp(FMath::Lerp(LowerLeftPixelColor,LowerRightPixelColor,FracX),
	 									FMath::Lerp(UpperLeftPixelColor,UpperRightPixelColor,FracX),FracY);
	// Force final color alpha to opaque if requested
	if (!bIncludeAlpha)
	{
		InterpolatedPixelColor.A = 1.0f;
	}

	return InterpolatedPixelColor;
}


DECLARE_CYCLE_STAT(TEXT("STAT_MoviePipeline_PanoBlend"), STAT_MoviePipeline_PanoBlend, STATGROUP_MoviePipeline);

// The callback function _ data after rendering the render channel allows running on any thread
void FPanoramicBlender::OnCompleteRenderPassDataAvailable_AnyThread(TUniquePtr<FImagePixelData>&& InData)
{
	SCOPE_CYCLE_COUNTER(STAT_MoviePipeline_PanoBlend);
	
	// This function is called whenever a sample is received from the GPU (after summing),
	// and it needs to process multiple samples from multiple frames at the same time.
	// The first step is to search to see if we're already printing a frame for this sample

	// Output frame
	TSharedPtr<FPanoramicOutputFrame> OutputFrame = nullptr;
	// The hybrid data object, which has a bunch of data in it, is used to make panoramas
	TSharedPtr<FPanoramicBlendData> BlendDataTarget = nullptr;

	// Panoramic image data load
	FPanoramicImagePixelDataPayload* DataPayload = InData->GetPayload<FPanoramicImagePixelDataPayload>();
	check(DataPayload);

	// Mixing start time
	const double BlendStartTime = FPlatformTime::Seconds();
	
	//We compute all of this on the task thread right away because it's read-only
	//and we need the information it computes to allocate memory when we start working on a sample.
	FIntPoint SampleSize = DataPayload->Pane.Resolution;
	// 是否启用半透明
	const bool bIncludeAlpha = DataPayload->Pane.bIncludeAlpha;
	// SampleRotation
	FTransform ActorTransform= FTransform(DataPayload->Pane.OriginalCameraRotation,DataPayload->Pane.OriginalCameraLocation,FVector(1.f, 1.f, 1.f));
	FRotator CameraRotation = FRotator(DataPayload->Pane.CameraRotation);
	FRotator SampleRotation = ActorTransform.InverseTransformRotation(CameraRotation.Quaternion()).Rotator();
	
	// Half horizontal FOV Angle of sample
	const float SampleHalfHorizontalFoVDegrees = 0.5f * DataPayload->Pane.HorizontalFieldOfView;  
	// half the vertical FOV angle of the sample
	const float SampleHalfVerticalFoVDegrees = 0.5f * DataPayload->Pane.VerticalFieldOfView;

	// Calculate the semi-horizontal FOVcoss value
	const float SampleHalfHorizontalFoVCosine = FMath::Cos(FMath::DegreesToRadians(SampleHalfHorizontalFoVDegrees));
	// Calculate the semi-vertical FOVcoss value
	const float SampleHalfVerticalFoVCosine = FMath::Cos(FMath::DegreesToRadians(SampleHalfVerticalFoVDegrees));
	
	// Now calculate the direction in which the panoramic pane (represented by this example) was originally oriented.
	// Radian of the sample Z axis
	const float SampleYawRad = FMath::DegreesToRadians(SampleRotation.Yaw);
	// Radian of the X axis of the sample
	const float SamplePitchRad = FMath::DegreesToRadians(SampleRotation.Pitch);
	
	// Sample direction vector, Z axis rotation theta, scalar. Based on world coordinates
	const FVector SampleDirectionOnTheta = FVector(FMath::Cos(SampleYawRad), FMath::Sin(SampleYawRad), 0);
	// Sample direction vector, Y axis rotation φ, quantized. Based on world coordinates
	const FVector SampleDirectionOnPhi = FVector(FMath::Cos(SamplePitchRad), 0.f, FMath::Sin(SamplePitchRad));
	
	// Now construct a projection matrix that represents the samples that match the original perspective.
	const FMatrix SampleProjectionMatrix = FReversedZPerspectiveMatrix(FMath::DegreesToRadians(SampleHalfHorizontalFoVDegrees), SampleSize.X, SampleSize.Y, DataPayload->Pane.NearClippingPlane);

	// For a given output size, figure out how many degrees each pixel represents.
	const float EquiRectMapThetaStep = 360.f / (float)OutputEquirectangularMapSize.X; 
	const float EquiRectMapPhiStep = 180.f / (float)OutputEquirectangularMapSize.Y;
	
	/************************** The maximum and minimum value of the Z axis direction ************************************/
	
	// What is calculated here is the maximum and minimum Yaw of the sample: there is a problem here. When it rotates to 45 degrees, it is diagonally.
	float SampleYawMin = SampleRotation.Yaw - SampleHalfHorizontalFoVDegrees;
	float SampleYawMax = SampleRotation.Yaw + SampleHalfHorizontalFoVDegrees;
	int32 PixelIndexHorzMinBound = FMath::FloorToInt(((SampleYawMin) + 180.f) / EquiRectMapThetaStep);
	int32 PixelIndexHorzMaxBound = FMath::FloorToInt(((SampleYawMax) + 180.f) / EquiRectMapThetaStep); 

	
	// About restrictions in the vertical direction
	float SamplePitchMin = FMath::Max(SampleRotation.Pitch - SampleHalfVerticalFoVDegrees, -90.f); // Clamped to [-90, 90]
	float SamplePitchMax = FMath::Min(SampleRotation.Pitch + SampleHalfVerticalFoVDegrees, 90.f); // Clamped to [-90, 90]
	int32 PixelIndexVertMinBound = FMath::Max((OutputEquirectangularMapSize.Y) - FMath::FloorToInt((SamplePitchMax + 90.f) / EquiRectMapPhiStep), 0);
	int32 PixelIndexVertMaxBound = FMath::Min((OutputEquirectangularMapSize.Y) - FMath::FloorToInt((SamplePitchMin + 90.f) / EquiRectMapPhiStep), OutputEquirectangularMapSize.Y);

	// Check the pending items
	{
		// When we iterate/add the PendingData array, a quick lock is performed so that a second sample does not appear during the iteration.
		FScopeLock ScopeLock(&GlobalQueueDataMutex);
		
		for (TPair<FMoviePipelineFrameOutputState, TSharedPtr<FPanoramicOutputFrame>>& KVP : PendingData)
		{
			if (KVP.Key.OutputFrameNumber == DataPayload->SampleState.OutputState.OutputFrameNumber)
			{
				OutputFrame = KVP.Value;
				break;
			}
		}
		// Add data if it is empty
		if (!OutputFrame)
		{
			// Start a new output frame in Panorama Mixed frame = Number of output frames for sample state in data load
			OutputFrame = PendingData.Add(DataPayload->SampleState.OutputState, MakeShared<FPanoramicOutputFrame>());
			int32 EyeMultiplier = DataPayload->Pane.EyeIndex == -1 ? 1 : 2;
			int32 TotalSampleCount = DataPayload->Pane.NumHorizontalSteps * DataPayload->Pane.NumVerticalSteps * EyeMultiplier;
			OutputFrame->NumSamplesTotal = TotalSampleCount;
			{
				// Log macro
				LLM_SCOPE_BYNAME(TEXT("MoviePipeline/PanoBlendFrameOutput"));
				// An array of (panoramic pixels) mapped by an isometric cylinder of the output frame. 
				// Set the number of arrays and fill the data bits to 0
				OutputFrame->OutputEquirectangularMap.SetNumZeroed(OutputEquirectangularMapSize.X * OutputEquirectangularMapSize.Y * EyeMultiplier);
				if(bIncludeAlpha)
				{
					OutputFrame->AlphaArray.SetNumZeroed(OutputEquirectangularMapSize.X * OutputEquirectangularMapSize.Y * EyeMultiplier);
				}
			}
		}
	}
	
	// Now that we know which output frame we are contributing to,
	// we will ask it for our own copy of the data so that we can mix without worrying about other threads.

	// Check whether there is something in the output frame object, add it if not, and create a storage space by the way.
	check(OutputFrame);
	{
		FScopeLock ScopeLock(&GlobalQueueDataMutex);
		BlendDataTarget = MakeShared<FPanoramicBlendData>();
		BlendDataTarget->EyeIndex = DataPayload->Pane.EyeIndex;
		BlendDataTarget->bFinished = false;
		BlendDataTarget->OriginalDataPayload = StaticCastSharedRef<FPanoramicImagePixelDataPayload>(DataPayload->Copy());
		// In fact, this is just adding in, if you find this array of eyes
		TArray<TSharedPtr<FPanoramicBlendData>>& EyeArray = OutputFrame->BlendedData.FindOrAdd(DataPayload->Pane.EyeIndex);
		EyeArray.Add(BlendDataTarget);
	}
	
	// Ok, so our BlendDataTarget is now the only copy of the data we want to process ourselves.
	BlendDataTarget->BlendStartTime = BlendStartTime;
	// Build a rectangle that describes which part of the output Map we will render to
	BlendDataTarget->OutputBoundsMin = FIntPoint(PixelIndexHorzMinBound, PixelIndexVertMinBound);
	BlendDataTarget->OutputBoundsMax = FIntPoint(PixelIndexHorzMaxBound, PixelIndexVertMaxBound);

	// Mixed data object (Pane) pixel width and height, which is equivalent to the process of drawing a grid.
	BlendDataTarget->PixelWidth = BlendDataTarget->OutputBoundsMax.X - BlendDataTarget->OutputBoundsMin.X;
	BlendDataTarget->PixelHeight = BlendDataTarget->OutputBoundsMax.Y - BlendDataTarget->OutputBoundsMin.Y;

	// These need to be zeroed as we don't always touch every pixel in the rect with blending
	// and they get +=
	{
		LLM_SCOPE_BYNAME(TEXT("MoviePipeline/PanoBlendPerTaskOutput"));
		BlendDataTarget->Data.SetNumZeroed((BlendDataTarget->PixelWidth) * (BlendDataTarget->PixelHeight));
		if(bIncludeAlpha)
		{
			BlendDataTarget->AlphaArray.SetNumZeroed((BlendDataTarget->PixelWidth) * (BlendDataTarget->PixelHeight));
		}
		
	}

	/***************************************** Pixel processing process ******************************************************/
	
	// Finally, we can perform the actual blending, which we mix into the intermediate buffer rather than the final output array to avoid multiple threads contending for pixels
	for (int32 Y = PixelIndexVertMinBound; Y < PixelIndexVertMaxBound; Y++)
	{
		for (int32 X = PixelIndexHorzMinBound; X < PixelIndexHorzMaxBound; X++)
		{
			// These X-ray coordinates are in the output resolution space, and this is where we want to mix them.
			// Our X limit may be OOB, but we wrap horizontally, so we need to find the appropriate X index.
			const int32 OutputPixelX = ((X % OutputEquirectangularMapSize.X) + OutputEquirectangularMapSize.X) % OutputEquirectangularMapSize.X;
			const int32 OutputPixelY = Y;
			
			// Obtain the spherical coordinates (Theta and Phi) corresponding to the X and Y of the isometric map coordinates,
			// converted to the [-180,180] and [-90, 90] coordinate Spaces, respectively.
			// The half-pixel offset is used so that the center of the pixel is treated as that coordinate, and Phi increases in the opposite direction to Y.
			// Add or subtract 0.5 to shift to the center of the pixel
			const float Theta = EquiRectMapThetaStep * (((float)OutputPixelX) + 0.5f) - 180.f;
			const float Phi = EquiRectMapPhiStep * (((float)OutputEquirectangularMapSize.Y - OutputPixelY) + 0.5f) - 90.f;

			// Now convert the spherical coordinates into an actual direction (on the output map)
		
			// Convert to radians for subsequent calculations
			const float ThetaDeg = FMath::DegreesToRadians(Theta);
			const float PhiDeg = FMath::DegreesToRadians(Phi);
			// The output direction of the pixel
			const FVector OutputDirection(FMath::Cos(PhiDeg) * FMath::Cos(ThetaDeg), FMath::Cos(PhiDeg) * FMath::Sin(ThetaDeg), FMath::Sin(PhiDeg));
			// Pixel output direction Theta
			const FVector OutputDirectionTheta = FVector(FMath::Cos(ThetaDeg), FMath::Sin(ThetaDeg), 0);
			// Pixel output direction Phi
			const FVector OutputDirectionPhi = FVector(FMath::Cos(PhiDeg), 0.f, FMath::Sin(PhiDeg));

			// Now we can compute how much the sample should influence this pixel. It is weighted by angular distance to the direction
			// so that the edges have less influence (where they'd be more distorted anyways).
			// ToDo: This only considers the whole Pano Pane and not per pixel of sample?
			const float DirectionThetaDot = FVector::DotProduct(OutputDirectionTheta, SampleDirectionOnTheta);
			const float DirectionPhiDot = FVector::DotProduct(OutputDirectionPhi, SampleDirectionOnPhi);
			const float WeightTheta = FMath::Max(DirectionThetaDot - SampleHalfHorizontalFoVCosine, 0.0f) / (1.0f - SampleHalfHorizontalFoVCosine);
			const float WeightPhi = FMath::Max(DirectionPhiDot - SampleHalfVerticalFoVCosine, 0.0f) / (1.0f - SampleHalfVerticalFoVCosine);
			const float SampleWeight = WeightTheta * WeightPhi;
			const float SampleWeightSquared = SampleWeight* SampleWeight; // Exponential falloff produces a nicer blending result.

			// The sample weight may be very small and not worth influencing this pixel.
			if (SampleWeightSquared > KINDA_SMALL_NUMBER)
			{
				FVector4 DirectionInSampleWorldSpace = FVector4(SampleRotation.UnrotateVector(OutputDirection), 1.0f);
				static const FMatrix UnrealCoordinateConversion = FMatrix(
					FPlane(0, 0, 1, 0),
					FPlane(1, 0, 0, 0),
					FPlane(0, 1, 0, 0),
					FPlane(0, 0, 0, 1));
				DirectionInSampleWorldSpace = UnrealCoordinateConversion.TransformFVector4(DirectionInSampleWorldSpace);
				// Then project that direction into sample clip space
				FVector4 DirectionInSampleClipSpace = SampleProjectionMatrix.TransformFVector4(DirectionInSampleWorldSpace);

				// Converted into normalized device space (Divide by w for perspective)
				FVector DirectionInSampleNDSpace = FVector(DirectionInSampleClipSpace) / DirectionInSampleClipSpace.W;

				// Get the final pixel coordinates (direction in screen space)
				FVector2D DirectionInSampleScreenSpace = ((FVector2D(DirectionInSampleNDSpace) + 1.0f) / 2.0f) * FVector2D(SampleSize.X, SampleSize.Y);
				
				// Flip the Y value due to Y's zero coordinate being top left.
				DirectionInSampleScreenSpace.Y = ((float)SampleSize.Y - DirectionInSampleScreenSpace.Y) - 1.0f;
				// What is calculated here is the position of the pixels in the screen at the pixel coordinates (from the sample) execute a bilinear color sample, weight it,
				// and add it to the output map. We store the weights separately so that we can preserve the alpha channel of the main image.
				bool bClipped = false;
				FLinearColor SampleColor = GetColorBilinearFiltered(InData.Get(), DirectionInSampleScreenSpace, bClipped, bIncludeAlpha);
				
				if (!bClipped)
				{
					// When we calculate the actual output position, we need to move X/Y. This is because until now, the math has been done in the output resolution space, but each sample is only assigned a large enough color plot.
					// It will be moved back to the correct position.
					int32 SampleOutputX = OutputPixelX - BlendDataTarget->OutputBoundsMin.X;
					// Mod this again by our output map so we don't OOB on it. It'll wrap weirdly in the output map but should restore fine.
					SampleOutputX = ((SampleOutputX % (BlendDataTarget->PixelWidth)) + (BlendDataTarget->PixelWidth)) % (BlendDataTarget->PixelWidth); // Positive Mod
					int32 SampleOutputY = Y;
					SampleOutputY -= BlendDataTarget->OutputBoundsMin.Y;
						
					const int32 FinalIndex = SampleOutputX + (SampleOutputY * (BlendDataTarget->PixelWidth));
					BlendDataTarget->Data[FinalIndex] += SampleColor * SampleWeightSquared;
					if(bIncludeAlpha)
					{
						BlendDataTarget->AlphaArray[FinalIndex] += SampleWeightSquared;
					}
					// OutputEquirectangularMap[OutputPixelX + (OutputPixelY * OutputEquirectangularMapSize.X)] += SampleColor * SampleWeightSquared; // ToDo move to weight map.
				}
			}
		}
	}
	
	BlendDataTarget->BlendEndTime = FPlatformTime::Seconds();

	/************************************ The main work in this section is pixel mapping **************************************/
	// Mix the new sample into the output map as soon as possible so that we can free up the temporary memory occupied by the sample.
	// This part is single-threaded (as opposed to other tasks).
	{
		// Lock access to our output map
		FScopeLock ScopeLock(&OutputDataMutex);
		
		int32 EyeOffset = 0;
		// If the window number of the original data payload of the mixed data target is not equal to -1
		if (BlendDataTarget->OriginalDataPayload->Pane.EyeIndex != -1)
		{
			EyeOffset = (OutputEquirectangularMapSize.X * OutputEquirectangularMapSize.Y) * BlendDataTarget->OriginalDataPayload->Pane.EyeIndex;
		}
		// Traversal of sample Y when the position of sample Y is < less than the mixed data target. High pixel;
		for (int32 SampleY = 0; SampleY < BlendDataTarget->PixelHeight; SampleY++)
		{
			for (int32 SampleX = 0; SampleX < BlendDataTarget->PixelWidth; SampleX++)
			{
				int32 OriginalX = SampleX + BlendDataTarget->OutputBoundsMin.X;
				int32 OriginalY = SampleY + BlendDataTarget->OutputBoundsMin.Y;
				
				const int32 OutputPixelX = ((OriginalX % OutputEquirectangularMapSize.X) + OutputEquirectangularMapSize.X) % OutputEquirectangularMapSize.X;
				const int32 OutputPixelY = OriginalY;
				
				int32 SourceIndex = SampleX + (SampleY * (BlendDataTarget->PixelWidth));
				int32 DestIndex = OutputPixelX + (OutputPixelY * OutputEquirectangularMapSize.X);
				OutputFrame->OutputEquirectangularMap[DestIndex + EyeOffset] += BlendDataTarget->Data[SourceIndex];
				if(bIncludeAlpha)
				{
					OutputFrame->AlphaArray[DestIndex + EyeOffset] += BlendDataTarget->AlphaArray[SourceIndex];
				}
			}
		}
		
 		bool bDebugSamples = DataPayload->SampleState.bWriteSampleToDisk;
		if (bDebugSamples)
		{
			// Write each blended sample to the output as a debug sample so we can inspect the job blending is doing for each pane.
			// Hack up the debug output name a bit so they're unique.
			if (BlendDataTarget->OriginalDataPayload->Pane.EyeIndex >= 0)
			{
				BlendDataTarget->OriginalDataPayload->Debug_OverrideFilename = FString::Printf(TEXT("/%s_PaneX_%d_PaneY_%dEye_%d-Blended.%d"),
					*BlendDataTarget->OriginalDataPayload->PassIdentifier.Name, BlendDataTarget->OriginalDataPayload->Pane.HorizontalStepIndex,
					BlendDataTarget->OriginalDataPayload->Pane.VerticalStepIndex, DataPayload->Pane.EyeIndex, BlendDataTarget->OriginalDataPayload->SampleState.OutputState.OutputFrameNumber);
			}
			else
			{
				BlendDataTarget->OriginalDataPayload->Debug_OverrideFilename = FString::Printf(TEXT("/%s_PaneX_%d_PaneY_%d-Blended.%d"),
					*BlendDataTarget->OriginalDataPayload->PassIdentifier.Name, BlendDataTarget->OriginalDataPayload->Pane.HorizontalStepIndex,
					BlendDataTarget->OriginalDataPayload->Pane.VerticalStepIndex, BlendDataTarget->OriginalDataPayload->SampleState.OutputState.OutputFrameNumber);
			}

			// Now that the sample has been blended pass it (and the memory it owned, we already read from it) to the debug output step.
			TUniquePtr<TImagePixelData<FLinearColor>> FinalPixelData = MakeUnique<TImagePixelData<FLinearColor>>(FIntPoint(BlendDataTarget->PixelWidth, BlendDataTarget->PixelHeight), TArray64<FLinearColor>(MoveTemp(BlendDataTarget->Data)), BlendDataTarget->OriginalDataPayload);
			ensure(OutputMerger.IsValid());
			OutputMerger.Pin()->OnSingleSampleDataAvailable_AnyThread(MoveTemp(FinalPixelData));
		}
		else
		{
			// Ensure we reset the memory we allocated, to minimize concurrent allocations.
			BlendDataTarget->Data.Reset();
			BlendDataTarget->Data.Empty();
			if(bIncludeAlpha)
			{
				BlendDataTarget->AlphaArray.Reset();
				BlendDataTarget->AlphaArray.Empty();
			}
		}
	}

	/************************ This section is to check if it's the last sample ***************************/
	// This should only be set to True if the thread actually does the work.
	bool bIsLastSample = false;
	{
		FScopeLock ScopeLock(&GlobalQueueDataMutex);
		// Data blending complete
		BlendDataTarget->bFinished = true;
		
		// Check if all samples "are from GPU" and "have been mixed"
		int32 NumFinishedSamples = 0;
		
		// Go through every eye
		for(TPair<int32, TArray<TSharedPtr<FPanoramicBlendData>>>& KVP : OutputFrame->BlendedData)
		{
			// For each sample in the eye
			for (int32 Index = 0; Index < KVP.Value.Num(); Index++)
			{
				NumFinishedSamples += KVP.Value[Index]->bFinished ? 1 : 0;
			}
		}
		bIsLastSample = NumFinishedSamples == OutputFrame->NumSamplesTotal;
	}

	/*************************** Color in if it's the last one ************************/
	if (bIsLastSample)
	{
		{
			// Now that we have accumulated the values of all the pixels, we need to scale them
			for (int32 PixelIndex = 0; PixelIndex < OutputFrame->OutputEquirectangularMap.Num(); PixelIndex++)
			{
				FLinearColor& Pixel = OutputFrame->OutputEquirectangularMap[PixelIndex];
				if(bIncludeAlpha)
				{
					float& AlphaNum = OutputFrame->AlphaArray[PixelIndex];
					Pixel.R /= AlphaNum;
					Pixel.G /= AlphaNum;
					Pixel.B /= AlphaNum;
					Pixel.A /= AlphaNum;
				}
				else
				{
					Pixel.R /= Pixel.A;
					Pixel.G /= Pixel.A;
					Pixel.B /= Pixel.A;
					Pixel.A = 1;
				}
			}
		}
		TSharedRef<FImagePixelDataPayload, ESPMode::ThreadSafe> NewPayload = DataPayload->Copy();
		int32 OutputSizeX = OutputEquirectangularMapSize.X;
		int32 OutputSizeY = DataPayload->Pane.EyeIndex >= 0 ? OutputEquirectangularMapSize.Y * 2 : OutputEquirectangularMapSize.Y;
		TUniquePtr<TImagePixelData<FLinearColor>> FinalPixelData = MakeUnique<TImagePixelData<FLinearColor>>(FIntPoint(OutputSizeX, OutputSizeY), TArray64<FLinearColor>(MoveTemp(OutputFrame->OutputEquirectangularMap)), NewPayload);
		
		if(ensure(OutputMerger.IsValid()))
		{
			OutputMerger.Pin()->OnCompleteRenderPassDataAvailable_AnyThread(MoveTemp(FinalPixelData));
		}
		PendingData.Remove(DataPayload->SampleState.OutputState);
	}
}

void FPanoramicBlender::OnSingleSampleDataAvailable_AnyThread(TUniquePtr<FImagePixelData>&& InData)
{
	// This is a debug output, directly output through it
	ensure(OutputMerger.IsValid());
	// Whether sample data is available in any thread
	OutputMerger.Pin()->OnSingleSampleDataAvailable_AnyThread(MoveTemp(InData));
}

// Setting a "Movie pipeline fake output frame" type is a movie pipeline merge output frame,
// the specific impact of this is unknown. But there is no other way to do it here.
static FMoviePipelineMergerOutputFrame MoviePipelineDummyOutputFrame;
FMoviePipelineMergerOutputFrame& FPanoramicBlender::QueueOutputFrame_GameThread(const FMoviePipelineFrameOutputState& CachedOutputState)
{
	check(0);
	return  MoviePipelineDummyOutputFrame;
}

void FPanoramicBlender::AbandonOutstandingWork()
{
	check(0);
}

FPanoramicBlender::~FPanoramicBlender()
{
	PendingData.Empty(0);
}

