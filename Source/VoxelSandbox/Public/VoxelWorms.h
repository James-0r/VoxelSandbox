// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "VoxelCaveSettings.h"
#include "VoxelGeneration.h"

/** One step of a worm tunnel: everything within Radius of Center (in cube units) becomes Air. */
struct FVoxelWormSphere
{
	FVector Center = FVector::ZeroVector;
	double Radius = 0.0;
};

/** A planned worm: its spheres in order, the box around all of them (cube units) and how it ended. */
struct FVoxelWormPath
{
	TArray<FVoxelWormSphere> Spheres;
	FBox Bounds = FBox(ForceInit);
	bool bConnected = false;
};

/** Everything a region's worms depend on. Same inputs = same worms, whatever is loaded. */
struct FVoxelWormPlanParams
{
	FVoxelGenerationParams Generation;
	FVoxelWormSettings Worms;
	int32 Seed = 0;
};

/**
 * Worm tunnels for a streamed world, as pure functions. The world is split into square regions; each region's
 * worms are planned from (Seed, region) alone, using the noise (not loaded chunks) for start heights and for the
 * "reached a cave" test. A plan is just a list of spheres, so any chunk can carve the part of a worm that falls
 * inside it, and a worm crossing a border is cut by both chunks from the same spheres.
 */
namespace VoxelWorms
{
	/** Side of a planning region, in cubes. */
	int32 GetRegionSizeCubes(const FVoxelWormSettings& Worms);

	/** Region that contains a world cell column (floor division, correct for negatives). */
	FIntPoint CellToRegion(int32 CellX, int32 CellY, const FVoxelWormSettings& Worms);

	/** Furthest a worm's carving can be from its start column, in cubes (steps, the push into a cave, the radius). */
	int32 GetMaxReachCubes(const FVoxelWormSettings& Worms);

	/** Plans every worm that starts in Region. Logs the count and time at Verbose. */
	TArray<FVoxelWormPath> PlanRegion(FIntPoint Region, const FVoxelWormPlanParams& Params);

	/** Sets every cell of the chunk within any sphere of Paths to Air, above bedrock. Returns how many cells changed. */
	int32 CarveIntoChunk(TConstArrayView<const FVoxelWormPath*> Paths, FIntPoint ChunkCoord, int32 BedrockLayers, FVoxelChunkData& Data);
}
