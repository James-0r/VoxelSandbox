// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoxelCaveSettings.h"
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
	 * Carves winding tunnels from the surface down until each one breaks into an existing cave, reaches bedrock,
	 * leaves the loaded chunks or runs out of steps. Same Seed = same worms. Only writes cube bytes: no mesh
	 * rebuild, no save record. Call after the caves pass and before ApplySavedChanges and meshing.
	 * Outputs how many worms were carved and how many connected to a cave; also logs both and the time taken.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|World")
	void CarveWorms(int32 Seed, const FVoxelWormSettings& Worms, int32 BedrockLayers, int32& WormsCarved, int32& WormsConnected);

	/**
	 * Writes a cube byte at a world cell through its chunk's SetLocalCubeByte. Doesn't rebuild any mesh or record
	 * a save change (that's SetCube's job in Blueprint). False when the chunk doesn't exist or Z is out of range.
	 */
	bool SetCubeByte(FIntVector Cell, uint8 Type);

private:
	/** Floor division by ChunkSize, correct for negative values. */
	static int32 FloorDivChunk(int32 Value);

	/** Every registered chunk by chunk coordinate. */
	UPROPERTY(Transient)
	TMap<FIntPoint, TObjectPtr<AVoxelChunkBase>> Chunks;
};
