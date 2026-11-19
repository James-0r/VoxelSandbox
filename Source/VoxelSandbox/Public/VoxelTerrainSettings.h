// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "VoxelTerrainSettings.generated.h"

class UCurveFloat;

/**
 * Tunables for the per-column terrain: surface height (several noise octaves shaped by HeightCurve),
 * surface layer depth and snow line. Used by VoxelGeneration::SampleColumn, which any chunk can call for any
 * world column, so a chunk can compute its neighbours' columns without them being loaded.
 * Defaults match DA_VoxelSettings at the time this struct was added.
 */
USTRUCT(BlueprintType)
struct VOXELSANDBOX_API FVoxelTerrainSettings
{
	GENERATED_BODY()

	/** Frequency of the first height octave (per cube). Lower = wider hills. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Height", meta = (ClampMin = "0.0", ToolTip = "Frequency of the first height noise octave (per cube). Lower = wider, gentler hills."))
	double NoiseFrequency = 0.02;

	/** Number of height noise layers added together. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Height", meta = (ClampMin = "1", ToolTip = "Number of height noise layers added together. Each extra octave adds smaller, finer bumps."))
	int32 Octaves = 3;

	/** Frequency multiplier from one octave to the next. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Height", meta = (ClampMin = "1.0", ToolTip = "Frequency multiplier per octave. 2 = each octave's bumps are half as wide as the previous one's."))
	double Lacunarity = 2.0;

	/** Strength multiplier from one octave to the next. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Height", meta = (ClampMin = "0.0", ClampMax = "1.0", ToolTip = "Strength multiplier per octave. 0.4 = each octave is 40% as strong as the previous one. Lower = smoother terrain."))
	double Persistence = 0.4;

	/** Maps the combined noise (about -1..1) to a fraction of WorldHeight. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Height", meta = (ToolTip = "Maps the combined height noise (about -1..1) to a fraction of WorldHeight. Shapes plains, hills and peaks."))
	TObjectPtr<UCurveFloat> HeightCurve = nullptr;

	/** Frequency of the surface layer depth noise (per cube). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Surface", meta = (ClampMin = "0.0", ToolTip = "Frequency of the noise that varies the grass / snow layer depth (per cube)."))
	double GrassDepthFrequency = 0.05;

	/** Thinnest surface layer, in cubes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Surface", meta = (ClampMin = "0", ToolTip = "Thinnest grass / snow layer, in cubes."))
	int32 GrassDepthMin = 1;

	/** Thickest surface layer, in cubes. Caves also stay this far below the surface. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Surface", meta = (ClampMin = "0", ToolTip = "Thickest grass / snow layer, in cubes. Caves also never carve within this many cubes of a column's top."))
	int32 GrassDepthMax = 4;

	/** Noise span (±) mapped onto the depth and snow line ranges. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Surface", meta = (ClampMin = "0.01", ToolTip = "Noise span (±) mapped onto GrassDepthMin..Max and ±SnowLineVariation. Perlin rarely reaches ±1, so ~0.3 uses the whole range."))
	double GrassNoiseRange = 0.3;

	/** Average snow line as a fraction of WorldHeight. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Snow", meta = (ClampMin = "0.0", ClampMax = "1.0", ToolTip = "Average snow line as a fraction of WorldHeight. Columns whose top reaches the snow line get snow instead of grass."))
	double SnowLineFraction = 0.65;

	/** Frequency of the snow line noise (per cube). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Snow", meta = (ClampMin = "0.0", ToolTip = "Frequency of the noise that moves the snow line up and down (per cube)."))
	double SnowLineFrequency = 0.08;

	/** How far the snow line moves up or down, in cubes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Snow", meta = (ClampMin = "0", ToolTip = "How far the snow line moves up or down from its average, in cubes."))
	int32 SnowLineVariation = 4;

	/** Height curve for the plains biome. None = no biomes, the whole world uses HeightCurve. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Biomes", meta = (ToolTip = "Height curve for the plains biome: same input as HeightCurve (height noise, about -1..1), usually much flatter. None = no biomes, the whole world uses HeightCurve (mountains)."))
	TObjectPtr<UCurveFloat> PlainsHeightCurve = nullptr;

	/** Frequency of the biome noise (per cube). Lower = bigger biomes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Biomes", meta = (ClampMin = "0.0", ToolTip = "Frequency of the noise that picks mountains or plains (per cube). A biome is roughly 1 / frequency cubes across: 0.004 ≈ 250 cubes ≈ 15 chunks."))
	double BiomeFrequency = 0.004;

	/** Biome noise below this is plains, above it mountains. Higher = more plains. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Biomes", meta = (ToolTip = "Biome noise below this value is plains, above it mountains. 0 = about half and half; higher = more plains, lower = more mountains. Perlin stays within about ±0.6."))
	double BiomeThreshold = 0.0;

	/** Half-width of the transition between biomes, in biome noise units. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Biomes", meta = (ClampMin = "0.0", ToolTip = "Half-width of the blend between plains and mountains, in biome noise units. Heights mix smoothly from Threshold − Blend to Threshold + Blend. Higher = wider, gentler foothills; 0 = a hard edge (cliffs)."))
	double BiomeBlend = 0.1;
};
