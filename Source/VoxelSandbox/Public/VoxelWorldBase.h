// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoxelCaveSettings.h"
#include "VoxelTerrainSettings.h"
#include "VoxelWorms.h"
#include "VoxelWorldBase.generated.h"

class AVoxelChunkBase;

/**
 * C++ base for BP_VoxelWorld: keeps the chunk map and answers cube lookups by world cell,
 * so chunks can cull faces across their borders.
 */
UCLASS(Abstract)
class VOXELSANDBOX_API AVoxelWorldBase : public AActor
{
	GENERATED_BODY()

public:
	AVoxelWorldBase();

	/** Adds a chunk under its ChunkCoord and points the chunk back at this world. */
	UFUNCTION(BlueprintCallable, Category = "Voxel|World")
	void RegisterChunk(AVoxelChunkBase* Chunk);

	/** Returns the chunk at a chunk coordinate, or None if there is none. */
	UFUNCTION(BlueprintPure, Category = "Voxel|World")
	AVoxelChunkBase* FindChunk(FIntPoint Coord) const;

	/** Returns the cube byte at a world cell: Rock below Z 0, Air where no chunk exists. */
	UFUNCTION(BlueprintPure, Category = "Voxel|World")
	uint8 GetCubeByte(FIntVector Cell) const;

	/** Chunk coordinate that contains a world cell (floor division, so cell X −1 is chunk −1). */
	UFUNCTION(BlueprintPure, Category = "Voxel|World")
	static FIntPoint CellToChunkCoord(FIntVector Cell);

	/** Local cell inside its chunk (X/Y in 0..ChunkSize−1, Z unchanged), so cell X −1 is local 15. */
	UFUNCTION(BlueprintPure, Category = "Voxel|World")
	static FIntVector CellToLocal(FIntVector Cell);

	/**
	 * Stores what generation depends on (seed, settings, noise offset) for the worm planner, and forgets every
	 * planned worm region. Call once after InitSeed, before any chunk loads.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|World|Worms")
	void SetGenerationParams(int32 Seed, const FVoxelTerrainSettings& Terrain, const FVoxelCaveSettings& Caves,
		const FVoxelWormSettings& Worms, FVector NoiseOffset, int32 WorldHeight, int32 BedrockLayers, bool bEnableCaves);

	/**
	 * Carves the parts of every worm that pass through this chunk. Plans any region it needs that isn't planned
	 * yet (normally PlanWormRegionsNear has done that already). Call after the caves pass and before saved edits.
	 * Returns how many cells became Air.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|World|Worms")
	int32 CarveWormsInChunk(AVoxelChunkBase* Chunk);

	/**
	 * Plans up to MaxRegions worm regions that chunks within LoadRadiusChunks of Center will need, nearest first,
	 * a region ahead of time so the cost is spread over frames instead of landing on the frame a chunk loads.
	 * 0 = no limit. Returns how many regions were planned this call.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|World|Worms")
	int32 PlanWormRegionsNear(FIntPoint Center, int32 LoadRadiusChunks, int32 MaxRegions);

	/** Forgets planned worm regions no chunk within UnloadRadiusChunks of Center can need. Returns how many. */
	UFUNCTION(BlueprintCallable, Category = "Voxel|World|Worms")
	int32 EvictWormRegionsFar(FIntPoint Center, int32 UnloadRadiusChunks);

