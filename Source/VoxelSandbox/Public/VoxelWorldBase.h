// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
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

private:
	/** Floor division by ChunkSize, correct for negative values. */
	static int32 FloorDivChunk(int32 Value);

	/** Every registered chunk by chunk coordinate. */
	UPROPERTY(Transient)
	TMap<FIntPoint, TObjectPtr<AVoxelChunkBase>> Chunks;
};
