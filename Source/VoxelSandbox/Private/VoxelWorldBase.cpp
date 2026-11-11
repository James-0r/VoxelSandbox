// Fill out your copyright notice in the Description page of Project Settings.

#include "VoxelWorldBase.h"

#include "HAL/PlatformTime.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "VoxelChunkBase.h"
#include "VoxelNoiseLibrary.h"
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

bool AVoxelWorldBase::SetCubeByte(const FIntVector Cell, const uint8 Type)
{
	// Raw write for generation passes: the Blueprint SetCube adds the mesh rebuild and the save record on top.
	AVoxelChunkBase* Chunk = FindChunk(CellToChunkCoord(Cell));
	if (!Chunk)
	{
		return false;
	}

	const FIntVector Local = CellToLocal(Cell);
	return Chunk->SetLocalCubeByte(Local.X, Local.Y, Local.Z, Type);
}

namespace
{
	/**
	 * Added to the world seed for the worm random stream, so worms never reuse the terrain's random numbers.
	 * 0x9E3779B9 is the usual "golden ratio" hashing constant: its bits are well mixed.
	 */
	constexpr uint32 WormSeedSalt = 0x9E3779B9u;

	/** Distance a worm moves per step, in cubes. */
	constexpr double WormStepLength = 1.0;

	/** Tries to find a start column outside the spawn area before giving up on that worm. */
	constexpr int32 MaxWormStartAttempts = 8;

	/** Extra steps carved forward once a worm finds a cave ahead, so no thin wall is left between them. */
	constexpr int32 WormStepsIntoCave = 2;

	/** Range of each random per-worm noise offset, in cubes; gives every worm its own wiggles. */
	constexpr double WormNoiseOffsetRange = 1000.0;

	/** Perlin output sits mostly within ±0.5; this span is mapped to MinRadius..MaxRadius. */
	constexpr double WormRadiusNoiseRange = 0.5;

	// Fixed offsets so the heading, slope and radius noises are independent of each other.

	/** Offset for the heading (left / right) noise. */
	const FVector WormHeadingNoiseOffset(0.0, 0.0, 0.0);

	/** Offset for the slope (up / down) noise. */
	const FVector WormSlopeNoiseOffset(1700.0, 0.0, 0.0);

	/** Offset for the radius noise. */
	const FVector WormRadiusNoiseOffset(0.0, 2900.0, 0.0);

	/** Why a worm stopped; counted for the log. */
	enum class EWormStop : uint8
	{
		MaxSteps,
		Bedrock,
		LeftWorld,
		Connected,
		Count,
	};

	/** Cell that contains a position in cube units (cell X covers X..X+1). */
	FIntVector PositionToCell(const FVector& Position)
	{
		return FIntVector(FMath::FloorToInt(Position.X), FMath::FloorToInt(Position.Y), FMath::FloorToInt(Position.Z));
	}

	/** 3D Perlin at Position × Frequency + Offset. */
	double SampleWormNoise(const FVector& Position, const double Frequency, const FVector& Offset)
	{
		const FVector Sample = Position * Frequency + Offset;
		return UVoxelNoiseLibrary::PerlinNoise3D(static_cast<float>(Sample.X), static_cast<float>(Sample.Y), static_cast<float>(Sample.Z));
	}

	/**
	 * True when the cell is air that was there before this worm: inside a loaded chunk, at or below its column's
	 * top (so it's a cave, not the sky) and not carved by this worm itself.
	 */
	bool IsExistingCaveAir(const AVoxelWorldBase& World, const FIntVector& Cell, const TSet<FIntVector>& OwnCells)
	{
		const AVoxelChunkBase* Chunk = World.FindChunk(AVoxelWorldBase::CellToChunkCoord(Cell));
		if (!Chunk || Cell.Z < 0)
		{
			return false;
		}

		const FIntVector Local = AVoxelWorldBase::CellToLocal(Cell);
		return Cell.Z <= Chunk->GetColumnHeight(Local.X, Local.Y)
			&& Chunk->GetLocalCubeByte(Local.X, Local.Y, Local.Z) == Voxel::CubeType::Air
			&& !OwnCells.Contains(Cell);
	}

