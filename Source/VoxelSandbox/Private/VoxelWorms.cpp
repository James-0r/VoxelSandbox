// Fill out your copyright notice in the Description page of Project Settings.

#include "VoxelWorms.h"

#include "Algo/Count.h"
#include "HAL/PlatformTime.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Templates/TypeHash.h"
#include "VoxelLog.h"
#include "VoxelNoiseLibrary.h"
#include "VoxelTypes.h"

namespace
{
	/**
	 * Added to the world seed for the worm random streams, so worms never reuse the terrain's random numbers.
	 * 0x9E3779B9 is the usual "golden ratio" hashing constant: its bits are well mixed.
	 */
	constexpr uint32 WormSeedSalt = 0x9E3779B9u;

	/** Distance a worm moves per step, in cubes. */
	constexpr double WormStepLength = 1.0;

	/** Tries to find a start column outside the spawn area before giving up on that worm. */
	constexpr int32 MaxWormStartAttempts = 8;

	/** Extra steps carved forward once a worm finds a cave ahead, so no thin wall is left between them. */
	constexpr int32 WormStepsIntoCave = 2;

	/** How far past the front of a step's sphere the worm looks for a cave, in cubes. */
	constexpr double WormLookAheadCubes = 1.0;

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

	/** Marks a column the sampler hasn't computed yet. */
	constexpr int32 UnsetColumn = TNumericLimits<int32>::Lowest();

	/** Cell that contains a position in cube units (cell X covers X..X+1). */
	FIntVector PositionToCell(const FVector& Position)
	{
		return FIntVector(FMath::FloorToInt(Position.X), FMath::FloorToInt(Position.Y), FMath::FloorToInt(Position.Z));
	}

	/** Floor division, so cell −1 is in region −1, not region 0. */
	int32 FloorDiv(const int32 Value, const int32 Divisor)
	{
		return Value >= 0 ? Value / Divisor : -((-Value + Divisor - 1) / Divisor);
	}

	/** 3D Perlin at Position × Frequency + Offset. */
	double SampleWormNoise(const FVector& Position, const double Frequency, const FVector& Offset)
	{
		const FVector Sample = Position * Frequency + Offset;
		return UVoxelNoiseLibrary::PerlinNoise3D(static_cast<float>(Sample.X), static_cast<float>(Sample.Y), static_cast<float>(Sample.Z));
	}

	/** One seed per region, mixed from the world seed and the region coordinate. */
	int32 MakeRegionSeed(const int32 Seed, const FIntPoint Region)
	{
		uint32 Hash = static_cast<uint32>(Seed) + WormSeedSalt;
		Hash = HashCombineFast(Hash, static_cast<uint32>(Region.X));
		Hash = HashCombineFast(Hash, static_cast<uint32>(Region.Y));
		return static_cast<int32>(Hash);
	}

	/**
	 * Column heights and "lowest height nearby" for one worm, computed from the noise on demand and cached in a
	 * square grid around the worm's start. A worm moves about one column per step and the cave test looks at a
	 * circle of columns around it, so most lookups are cache hits. Columns outside the grid are computed directly.
	 */
	class FWormColumnSampler
	{
	public:
		FWormColumnSampler(const FVoxelGenerationParams& InParams, const int32 InHalfSize)
			: Params(InParams)
			, Padding(VoxelGeneration::GetColumnPadding(InParams.Caves))
			, HalfSize(FMath::Max(InHalfSize, 0))
			, Size(2 * HalfSize + 1)
		{
			Heights.SetNumUninitialized(Size * Size);
			Lowest.SetNumUninitialized(Size * Size);
		}

		/** Centres the grid on a new worm's start column and forgets every cached value. */
		void Recentre(const int32 CenterX, const int32 CenterY)
		{
			Origin = FIntPoint(CenterX - HalfSize, CenterY - HalfSize);
			for (int32& Value : Heights)
			{
				Value = UnsetColumn;
			}
			for (int32& Value : Lowest)
			{
				Value = UnsetColumn;
			}
		}

		/** Surface height of a world column, from the noise. */
		int32 GetHeight(const int32 X, const int32 Y)
		{
			const int32 Index = GetIndex(X, Y);
			if (Index == INDEX_NONE)
			{
				return SampleHeight(X, Y);
			}
			int32& Cached = Heights[Index];
			if (Cached == UnsetColumn)
			{
				Cached = SampleHeight(X, Y);
			}
			return Cached;
		}

