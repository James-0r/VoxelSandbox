// Fill out your copyright notice in the Description page of Project Settings.

#include "VoxelWorldBase.h"

#include "HAL/PlatformTime.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "VoxelChunkBase.h"
#include "VoxelGeneration.h"
#include "VoxelLog.h"
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

namespace
{
	/**
	 * Every chunk coordinate within Radius of Center (a circle), nearest first. Ties are broken by X then Y so
	 * the order never depends on anything but the inputs.
	 */
	TArray<FIntPoint> GetCoordsNearestFirst(const FIntPoint Center, const int32 Radius)
	{
		const int32 SafeRadius = FMath::Max(Radius, 0);
		const int32 RadiusSquared = SafeRadius * SafeRadius;

		TArray<FIntPoint> Offsets;
		for (int32 OffsetY = -SafeRadius; OffsetY <= SafeRadius; ++OffsetY)
		{
			for (int32 OffsetX = -SafeRadius; OffsetX <= SafeRadius; ++OffsetX)
			{
				if (OffsetX * OffsetX + OffsetY * OffsetY <= RadiusSquared)
				{
					Offsets.Add(FIntPoint(OffsetX, OffsetY));
				}
			}
		}

		Offsets.Sort([](const FIntPoint& A, const FIntPoint& B)
		{
			const int32 DistanceA = A.X * A.X + A.Y * A.Y;
			const int32 DistanceB = B.X * B.X + B.Y * B.Y;
			if (DistanceA != DistanceB)
			{
				return DistanceA < DistanceB;
			}
			return A.X != B.X ? A.X < B.X : A.Y < B.Y;
		});

		for (FIntPoint& Offset : Offsets)
		{
			Offset += Center;
		}
		return Offsets;
	}

	/** True when Count has reached a MaxCount limit (0 or less = no limit). */
	bool IsAtLimit(const int32 Count, const int32 MaxCount)
	{
		return MaxCount > 0 && Count >= MaxCount;
	}
}

TArray<FIntPoint> AVoxelWorldBase::GetMissingChunksNear(const FIntPoint Center, const int32 RadiusChunks, const int32 MaxCount) const
{
	// Walk the circle from the centre outwards and keep the holes, so the closest ground always loads first.
	TArray<FIntPoint> Missing;
	for (const FIntPoint& Coord : GetCoordsNearestFirst(Center, RadiusChunks))
	{
		if (IsAtLimit(Missing.Num(), MaxCount))
		{
			break;
		}
		if (!Chunks.Contains(Coord))
		{
			Missing.Add(Coord);
		}
	}
	return Missing;
}

TArray<AVoxelChunkBase*> AVoxelWorldBase::GetChunksReadyToMesh(const FIntPoint Center, const int32 RadiusChunks, const int32 MaxCount) const
{
	// A chunk's mesh culls its border faces against the four side neighbours, so wait until they all exist.
	static const FIntPoint SideOffsets[] = { FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1) };

	TArray<AVoxelChunkBase*> Ready;
	for (const FIntPoint& Coord : GetCoordsNearestFirst(Center, RadiusChunks))
	{
		if (IsAtLimit(Ready.Num(), MaxCount))
		{
			break;
		}

		AVoxelChunkBase* Chunk = FindChunk(Coord);
		if (!Chunk || Chunk->HasMesh())
		{
			continue;
		}

		bool bNeighboursLoaded = true;
		for (const FIntPoint& Offset : SideOffsets)
		{
			bNeighboursLoaded &= Chunks.Contains(Coord + Offset);
		}
		if (bNeighboursLoaded)
		{
			Ready.Add(Chunk);
		}
	}
	return Ready;
}

namespace
{
	/** Squared distance in chunks between two chunk coordinates (no square root needed to compare). */
	int32 ChunkDistanceSquared(const FIntPoint A, const FIntPoint B)
	{
		const FIntPoint Delta = A - B;
		return Delta.X * Delta.X + Delta.Y * Delta.Y;
	}