	/**
	 * Chunk coordinates within RadiusChunks of Center (a circle) that have no chunk yet, nearest first.
	 * At most MaxCount entries (0 = all). The streaming manager spawns these.
	 * BlueprintPure = false: as a pure node, a For Each Loop would re-run it every iteration while chunks are
	 * being added, and the list would shift under the loop.
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure = false, Category = "Voxel|World|Streaming")
	TArray<FIntPoint> GetMissingChunksNear(FIntPoint Center, int32 RadiusChunks, int32 MaxCount) const;

	/**
	 * Loaded chunks within RadiusChunks of Center that have no mesh yet and whose four side neighbours are all
	 * loaded, nearest first. At most MaxCount entries (0 = all). Meshing only these means border faces are
	 * always culled against real neighbour data, so no chunk ever needs a second build because a neighbour arrived.
	 * BlueprintPure = false for the same reason as GetMissingChunksNear.
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure = false, Category = "Voxel|World|Streaming")
	TArray<AVoxelChunkBase*> GetChunksReadyToMesh(FIntPoint Center, int32 RadiusChunks, int32 MaxCount) const;

	/**
	 * Loaded chunks that still have a mesh but lie further than RadiusChunks from Center, farthest first.
	 * At most MaxCount entries (0 = all). Clearing these keeps meshes only inside the view, so a mesh always has
	 * all four neighbours loaded and the outer edge of the drawn world is always closed.
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure = false, Category = "Voxel|World|Streaming")
	TArray<AVoxelChunkBase*> GetMeshedChunksOutside(FIntPoint Center, int32 RadiusChunks, int32 MaxCount) const;

	/**
	 * Loaded chunks further than RadiusChunks from Center, farthest first. At most MaxCount entries (0 = all).
	 * The streaming manager unregisters and destroys these.
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure = false, Category = "Voxel|World|Streaming")
	TArray<AVoxelChunkBase*> GetChunksToUnload(FIntPoint Center, int32 RadiusChunks, int32 MaxCount) const;

	/** Removes a chunk from the chunk map, so lookups treat its cells as unloaded. Call before destroying it. */
	UFUNCTION(BlueprintCallable, Category = "Voxel|World|Streaming")
	void UnregisterChunk(AVoxelChunkBase* Chunk);

	/**
	 * The cells of Cells that lie inside chunk Coord. Used to apply only a chunk's own saved edits when it loads.
	 * Not pure, so a For Each Loop over the result filters every saved edit once, not once per iteration.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|World|Streaming")
	static TArray<FIntVector> FilterCellsInChunk(const TArray<FIntVector>& Cells, FIntPoint Coord);

	/**
	 * Debug test for "each chunk can generate alone": regenerates every registered chunk with noise only
	 * (VoxelGeneration::GenerateChunk, no neighbour lookups) and compares it with what the chunk holds now.
	 * Call after the caves pass and before worms and saved changes. Logs a summary and the average ms per chunk.
	 * ColumnMismatchChunks should be 0. InteriorMismatchChunks counts chunks whose neighbours within the caves'
	 * reach all exist and should be 0; rim chunks may differ, because the old caves ignored missing neighbours.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|World|Debug")
	void VerifyStandaloneGeneration(const FVoxelTerrainSettings& Terrain, const FVoxelCaveSettings& Caves, FVector NoiseOffset,
		int32 BedrockLayers, bool bEnableCaves, int32& ColumnMismatchChunks, int32& InteriorMismatchChunks, int32& RimMismatchChunks);

private:
	/** Floor division by ChunkSize, correct for negative values. */
	static int32 FloorDivChunk(int32 Value);

	/** A region's worms, planning it first if needed. */
	const TArray<FVoxelWormPath>& FindOrPlanWormRegion(FIntPoint Region);

	/** Worm regions whose worms can reach a chunk within RadiusChunks of Center, nearest first. */
	TArray<FIntPoint> GetWormRegionsNeededNear(FIntPoint Center, int32 RadiusChunks) const;

	/** How far a worm can reach from its region, in whole chunks. */
	int32 GetWormReachChunks() const;

	/** Every registered chunk by chunk coordinate. */
	UPROPERTY(Transient)
	TMap<FIntPoint, TObjectPtr<AVoxelChunkBase>> Chunks;

	// Plain C++ (not UPROPERTY): the worm cache is rebuilt from the seed, never saved or shown in the editor.

	/** Inputs to worm planning, from SetGenerationParams. */
	FVoxelWormPlanParams WormParams;

	/** True once SetGenerationParams has run. */
	bool bHasGenerationParams = false;

	/** Planned worms by region coordinate. */
	TMap<FIntPoint, TArray<FVoxelWormPath>> WormRegions;
};
