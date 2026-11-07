// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoxelCaveSettings.h"
#include "VoxelChunkBase.generated.h"

class AVoxelWorldBase;

/**
 * C++ base for BP_VoxelChunk: owns the cube data and runs the per-cell loops
 * (fill, caves, meshing). Per-column and per-chunk logic stays in Blueprint.
 */
UCLASS(Abstract)
class VOXELSANDBOX_API AVoxelChunkBase : public AActor
{
	GENERATED_BODY()

public:
	AVoxelChunkBase();

	/** Sizes the cube and column arrays for this chunk and fills them with Air. Replaces InitCubes. */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Chunk")
	void InitChunkData(FIntPoint InChunkCoord, int32 InWorldHeight);

	/** Stores one column's surface height, grass depth and snow line, computed in Blueprint GenerateTerrain. */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Chunk")
	void SetColumn(int32 X, int32 Y, int32 Height, int32 GrassDepth, int32 SnowLine);

	/** Fills every column from Z 0 to its Height using the GetTypeForCell rule (Rock / Grass / Snow). */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Chunk")
	void FillColumns();

	/**
	 * Carves cheese caverns (one noise above CaveCurve) and round tunnels (two noises both near zero, with a
	 * width that varies along the tunnel). Z runs from BedrockLayers to (Height − GrassDepthMax). Deterministic per seed.
	 */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Chunk")
	void CarveCaves(FVector NoiseOffset, const FVoxelCaveSettings& Caves, int32 BedrockLayers, int32 GrassDepthMax);

	/**
	 * Builds the mesh arrays for one cube type with face culling. Outputs plug straight into Create Mesh Section.
	 * Neighbours outside the chunk in X/Y are read through the world; Z < 0 is solid, Z >= WorldHeight is air.
	 * BlueprintPure = false: a const function would otherwise become a pure node and rebuild once per output pin.
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure = false, Category = "Voxel|Chunk")
	void BuildSectionData(uint8 CubeType, double CubeSize, TArray<FVector>& Vertices, TArray<int32>& Triangles, TArray<FVector>& Normals, TArray<FVector2D>& UVs) const;

	/** Returns the cube byte at a local cell, or Air when the cell is outside this chunk. */
	UFUNCTION(BlueprintPure, Category = "Voxel|Chunk")
	uint8 GetLocalCubeByte(int32 X, int32 Y, int32 Z) const;

	/** Writes a cube byte at a local cell; raises the column Height if a solid cube goes above it. False when out of range. */
	UFUNCTION(BlueprintCallable, Category = "Voxel|Chunk")
	bool SetLocalCubeByte(int32 X, int32 Y, int32 Z, uint8 Type);

	/** Returns the top filled Z of a column, or INDEX_NONE (−1) when X/Y are outside the chunk. */
	UFUNCTION(BlueprintPure, Category = "Voxel|Chunk")
	int32 GetColumnHeight(int32 X, int32 Y) const;

	/** Returns this chunk's coordinate in the world's chunk grid. */
	UFUNCTION(BlueprintPure, Category = "Voxel|Chunk")
	FIntPoint GetChunkCoord() const { return ChunkCoord; }

	/** Called by AVoxelWorldBase::RegisterChunk so neighbour lookups can cross chunk borders. */
	void SetVoxelWorld(AVoxelWorldBase* InWorld) { VoxelWorld = InWorld; }

private:
	/** Flat index of a local cell: X + Y*ChunkSize + Z*ChunkSize². Caller checks the range. */
	static int32 CubeIndex(int32 X, int32 Y, int32 Z);

	/** Flat index of a column: X + Y*ChunkSize. Caller checks the range. */
	static int32 ColumnIndex(int32 X, int32 Y);

	/** True when X and Y are inside the chunk (0..ChunkSize−1). */
	static bool IsInsideColumns(int32 X, int32 Y);

	/** True when the cell is inside the chunk in X, Y and Z. */
	bool IsInsideChunk(int32 X, int32 Y, int32 Z) const;

	/** GetTypeForCell rule: top TopDepth cubes are Snow if the surface reaches SnowLine, else Grass; below is Rock. */
	static uint8 GetTypeForCell(int32 Z, int32 SurfaceZ, int32 TopDepth, int32 SnowLine);

	/** Neighbour test for face culling: Z < 0 solid, Z >= WorldHeight air, outside X/Y via the world (Air if none). */
	bool IsAirAt(int32 X, int32 Y, int32 Z) const;

	/**
	 * For each of this chunk's columns, the lowest column height within a horizontal circle of Radius columns,
	 * including neighbour chunks through VoxelWorld. Missing chunks are ignored. Used for the cave surface margin.
	 */
	void ComputeLowestNearbyHeights(int32 Radius, TArray<int32>& OutLowest) const;

	/** The world that registered this chunk; used for cross-chunk neighbour lookups. */
	UPROPERTY(Transient)
	TObjectPtr<AVoxelWorldBase> VoxelWorld;

	// Plain C++ data (not UPROPERTY) so names can't clash with the Blueprint's own variables.

	/** Cube bytes, Index = X + Y*ChunkSize + Z*ChunkSize². */
	TArray<uint8> Cubes;

	/** Top filled Z per column, Index = X + Y*ChunkSize. */
	TArray<int32> Heights;

	/** Surface layer depth per column. */
	TArray<int32> GrassDepths;

	/** Snow line per column. */
	TArray<int32> SnowLines;

	/** This chunk's coordinate in the chunk grid. */
	FIntPoint ChunkCoord = FIntPoint::ZeroValue;

	/** Number of cube layers in Z. */
	int32 WorldHeight = 0;
};
