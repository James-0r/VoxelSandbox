// Fill out your copyright notice in the Description page of Project Settings.

#include "VoxelWorldBase.h"

#include "VoxelChunkBase.h"
#include "VoxelTypes.h"

AVoxelWorldBase::AVoxelWorldBase()
{
	// The world reacts to spawning and player actions; it has nothing to poll every frame.
	PrimaryActorTick.bCanEverTick = false;
}

void AVoxelWorldBase::RegisterChunk(AVoxelChunkBase* Chunk)
{
	// The chunk must already know its ChunkCoord (InitChunkData) so it lands under the right key.
	if (!ensureMsgf(Chunk != nullptr, TEXT("RegisterChunk: Chunk is None.")))
	{
		return;
	}

	Chunks.Add(Chunk->GetChunkCoord(), Chunk);
	Chunk->SetVoxelWorld(this);
}

AVoxelChunkBase* AVoxelWorldBase::FindChunk(const FIntPoint Coord) const
{
	// Map lookup; None when that chunk hasn't been spawned.
	const TObjectPtr<AVoxelChunkBase>* Found = Chunks.Find(Coord);
	return Found ? Found->Get() : nullptr;
}

uint8 AVoxelWorldBase::GetCubeByte(const FIntVector Cell) const
{
	// Below Z 0 is solid bedrock; unloaded space is Air so border faces still draw.
	if (Cell.Z < 0)
	{
		return Voxel::CubeType::Rock;
	}

	const AVoxelChunkBase* Chunk = FindChunk(CellToChunkCoord(Cell));
	if (!Chunk)
	{
		return Voxel::CubeType::Air;
	}

	const FIntVector Local = CellToLocal(Cell);
	return Chunk->GetLocalCubeByte(Local.X, Local.Y, Local.Z);
}

FIntPoint AVoxelWorldBase::CellToChunkCoord(const FIntVector Cell)
{
	// Floor division keeps negative cells in the chunk to their left/below.
	return FIntPoint(FloorDivChunk(Cell.X), FloorDivChunk(Cell.Y));
}

FIntVector AVoxelWorldBase::CellToLocal(const FIntVector Cell)
{
	// Remainder that is always 0..ChunkSize−1, even for negative cells.
	return FIntVector(Cell.X - FloorDivChunk(Cell.X) * Voxel::ChunkSize,
		Cell.Y - FloorDivChunk(Cell.Y) * Voxel::ChunkSize,
		Cell.Z);
}

int32 AVoxelWorldBase::FloorDivChunk(const int32 Value)
{
	// C++ '/' rounds toward zero, so -1 / 16 would be 0; this rounds down to -1 instead.
	return Value >= 0 ? Value / Voxel::ChunkSize : -((-Value + Voxel::ChunkSize - 1) / Voxel::ChunkSize);
}
