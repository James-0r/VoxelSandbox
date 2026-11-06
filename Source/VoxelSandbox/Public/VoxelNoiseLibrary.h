// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "VoxelNoiseLibrary.generated.h"

/** Exposes the engine's Perlin noise to Blueprint for terrain and cave generation. */
UCLASS()
class VOXELSANDBOX_API UVoxelNoiseLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * 2D Perlin noise in [-1, 1].
	 * Returns 0 at whole numbers, so scale inputs by a frequency before sampling.
	 */
	UFUNCTION(BlueprintPure, Category = "Voxel|Noise")
	static float PerlinNoise2D(float X, float Y);

	/** 3D Perlin noise in [-1, 1]. Used for caves. */
	UFUNCTION(BlueprintPure, Category = "Voxel|Noise")
	static float PerlinNoise3D(float X, float Y, float Z);
};