	/**
	 * Chunks of the map further than Radius from Center that pass Filter, farthest first, at most MaxCount.
	 * Farthest first so the chunks that matter least go first when the budget cuts the list short.
	 */
	template <typename FilterType>
	TArray<AVoxelChunkBase*> GetChunksOutsideFarthestFirst(const TMap<FIntPoint, TObjectPtr<AVoxelChunkBase>>& Chunks,
		const FIntPoint Center, const int32 Radius, const int32 MaxCount, FilterType Filter)
	{
		const int32 SafeRadius = FMath::Max(Radius, 0);
		const int32 RadiusSquared = SafeRadius * SafeRadius;

		TArray<TPair<int32, AVoxelChunkBase*>> Found;
		for (const TPair<FIntPoint, TObjectPtr<AVoxelChunkBase>>& Entry : Chunks)
		{
			const int32 DistanceSquared = ChunkDistanceSquared(Entry.Key, Center);
			if (DistanceSquared > RadiusSquared && Entry.Value && Filter(*Entry.Value))
			{
				Found.Emplace(DistanceSquared, Entry.Value.Get());
			}
		}

		// Ties broken by coordinate so the order never depends on the map's internal order.
		Found.Sort([](const TPair<int32, AVoxelChunkBase*>& A, const TPair<int32, AVoxelChunkBase*>& B)
		{
			if (A.Key != B.Key)
			{
				return A.Key > B.Key;
			}
			const FIntPoint CoordA = A.Value->GetChunkCoord();
			const FIntPoint CoordB = B.Value->GetChunkCoord();
			return CoordA.X != CoordB.X ? CoordA.X < CoordB.X : CoordA.Y < CoordB.Y;
		});

		TArray<AVoxelChunkBase*> Result;
		for (const TPair<int32, AVoxelChunkBase*>& Entry : Found)
		{
			if (IsAtLimit(Result.Num(), MaxCount))
			{
				break;
			}
			Result.Add(Entry.Value);
		}
		return Result;
	}
}

TArray<AVoxelChunkBase*> AVoxelWorldBase::GetMeshedChunksOutside(const FIntPoint Center, const int32 RadiusChunks, const int32 MaxCount) const
{
	// Only chunks that still have something drawn; data-only chunks are already in the right state.
	return GetChunksOutsideFarthestFirst(Chunks, Center, RadiusChunks, MaxCount,
		[](const AVoxelChunkBase& Chunk) { return Chunk.HasMesh(); });
}

TArray<AVoxelChunkBase*> AVoxelWorldBase::GetChunksToUnload(const FIntPoint Center, const int32 RadiusChunks, const int32 MaxCount) const
{
	// Every chunk past the unload radius goes, meshed or not.
	return GetChunksOutsideFarthestFirst(Chunks, Center, RadiusChunks, MaxCount,
		[](const AVoxelChunkBase&) { return true; });
}

void AVoxelWorldBase::UnregisterChunk(AVoxelChunkBase* Chunk)
{
	// Removes by the chunk's own coordinate, and only if the map still points at this chunk.
	if (!ensureMsgf(Chunk != nullptr, TEXT("UnregisterChunk: Chunk is None.")))
	{
		return;
	}

	const FIntPoint Coord = Chunk->GetChunkCoord();
	if (const TObjectPtr<AVoxelChunkBase>* Found = Chunks.Find(Coord); Found && *Found == Chunk)
	{
		Chunks.Remove(Coord);
	}
	Chunk->SetVoxelWorld(nullptr);
}

TArray<FIntVector> AVoxelWorldBase::FilterCellsInChunk(const TArray<FIntVector>& Cells, const FIntPoint Coord)
{
	// One pass over every saved edit; only this chunk's ones are returned, so Blueprint loops over a few cells.
	TArray<FIntVector> Inside;
	for (const FIntVector& Cell : Cells)
	{
		if (CellToChunkCoord(Cell) == Coord)
		{
			Inside.Add(Cell);
		}
	}
	return Inside;
}

