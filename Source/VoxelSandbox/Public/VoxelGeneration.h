// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "VoxelCaveSettings.h"
#include "VoxelTerrainSettings.h"

/**
 * Everything one chunk's generation needs. Copied by value into each generation call, so the generation
 * code never reads an actor or a data asset while it runs (that's what will let it run off the game thread).
 */
struct FVoxelGenerationParams
{
	/** Column height, surface depth and snow line tunables. */
	FVoxelTerrainSettings Terrain;

	/** Cave tunables. */
	FVoxelCaveSettings Caves;

	/** Per-seed offset added to every noise sample (BP_VoxelWorld.NoiseOffset). */
	FVector NoiseOffset = FVector::ZeroVector;

	/** Number of cube layers in Z. */
	int32 WorldHeight = 0;

	/** Solid layers at the bottom that caves never carve. */
	int32 BedrockLayers = 0;

	/** Turns the cave pass on or off. */
	bool bEnableCaves = true;
};

/**
 * One chunk's cube and column data. Plain C++ (not a UObject), so it can be built anywhere and then moved
 * into a chunk actor in one step.
 */
struct FVoxelChunkData
{
	/** Cube bytes, Index = X + Y*ChunkSize + Z*ChunkSize². */
	TArray<uint8> Cubes;

	/** Top filled Z per column, Index = X + Y*ChunkSize. */
	TArray<int32> Heights;

	/** Surface layer depth per column. */
	TArray<int32> GrassDepths;

	/** Snow line per column. */
	TArray<int32> SnowLines;

	/** Number of cube layers in Z. */
	int32 WorldHeight = 0;

	/** Sizes every array for InWorldHeight layers and fills them with Air / 0. */
	void Init(int32 InWorldHeight);

	/** True once Init has run with a positive height. */
	bool IsInitialised() const;
};

/** Surface height, surface layer depth and snow line of one column. */
struct FVoxelColumn
{
	int32 Height = 0;
	int32 GrassDepth = 0;
	int32 SnowLine = 0;
};

/**
 * Chunk generation as pure functions: inputs in, data out, no actors and no world lookups.
 * Two chunks that share a border compute the same columns for it from the same noise, so each chunk can be
 * generated alone, in any order, and still match its neighbours exactly.
 */
namespace VoxelGeneration
{
	/** Flat index of a local cell: X + Y*ChunkSize + Z*ChunkSize². Caller checks the range. */
	int32 CubeIndex(int32 X, int32 Y, int32 Z);

	/** Flat index of a column: X + Y*ChunkSize. Caller checks the range. */
	int32 ColumnIndex(int32 X, int32 Y);

	/** True when X and Y are inside a chunk (0..ChunkSize−1). */
	bool IsInsideColumns(int32 X, int32 Y);

	/** GetTypeForCell rule: top TopDepth cubes are Snow if the surface reaches SnowLine, else Grass; below is Rock. */
	uint8 GetTypeForCell(int32 Z, int32 SurfaceZ, int32 TopDepth, int32 SnowLine);

	/** Height noise at a world column: Octaves layers of Perlin, normalised to about -1..1. Port of SampleHeightNoise. */
	double SampleHeightNoise(double WorldX, double WorldY, const FVoxelTerrainSettings& Terrain, const FVector& NoiseOffset);

	/** Surface height of the world column (CellX, CellY): height noise → HeightCurve → clamped cube count. */
	int32 SampleColumnHeight(int32 CellX, int32 CellY, const FVoxelTerrainSettings& Terrain, const FVector& NoiseOffset, int32 WorldHeight);

	/** Height, surface depth and snow line of the world column (CellX, CellY). Port of the GenerateTerrain column maths. */
	FVoxelColumn SampleColumn(int32 CellX, int32 CellY, const FVoxelTerrainSettings& Terrain, const FVector& NoiseOffset, int32 WorldHeight);

	/**
	 * How many columns beyond its own edge a chunk must know to generate alone. Only the caves' surface margin
	 * reads neighbouring columns (the lowest surface within SurfaceMargin + SurfaceFade), so that's the padding.
	 */
	int32 GetColumnPadding(const FVoxelCaveSettings& Caves);

	/**
	 * Samples every column of a chunk plus Padding columns on every side. The chunk's own columns go into Data;
	 * all heights (own and padding) go into OutPaddedHeights, row by row, (ChunkSize + 2 × Padding)² entries.
	 */
	void SampleColumns(FIntPoint ChunkCoord, int32 Padding, const FVoxelGenerationParams& Params, FVoxelChunkData& Data,
		TArray<int32>& OutPaddedHeights);

	/** Fills every column from Z 0 to its Height with Rock / Grass / Snow (GetTypeForCell). */
	void FillColumns(FVoxelChunkData& Data);

	/**
	 * For each of the chunk's columns, the lowest height within a circle of Radius columns, read from a padded
	 * height grid (Padding ≥ Radius). Entries of MissingHeight are skipped. Used for the cave surface margin.
	 */
	void ComputeLowestNearbyHeights(const TArray<int32>& PaddedHeights, int32 Padding, int32 Radius, TArray<int32>& OutLowest);

	/** Height value that marks a padded column with no data; ComputeLowestNearbyHeights skips it. */
	inline constexpr int32 MissingHeight = TNumericLimits<int32>::Max();

	/**
	 * Highest Z the caves may carve in a column: min(Height − GrassDepthMax, SurfaceZ − SurfaceMargin), where
	 * SurfaceZ is the lower of the column's own height and the lowest height nearby.
	 */
	int32 GetCaveTopZ(int32 Height, int32 SurfaceZ, const FVoxelCaveSettings& Caves, int32 GrassDepthMax);

	/**
	 * The cave rule for one cell (cheese caverns or round tunnels, eased in near the surface). Position is the
	 * cell in noise space: (world X + NoiseOffset.X, world Y + NoiseOffset.Y, Z + NoiseOffset.Z). The caller
	 * checks that Z is between BedrockLayers and GetCaveTopZ. Shared by CarveCaves and the worms' "did I reach a
	 * cave" test, so both always agree on where caves are.
	 */
	bool ShouldCarveCaveCell(const FVector& Position, int32 Z, int32 Height, int32 SurfaceZ, const FVoxelCaveSettings& Caves);

	/**
	 * Carves cheese caverns and round tunnels into Data. LowestNearby comes from ComputeLowestNearbyHeights.
	 * Z runs from BedrockLayers to min(Height − GrassDepthMax, lowest nearby surface − SurfaceMargin).
	 */
	void CarveCaves(FIntPoint ChunkCoord, const FVector& NoiseOffset, const FVoxelCaveSettings& Caves, int32 BedrockLayers,
		int32 GrassDepthMax, const TArray<int32>& LowestNearby, FVoxelChunkData& Data);

	/** Every step for one chunk, alone: columns (with padding), fill, caves. Same inputs = same bytes, in any order. */
	FVoxelChunkData GenerateChunk(FIntPoint ChunkCoord, const FVoxelGenerationParams& Params);
}
