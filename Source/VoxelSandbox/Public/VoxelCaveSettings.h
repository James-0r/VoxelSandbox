// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "VoxelCaveSettings.generated.h"

class UCurveFloat;

/**
 * Tunables for AVoxelChunkBase::CarveCaves: big cheese caverns from one noise, round tunnels from the
 * intersection of two noises. All noise is sampled in cube units, so CubeSize does not change cave sizes.
 */
USTRUCT(BlueprintType)
struct VOXELSANDBOX_API FVoxelCaveSettings
{
	GENERATED_BODY()

	/** Noise frequency for cheese caverns (per cube). Lower = bigger, rarer caverns. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Caves|Cheese", meta = (ClampMin = "0.0", ToolTip = "Noise frequency for cheese caverns (per cube). Lower = bigger, rarer caverns."))
	double CheeseFrequency = 0.02;

	/** Cheese threshold by depth fraction (0 = bottom, 1 = surface). A cell is carved where the cheese noise is above this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Caves|Cheese", meta = (ToolTip = "Cheese threshold by depth fraction (0 = bottom, 1 = surface). A cell is carved where the cheese noise is above the curve. Higher = fewer caverns."))
	TObjectPtr<UCurveFloat> CaveCurve = nullptr;

	/** Noise frequency of the two tunnel noises (per cube). Lower = longer, straighter tunnels. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Caves|Tunnels", meta = (ClampMin = "0.0", ToolTip = "Frequency of the two tunnel noises (per cube). Lower = longer, straighter tunnels that are also wider for the same width values."))
	double TunnelFrequency = 0.025;

	/** Narrowest tunnel, in noise units. Tunnel diameter in cubes ≈ 2 × width ÷ TunnelFrequency (≈ 80 × width at 0.025). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Caves|Tunnels", meta = (ClampMin = "0.0", ToolTip = "Narrowest tunnel, in noise units. Diameter in cubes ≈ 2 × width ÷ TunnelFrequency, so at 0.025 one width unit ≈ 80 cubes: 0.04 ≈ 3 cubes. Independent of CubeSize. Approximate: noise slope varies."))
	double TunnelWidthMin = 0.04;

	/** Widest tunnel, in noise units. Same relation as TunnelWidthMin: 0.08 ≈ 6–7 cubes at TunnelFrequency 0.025. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Caves|Tunnels", meta = (ClampMin = "0.0", ToolTip = "Widest tunnel, in noise units. Diameter in cubes ≈ 2 × width ÷ TunnelFrequency: 0.08 ≈ 6–7 cubes at 0.025. Independent of CubeSize."))
	double TunnelWidthMax = 0.08;

	/** Frequency of the width noise. Lower = width changes more slowly along a tunnel. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Caves|Tunnels", meta = (ClampMin = "0.0", ToolTip = "Frequency of the noise that picks the tunnel width (per cube). Lower = width changes more slowly along a tunnel."))
	double TunnelWidthFrequency = 0.01;

	/** Multiplies the tunnel noise frequency in Z. Above 1 = flatter tunnels that run more horizontally. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Caves|Tunnels", meta = (ClampMin = "0.0", ToolTip = "Multiplies the tunnel noise frequency in Z. 2 = tunnels about half as tall as wide and mostly horizontal; 1 = round and free to go up or down."))
	double TunnelVerticalScale = 2.0;

	/** No carving closer than this many cubes to the surface, measured against the lowest surface nearby. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Caves|Surface", meta = (ClampMin = "0", ToolTip = "Minimum solid cubes between a cave and the surface. Measured down from the lowest column top within SurfaceMargin + SurfaceFade columns (neighbour chunks included), so hillsides and cliffs keep a thick wall too. Higher = caves sit deeper and never break out."))
	int32 SurfaceMargin = 4;

	/** Over this many cubes beyond SurfaceMargin, caves ease in: cheese needs a higher threshold and tunnels are narrower. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Caves|Surface", meta = (ClampMin = "0", ToolTip = "Soft band below the margin, in cubes. Cheese caverns need a stronger noise and tunnels shrink, both blending to normal at SurfaceMargin + SurfaceFade. 0 = hard cut at the margin. It also widens the sideways search radius."))
	int32 SurfaceFade = 4;
};