void AVoxelWorldBase::VerifyStandaloneGeneration(const FVoxelTerrainSettings& Terrain, const FVoxelCaveSettings& Caves,
	const FVector NoiseOffset, const int32 BedrockLayers, const bool bEnableCaves, int32& ColumnMismatchChunks,
	int32& InteriorMismatchChunks, int32& RimMismatchChunks)
{
	// For each chunk: build it again from nothing but the noise and compare byte by byte. Columns never depend
	// on neighbours, so they must match everywhere; cubes must match wherever the old caves saw every neighbour.
	ColumnMismatchChunks = 0;
	InteriorMismatchChunks = 0;
	RimMismatchChunks = 0;

	TArray<FIntPoint> ChunkCoords;
	Chunks.GenerateKeyArray(ChunkCoords);
	ChunkCoords.Sort([](const FIntPoint& A, const FIntPoint& B) { return A.X != B.X ? A.X < B.X : A.Y < B.Y; });

	// A chunk is "interior" when every chunk the cave border can reach exists, so the old path saw no gaps.
	const int32 ReachChunks = FMath::DivideAndRoundUp(VoxelGeneration::GetColumnPadding(Caves), Voxel::ChunkSize);
	int32 InteriorChunks = 0;
	double GenerateSeconds = 0.0;

	for (const FIntPoint& Coord : ChunkCoords)
	{
		const AVoxelChunkBase* Chunk = FindChunk(Coord);
		const FVoxelChunkData& Existing = Chunk->GetChunkData();

		FVoxelGenerationParams Params;
		Params.Terrain = Terrain;
		Params.Caves = Caves;
		Params.NoiseOffset = NoiseOffset;
		Params.WorldHeight = Existing.WorldHeight;
		Params.BedrockLayers = BedrockLayers;
		Params.bEnableCaves = bEnableCaves;

		const double StartSeconds = FPlatformTime::Seconds();
		const FVoxelChunkData Generated = VoxelGeneration::GenerateChunk(Coord, Params);
		GenerateSeconds += FPlatformTime::Seconds() - StartSeconds;

		bool bInterior = true;
		for (int32 OffsetY = -ReachChunks; OffsetY <= ReachChunks && bInterior; ++OffsetY)
		{
			for (int32 OffsetX = -ReachChunks; OffsetX <= ReachChunks && bInterior; ++OffsetX)
			{
				bInterior = FindChunk(Coord + FIntPoint(OffsetX, OffsetY)) != nullptr;
			}
		}
		InteriorChunks += bInterior ? 1 : 0;

		const bool bColumnsMatch = Generated.Heights == Existing.Heights
			&& Generated.GrassDepths == Existing.GrassDepths
			&& Generated.SnowLines == Existing.SnowLines;

		int32 CubeMismatches = 0;
		if (Generated.Cubes.Num() == Existing.Cubes.Num())
		{
			for (int32 Index = 0; Index < Generated.Cubes.Num(); ++Index)
			{
				CubeMismatches += Generated.Cubes[Index] != Existing.Cubes[Index] ? 1 : 0;
			}
		}
		else
		{
			CubeMismatches = FMath::Max(Generated.Cubes.Num(), Existing.Cubes.Num());
		}

		if (!bColumnsMatch)
		{
			++ColumnMismatchChunks;
			UE_LOG(LogVoxel, Warning, TEXT("VerifyStandaloneGeneration: chunk (%d, %d) columns differ."), Coord.X, Coord.Y);
		}
		if (CubeMismatches > 0)
		{
			(bInterior ? InteriorMismatchChunks : RimMismatchChunks) += 1;
			UE_LOG(LogVoxel, Log, TEXT("VerifyStandaloneGeneration: chunk (%d, %d) [%s] %d cubes differ."),
				Coord.X, Coord.Y, bInterior ? TEXT("interior") : TEXT("rim"), CubeMismatches);
		}
	}

	const int32 ChunkCount = ChunkCoords.Num();
	UE_LOG(LogVoxel, Log,
		TEXT("VerifyStandaloneGeneration: %d chunks. Columns differ in %d. Cubes differ in %d of %d interior and %d of %d rim chunks. GenerateChunk avg %.2f ms."),
		ChunkCount, ColumnMismatchChunks, InteriorMismatchChunks, InteriorChunks, RimMismatchChunks, ChunkCount - InteriorChunks,
		ChunkCount > 0 ? GenerateSeconds * 1000.0 / ChunkCount : 0.0);
}