		/** True when the cave rule carves this cell: the same test CarveCaves runs, but from the noise alone. */
		bool IsCaveAir(const FIntVector& Cell)
		{
			if (!Params.bEnableCaves || Cell.Z < Params.BedrockLayers)
			{
				return false;
			}

			const int32 Height = GetHeight(Cell.X, Cell.Y);

			// Cheap check first: caves never reach the top GrassDepthMax cubes, so skip the lowest-nearby search.
			if (Cell.Z > Height - Params.Terrain.GrassDepthMax)
			{
				return false;
			}

			const int32 SurfaceZ = FMath::Min(Height, GetLowestNearby(Cell.X, Cell.Y));
			if (Cell.Z > VoxelGeneration::GetCaveTopZ(Height, SurfaceZ, Params.Caves, Params.Terrain.GrassDepthMax))
			{
				return false;
			}

			const FVector Position(
				static_cast<double>(Cell.X) + Params.NoiseOffset.X,
				static_cast<double>(Cell.Y) + Params.NoiseOffset.Y,
				static_cast<double>(Cell.Z) + Params.NoiseOffset.Z);
			return VoxelGeneration::ShouldCarveCaveCell(Position, Cell.Z, Height, SurfaceZ, Params.Caves);
		}

	private:
		/** Grid index of a world column, or INDEX_NONE outside the grid. */
		int32 GetIndex(const int32 X, const int32 Y) const
		{
			const int32 LocalX = X - Origin.X;
			const int32 LocalY = Y - Origin.Y;
			return (LocalX >= 0 && LocalX < Size && LocalY >= 0 && LocalY < Size) ? LocalX + LocalY * Size : INDEX_NONE;
		}

		int32 SampleHeight(const int32 X, const int32 Y) const
		{
			return VoxelGeneration::SampleColumnHeight(X, Y, Params.Terrain, Params.NoiseOffset, Params.WorldHeight);
		}

		/** Lowest height within a circle of Padding columns: what CarveCaves gets from its padded grid. */
		int32 GetLowestNearby(const int32 X, const int32 Y)
		{
			const int32 Index = GetIndex(X, Y);
			if (Index != INDEX_NONE && Lowest[Index] != UnsetColumn)
			{
				return Lowest[Index];
			}

			const int32 RadiusSquared = Padding * Padding;
			int32 Result = TNumericLimits<int32>::Max();
			for (int32 OffsetY = -Padding; OffsetY <= Padding; ++OffsetY)
			{
				for (int32 OffsetX = -Padding; OffsetX <= Padding; ++OffsetX)
				{
					if (OffsetX * OffsetX + OffsetY * OffsetY <= RadiusSquared)
					{
						Result = FMath::Min(Result, GetHeight(X + OffsetX, Y + OffsetY));
					}
				}
			}

			if (Index != INDEX_NONE)
			{
				Lowest[Index] = Result;
			}
			return Result;
		}

		const FVoxelGenerationParams& Params;
		const int32 Padding;
		const int32 HalfSize;
		const int32 Size;
		FIntPoint Origin = FIntPoint::ZeroValue;
		TArray<int32> Heights;
		TArray<int32> Lowest;
	};

	/** Adds one sphere to a path and grows the path's bounds around it. */
	void AddSphere(FVoxelWormPath& Path, const FVector& Center, const double Radius)
	{
		Path.Spheres.Add({ Center, Radius });
		Path.Bounds += FBox(Center - FVector(Radius), Center + FVector(Radius));
	}

