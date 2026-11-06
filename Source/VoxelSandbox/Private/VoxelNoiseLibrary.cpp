// Fill out your copyright notice in the Description page of Project Settings.

#include "VoxelNoiseLibrary.h"

float UVoxelNoiseLibrary::PerlinNoise2D(float X, float Y)
{
	return FMath::PerlinNoise2D(FVector2D(X, Y));
}

float UVoxelNoiseLibrary::PerlinNoise3D(float X, float Y, float Z)
{
	return FMath::PerlinNoise3D(FVector(X, Y, Z));
}