namespace
{
	/** Floor division, so chunk −1 is in region −1, not region 0. */
	int32 FloorDivRegion(const int32 Value, const int32 Divisor)
	{
		return Value >= 0 ? Value / Divisor : -((-Value + Divisor - 1) / Divisor);
	}

	/** Squared distance in chunks from a chunk to the nearest chunk of a region (0 when inside it). */
	int64 RegionDistanceSquared(const FIntPoint Center, const FIntPoint Region, const int32 RegionSizeChunks)
	{
		const int32 MinX = Region.X * RegionSizeChunks;
		const int32 MinY = Region.Y * RegionSizeChunks;
		const int64 DeltaX = FMath::Max3(MinX - Center.X, 0, Center.X - (MinX + RegionSizeChunks - 1));
		const int64 DeltaY = FMath::Max3(MinY - Center.Y, 0, Center.Y - (MinY + RegionSizeChunks - 1));
		return DeltaX * DeltaX + DeltaY * DeltaY;
	}
}

void AVoxelWorldBase::SetGenerationParams(const int32 Seed, const FVoxelTerrainSettings& Terrain, const FVoxelCaveSettings& Caves,
	const FVoxelWormSettings& Worms, const FVector NoiseOffset, const int32 WorldHeight, const int32 BedrockLayers, const bool bEnableCaves)
{
	// A copy of everything the planner reads, so planning never touches the data asset or an actor.
	WormParams = FVoxelWormPlanParams();
	WormParams.Seed = Seed;
	WormParams.Worms = Worms;
	WormParams.Generation.Terrain = Terrain;
	WormParams.Generation.Caves = Caves;
	WormParams.Generation.NoiseOffset = NoiseOffset;
	WormParams.Generation.WorldHeight = WorldHeight;
	WormParams.Generation.BedrockLayers = BedrockLayers;
	WormParams.Generation.bEnableCaves = bEnableCaves;
	bHasGenerationParams = true;

	// Plans made with other inputs would carve the wrong tunnels.
	WormRegions.Reset();
}

int32 AVoxelWorldBase::GetWormReachChunks() const
{
	// Whole chunks a worm can reach from its region, plus one so a sphere spilling into the next chunk counts.
	return FMath::DivideAndRoundUp(VoxelWorms::GetMaxReachCubes(WormParams.Worms), Voxel::ChunkSize) + 1;
}

TArray<FIntPoint> AVoxelWorldBase::GetWormRegionsNeededNear(const FIntPoint Center, const int32 RadiusChunks) const
{
	// A region matters if any chunk within RadiusChunks could be reached by its worms.
	const int32 RegionSize = FMath::Max(WormParams.Worms.RegionSizeChunks, 1);
	const int32 Reach = FMath::Max(RadiusChunks, 0) + GetWormReachChunks();
	const int64 ReachSquared = static_cast<int64>(Reach) * Reach;

	TArray<TPair<int64, FIntPoint>> Found;
	for (int32 RegionY = FloorDivRegion(Center.Y - Reach, RegionSize); RegionY <= FloorDivRegion(Center.Y + Reach, RegionSize); ++RegionY)
	{
		for (int32 RegionX = FloorDivRegion(Center.X - Reach, RegionSize); RegionX <= FloorDivRegion(Center.X + Reach, RegionSize); ++RegionX)
		{
			const FIntPoint Region(RegionX, RegionY);
			const int64 DistanceSquared = RegionDistanceSquared(Center, Region, RegionSize);
			if (DistanceSquared <= ReachSquared)
			{
				Found.Emplace(DistanceSquared, Region);
			}
		}
	}

	// Nearest first; ties by coordinate so the order only depends on the inputs.
	Found.Sort([](const TPair<int64, FIntPoint>& A, const TPair<int64, FIntPoint>& B)
	{
		if (A.Key != B.Key)
		{
			return A.Key < B.Key;
		}
		return A.Value.X != B.Value.X ? A.Value.X < B.Value.X : A.Value.Y < B.Value.Y;
	});

	TArray<FIntPoint> Regions;
	Regions.Reserve(Found.Num());
	for (const TPair<int64, FIntPoint>& Entry : Found)
	{
		Regions.Add(Entry.Value);
	}
	return Regions;
}