	/**
	 * Walks one worm from Start and records its spheres. Each step: check bedrock, steer with noise (heading
	 * wanders, slope = DownwardBias plus noise), pick the radius from noise, look ahead for a cave, add a sphere,
	 * move on. Same rules as the old whole-world CarveWorms, but the cave test reads the noise, not loaded chunks.
	 */
	FVoxelWormPath WalkWorm(FVector Position, double Heading, const FVector& WormNoiseOffset, const FVoxelWormPlanParams& Params,
		FWormColumnSampler& Sampler)
	{
		const FVoxelWormSettings& Worms = Params.Worms;
		const double StartZ = Position.Z;
		const double MinRadius = FMath::Min(Worms.MinRadius, Worms.MaxRadius);
		const double MaxRadius = FMath::Max(Worms.MinRadius, Worms.MaxRadius);

		FVoxelWormPath Path;
		for (int32 Step = 0; Step < Worms.MaxSteps; ++Step)
		{
			if (PositionToCell(Position).Z <= Params.Generation.BedrockLayers)
			{
				break;
			}

			// Steering. The noise is sampled at the worm's position, so it changes smoothly as the worm moves
			// and gives gentle bends rather than random jitter.
			const FVector NoisePosition = Position + WormNoiseOffset;
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

			// Look just past the front of this step's sphere. Cave air there means the worm has reached a cave.
			const bool bDeepEnough = StartZ - Position.Z >= Worms.MinDepthBeforeConnect;
			const FIntVector Ahead = PositionToCell(Position + Direction * (Radius + WormLookAheadCubes));
			if (bDeepEnough && Sampler.IsCaveAir(Ahead))
			{
				for (int32 Extra = 0; Extra <= WormStepsIntoCave; ++Extra)
				{
					AddSphere(Path, Position + Direction * (Extra * WormStepLength), Radius);
				}
				Path.bConnected = true;
				break;
			}

			AddSphere(Path, Position, Radius);
			Position += Direction * WormStepLength;
		}
		return Path;
	}
}

namespace VoxelWorms
{
	int32 GetRegionSizeCubes(const FVoxelWormSettings& Worms)
	{
		return FMath::Max(Worms.RegionSizeChunks, 1) * Voxel::ChunkSize;
	}

	FIntPoint CellToRegion(const int32 CellX, const int32 CellY, const FVoxelWormSettings& Worms)
	{
		const int32 RegionSize = GetRegionSizeCubes(Worms);
		return FIntPoint(FloorDiv(CellX, RegionSize), FloorDiv(CellY, RegionSize));
	}

	int32 GetMaxReachCubes(const FVoxelWormSettings& Worms)
	{
		// Every step moves one cube; on connect it carves a few more; each sphere reaches Radius beyond its centre.
		const double Steps = (FMath::Max(Worms.MaxSteps, 0) + WormStepsIntoCave) * WormStepLength;
		return FMath::CeilToInt(Steps + FMath::Max(Worms.MinRadius, Worms.MaxRadius)) + 1;
	}