	/** Sets every solid cell whose centre is within Radius of Center to Air, above bedrock. Remembers what it carved. */
	void CarveSphere(AVoxelWorldBase& World, const FVector& Center, const double Radius, const int32 BedrockLayers,
		TSet<FIntVector>& OwnCells)
	{
		const FIntVector MinCell = PositionToCell(Center - FVector(Radius));
		const FIntVector MaxCell = PositionToCell(Center + FVector(Radius));
		const double RadiusSquared = Radius * Radius;

		for (int32 Z = FMath::Max(MinCell.Z, BedrockLayers + 1); Z <= MaxCell.Z; ++Z)
		{
			for (int32 Y = MinCell.Y; Y <= MaxCell.Y; ++Y)
			{
				for (int32 X = MinCell.X; X <= MaxCell.X; ++X)
				{
					const FIntVector Cell(X, Y, Z);
					const FVector CellCenter(X + 0.5, Y + 0.5, Z + 0.5);
					if (FVector::DistSquared(CellCenter, Center) > RadiusSquared
						|| World.GetCubeByte(Cell) == Voxel::CubeType::Air)
					{
						continue;
					}

					if (World.SetCubeByte(Cell, Voxel::CubeType::Air))
					{
						OwnCells.Add(Cell);
					}
				}
			}
		}
	}

	/**
	 * Walks one worm from Start and carves it. Each step: check the stop rules, steer with noise (heading wanders,
	 * slope = DownwardBias plus noise), pick the radius from noise, look ahead for a cave, carve a sphere, move on.
	 */
	EWormStop CarveWorm(AVoxelWorldBase& World, FVector Position, double Heading, const FVector& NoiseOffset,
		const FVoxelWormSettings& Worms, const int32 BedrockLayers)
	{
		const double StartZ = Position.Z;
		const double MinRadius = FMath::Min(Worms.MinRadius, Worms.MaxRadius);
		const double MaxRadius = FMath::Max(Worms.MinRadius, Worms.MaxRadius);
		TSet<FIntVector> OwnCells;

		for (int32 Step = 0; Step < Worms.MaxSteps; ++Step)
		{
			const FIntVector Cell = PositionToCell(Position);
			if (!World.FindChunk(AVoxelWorldBase::CellToChunkCoord(Cell)))
			{
				return EWormStop::LeftWorld;
			}
			if (Cell.Z <= BedrockLayers)
			{
				return EWormStop::Bedrock;
			}

			// Steering. The noise is sampled at the worm's position, so it changes smoothly as the worm moves
			// and gives gentle bends rather than random jitter.
			const FVector NoisePosition = Position + NoiseOffset;
			Heading += Worms.TurnStrength * SampleWormNoise(NoisePosition, Worms.TurnFrequency, WormHeadingNoiseOffset);
			const double Drop = FMath::Clamp(
				Worms.DownwardBias + Worms.TurnStrength * SampleWormNoise(NoisePosition, Worms.TurnFrequency, WormSlopeNoiseOffset),
				-1.0, 1.0);
			const double Level = FMath::Sqrt(1.0 - Drop * Drop);
			const FVector Direction(FMath::Cos(Heading) * Level, FMath::Sin(Heading) * Level, -Drop);

			const double Radius = FMath::GetMappedRangeValueClamped(
				FVector2D(-WormRadiusNoiseRange, WormRadiusNoiseRange),
				FVector2D(MinRadius, MaxRadius),
				SampleWormNoise(NoisePosition, Worms.TurnFrequency, WormRadiusNoiseOffset));

			// Look just past the front of this step's sphere. Air there that this worm didn't make is a real cave.
			const bool bDeepEnough = StartZ - Position.Z >= Worms.MinDepthBeforeConnect;
			const FIntVector Ahead = PositionToCell(Position + Direction * (Radius + 1.0));
			if (bDeepEnough && IsExistingCaveAir(World, Ahead, OwnCells))
			{
				for (int32 Extra = 0; Extra <= WormStepsIntoCave; ++Extra)
				{
					CarveSphere(World, Position + Direction * (Extra * WormStepLength), Radius, BedrockLayers, OwnCells);
				}
				return EWormStop::Connected;
			}

			CarveSphere(World, Position, Radius, BedrockLayers, OwnCells);
			Position += Direction * WormStepLength;
		}
		return EWormStop::MaxSteps;
	}
}