const TArray<FVoxelWormPath>& AVoxelWorldBase::FindOrPlanWormRegion(const FIntPoint Region)
{
	if (const TArray<FVoxelWormPath>* Found = WormRegions.Find(Region))
	{
		return *Found;
	}
	return WormRegions.Add(Region, VoxelWorms::PlanRegion(Region, WormParams));
}

int32 AVoxelWorldBase::CarveWormsInChunk(AVoxelChunkBase* Chunk)
{
	// Plan first, then collect: adding regions to the map while holding pointers into it would be unsafe.
	TRACE_CPUPROFILER_EVENT_SCOPE(AVoxelWorldBase::CarveWormsInChunk);

	if (!ensureMsgf(Chunk != nullptr, TEXT("CarveWormsInChunk: Chunk is None."))
		|| !ensureMsgf(bHasGenerationParams, TEXT("CarveWormsInChunk: call SetGenerationParams first."))
		|| !WormParams.Worms.bEnabled)
	{
		return 0;
	}

	const TArray<FIntPoint> Regions = GetWormRegionsNeededNear(Chunk->GetChunkCoord(), 0);
	for (const FIntPoint& Region : Regions)
	{
		FindOrPlanWormRegion(Region);
	}

	TArray<const FVoxelWormPath*> Paths;
	for (const FIntPoint& Region : Regions)
	{
		for (const FVoxelWormPath& Path : WormRegions.FindChecked(Region))
		{
			Paths.Add(&Path);
		}
	}

	return VoxelWorms::CarveIntoChunk(Paths, Chunk->GetChunkCoord(), WormParams.Generation.BedrockLayers, Chunk->GetMutableChunkData());
}

int32 AVoxelWorldBase::PlanWormRegionsNear(const FIntPoint Center, const int32 LoadRadiusChunks, const int32 MaxRegions)
{
	// One region further out than the chunks need, so a region is ready long before the first chunk asks for it.
	if (!bHasGenerationParams || !WormParams.Worms.bEnabled)
	{
		return 0;
	}

	const double StartSeconds = FPlatformTime::Seconds();
	const int32 AheadRadius = FMath::Max(LoadRadiusChunks, 0) + FMath::Max(WormParams.Worms.RegionSizeChunks, 1);

	int32 Planned = 0;
	for (const FIntPoint& Region : GetWormRegionsNeededNear(Center, AheadRadius))
	{
		if (IsAtLimit(Planned, MaxRegions))
		{
			break;
		}
		if (!WormRegions.Contains(Region))
		{
			WormRegions.Add(Region, VoxelWorms::PlanRegion(Region, WormParams));
			++Planned;
		}
	}

	if (Planned > 0)
	{
		UE_LOG(LogVoxel, Verbose, TEXT("PlanWormRegionsNear: planned %d regions in %.2f ms (%d cached)."),
			Planned, (FPlatformTime::Seconds() - StartSeconds) * 1000.0, WormRegions.Num());
	}
	return Planned;
}

int32 AVoxelWorldBase::EvictWormRegionsFar(const FIntPoint Center, const int32 UnloadRadiusChunks)
{
	// Keep a margin of two regions beyond what PlanWormRegionsNear plans, so a region isn't dropped and replanned
	// when the player steps back and forth.
	const int32 RegionSize = FMath::Max(WormParams.Worms.RegionSizeChunks, 1);
	const int32 Keep = FMath::Max(UnloadRadiusChunks, 0) + GetWormReachChunks() + 2 * RegionSize;
	const int64 KeepSquared = static_cast<int64>(Keep) * Keep;

	int32 Evicted = 0;
	for (auto It = WormRegions.CreateIterator(); It; ++It)
	{
		if (RegionDistanceSquared(Center, It.Key(), RegionSize) > KeepSquared)
		{
			It.RemoveCurrent();
			++Evicted;
		}
	}
	return Evicted;
}