	TArray<FVoxelWormPath> PlanRegion(const FIntPoint Region, const FVoxelWormPlanParams& Params)
	{
		// One random stream per region drives every choice (count, start columns, headings, noise offsets) in a
		// fixed order, so the same seed always gives the same worms, whichever region is planned first.
		TRACE_CPUPROFILER_EVENT_SCOPE(VoxelWorms::PlanRegion);

		const FVoxelWormSettings& Worms = Params.Worms;
		TArray<FVoxelWormPath> Paths;
		if (!Worms.bEnabled)
		{
			return Paths;
		}

		const double StartSeconds = FPlatformTime::Seconds();
		FRandomStream Stream(MakeRegionSeed(Params.Seed, Region));

		// Average count per region, with the fraction decided by the stream: 4.8 → 4 or 5.
		const double ExpectedWorms = FMath::Square(static_cast<double>(FMath::Max(Worms.RegionSizeChunks, 1))) * FMath::Max(Worms.WormsPerChunk, 0.0);
		int32 WormCount = FMath::FloorToInt(ExpectedWorms);
		if (Stream.FRand() < ExpectedWorms - WormCount)
		{
			++WormCount;
		}

		const int32 RegionSize = GetRegionSizeCubes(Worms);
		const int64 SpawnClearSquared = static_cast<int64>(Worms.SpawnClearRadius) * Worms.SpawnClearRadius;
		FWormColumnSampler Sampler(Params.Generation, GetMaxReachCubes(Worms) + VoxelGeneration::GetColumnPadding(Params.Generation.Caves) + 1);

		for (int32 WormIndex = 0; WormIndex < WormCount; ++WormIndex)
		{
			// Start column: random column in the region, retried a few times if it lands in the spawn area.
			bool bFoundStart = false;
			int32 StartX = 0;
			int32 StartY = 0;
			for (int32 Attempt = 0; Attempt < MaxWormStartAttempts && !bFoundStart; ++Attempt)
			{
				const int32 CellX = Region.X * RegionSize + Stream.RandRange(0, RegionSize - 1);
				const int32 CellY = Region.Y * RegionSize + Stream.RandRange(0, RegionSize - 1);
				if (static_cast<int64>(CellX) * CellX + static_cast<int64>(CellY) * CellY < SpawnClearSquared)
				{
					continue;
				}
				StartX = CellX;
				StartY = CellY;
				bFoundStart = true;
			}

			// Drawn even for skipped worms, in the same order every time, so the sequence stays reproducible.
			const double Heading = Stream.FRandRange(0.0, UE_TWO_PI);
			const FVector WormNoiseOffset(
				Stream.FRandRange(-WormNoiseOffsetRange, WormNoiseOffsetRange),
				Stream.FRandRange(-WormNoiseOffsetRange, WormNoiseOffsetRange),
				Stream.FRandRange(-WormNoiseOffsetRange, WormNoiseOffsetRange));

			if (!bFoundStart)
			{
				continue;
			}

			// Centre of the column's top cube, in cube units.
			Sampler.Recentre(StartX, StartY);
			const int32 Height = Sampler.GetHeight(StartX, StartY);
			const FVector Start(StartX + 0.5, StartY + 0.5, Height + 0.5);
			Paths.Add(WalkWorm(Start, Heading, WormNoiseOffset, Params, Sampler));
		}

		const int32 Connected = Algo::CountIf(Paths, [](const FVoxelWormPath& Path) { return Path.bConnected; });
		UE_LOG(LogVoxel, Verbose, TEXT("PlanRegion (%d, %d): %d worms, %d connected to a cave, in %.2f ms."),
			Region.X, Region.Y, Paths.Num(), Connected, (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
		return Paths;
	}

	int32 CarveIntoChunk(const TConstArrayView<const FVoxelWormPath*> Paths, const FIntPoint ChunkCoord, const int32 BedrockLayers,
		FVoxelChunkData& Data)
	{
		// Each worm is tested against the chunk's box first, then each of its spheres, so only spheres that
		// actually touch this chunk are walked cell by cell.
		TRACE_CPUPROFILER_EVENT_SCOPE(VoxelWorms::CarveIntoChunk);

		if (!ensure(Data.IsInitialised()))
		{
			return 0;
		}

		const int32 MinX = ChunkCoord.X * Voxel::ChunkSize;
		const int32 MinY = ChunkCoord.Y * Voxel::ChunkSize;
		const FBox ChunkBox(FVector(MinX, MinY, 0.0), FVector(MinX + Voxel::ChunkSize, MinY + Voxel::ChunkSize, Data.WorldHeight));

		int32 Changed = 0;
		for (const FVoxelWormPath* Path : Paths)
		{
			if (!Path || !Path->Bounds.Intersect(ChunkBox))
			{
				continue;
			}

			for (const FVoxelWormSphere& Sphere : Path->Spheres)
			{
				const FVector Extent(Sphere.Radius);
				if (!FBox(Sphere.Center - Extent, Sphere.Center + Extent).Intersect(ChunkBox))
				{
					continue;
				}

				const FIntVector Low = PositionToCell(Sphere.Center - Extent);
				const FIntVector High = PositionToCell(Sphere.Center + Extent);
				const double RadiusSquared = Sphere.Radius * Sphere.Radius;

				for (int32 Z = FMath::Max(Low.Z, BedrockLayers + 1); Z <= FMath::Min(High.Z, Data.WorldHeight - 1); ++Z)
				{
					for (int32 Y = FMath::Max(Low.Y, MinY); Y <= FMath::Min(High.Y, MinY + Voxel::ChunkSize - 1); ++Y)
					{
						for (int32 X = FMath::Max(Low.X, MinX); X <= FMath::Min(High.X, MinX + Voxel::ChunkSize - 1); ++X)
						{
							if (FVector::DistSquared(FVector(X + 0.5, Y + 0.5, Z + 0.5), Sphere.Center) > RadiusSquared)
							{
								continue;
							}

							uint8& Cube = Data.Cubes[VoxelGeneration::CubeIndex(X - MinX, Y - MinY, Z)];
							if (Cube != Voxel::CubeType::Air)
							{
								Cube = Voxel::CubeType::Air;
								++Changed;
							}
						}
					}
				}
			}
		}
		return Changed;
	}
}