void AVoxelWorldBase::CarveWorms(const int32 Seed, const FVoxelWormSettings& Worms, const int32 BedrockLayers,
	int32& WormsCarved, int32& WormsConnected)
{
	// One random stream drives every choice (start columns, headings, noise offsets) in a fixed order, so the
	// same seed and the same chunk set always give the same worms.
	TRACE_CPUPROFILER_EVENT_SCOPE(AVoxelWorldBase::CarveWorms);

	WormsCarved = 0;
	WormsConnected = 0;

	if (!Worms.bEnabled)
	{
		return;
	}

	const double StartSeconds = FPlatformTime::Seconds();

	// Map order follows spawn order, so sort the keys: start picks must not depend on how chunks were added.
	TArray<FIntPoint> ChunkCoords;
	Chunks.GenerateKeyArray(ChunkCoords);
	ChunkCoords.Sort([](const FIntPoint& A, const FIntPoint& B) { return A.X != B.X ? A.X < B.X : A.Y < B.Y; });

	if (ChunkCoords.IsEmpty())
	{
		UE_LOG(LogVoxel, Warning, TEXT("CarveWorms: no chunks registered, nothing to carve."));
		return;
	}

	FRandomStream Stream(static_cast<int32>(static_cast<uint32>(Seed) + WormSeedSalt));
	const int32 WormCount = FMath::RoundToInt(ChunkCoords.Num() * FMath::Max(Worms.WormsPerChunk, 0.0));
	const int64 SpawnClearSquared = static_cast<int64>(Worms.SpawnClearRadius) * Worms.SpawnClearRadius;

	int32 SkippedStarts = 0;
	int32 StopCounts[static_cast<int32>(EWormStop::Count)] = {};

	for (int32 WormIndex = 0; WormIndex < WormCount; ++WormIndex)
	{
		// Start column: random chunk, random column, retried a few times if it lands in the spawn area.
		bool bFoundStart = false;
		FVector Start = FVector::ZeroVector;

		for (int32 Attempt = 0; Attempt < MaxWormStartAttempts && !bFoundStart; ++Attempt)
		{
			const FIntPoint ChunkCoord = ChunkCoords[Stream.RandRange(0, ChunkCoords.Num() - 1)];
			const int32 LocalX = Stream.RandRange(0, Voxel::ChunkSize - 1);
			const int32 LocalY = Stream.RandRange(0, Voxel::ChunkSize - 1);
			const int32 CellX = ChunkCoord.X * Voxel::ChunkSize + LocalX;
			const int32 CellY = ChunkCoord.Y * Voxel::ChunkSize + LocalY;

			if (static_cast<int64>(CellX) * CellX + static_cast<int64>(CellY) * CellY < SpawnClearSquared)
			{
				continue;
			}

			const int32 Height = FindChunk(ChunkCoord)->GetColumnHeight(LocalX, LocalY);
			if (Height == INDEX_NONE)
			{
				continue;
			}

			// Centre of the column's top cube, in cube units.
			Start = FVector(CellX + 0.5, CellY + 0.5, Height + 0.5);
			bFoundStart = true;
		}

		// Drawn even for skipped worms, in the same order every time, so the sequence stays reproducible.
		const double Heading = Stream.FRandRange(0.0, UE_TWO_PI);
		const FVector NoiseOffset(
			Stream.FRandRange(-WormNoiseOffsetRange, WormNoiseOffsetRange),
			Stream.FRandRange(-WormNoiseOffsetRange, WormNoiseOffsetRange),
			Stream.FRandRange(-WormNoiseOffsetRange, WormNoiseOffsetRange));

		if (!bFoundStart)
		{
			++SkippedStarts;
			continue;
		}

		const EWormStop Stop = CarveWorm(*this, Start, Heading, NoiseOffset, Worms, BedrockLayers);
		++StopCounts[static_cast<int32>(Stop)];
		++WormsCarved;
	}

	WormsConnected = StopCounts[static_cast<int32>(EWormStop::Connected)];
	const double ElapsedMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;

	UE_LOG(LogVoxel, Log,
		TEXT("CarveWorms: %d worms carved, %d connected to a cave (others stopped: %d bedrock, %d world edge, %d max steps; %d starts skipped near spawn) in %.2f ms."),
		WormsCarved, WormsConnected,
		StopCounts[static_cast<int32>(EWormStop::Bedrock)],
		StopCounts[static_cast<int32>(EWormStop::LeftWorld)],
		StopCounts[static_cast<int32>(EWormStop::MaxSteps)],
		SkippedStarts, ElapsedMs);
}
