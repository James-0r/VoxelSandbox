// Fill out your copyright notice in the Description page of Project Settings.

#include "VoxelGeneration.h"

#include "Curves/CurveFloat.h"
#include "Kismet/KismetMathLibrary.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "VoxelNoiseLibrary.h"
#include "VoxelTypes.h"

void FVoxelChunkData::Init(const int32 InWorldHeight)
{
	// Allocate once so the per-cell loops never resize; everything starts as Air.
	WorldHeight = FMath::Max(InWorldHeight, 0);
	Cubes.Init(Voxel::CubeType::Air, Voxel::ColumnCount * WorldHeight);
	Heights.Init(0, Voxel::ColumnCount);
	GrassDepths.Init(0, Voxel::ColumnCount);
	SnowLines.Init(0, Voxel::ColumnCount);
}

bool FVoxelChunkData::IsInitialised() const
{
	return WorldHeight > 0 && Cubes.Num() == Voxel::ColumnCount * WorldHeight && Heights.Num() == Voxel::ColumnCount;
}

namespace
{
	// Per-noise offsets in cube units, added on top of the seed's NoiseOffset so the four cave noise fields are
	// independent and nothing lines up with anything else. Chosen so offset × frequency never lands on a
	// multiple of 256 (Perlin's repeat period), which would make two fields identical.

	/** Offset for the cheese caverns noise. */
	const FVector CheeseNoiseOffset(-3000.0, 0.0, 0.0);

	/** Offset for tunnel noise A. */
	const FVector TunnelNoiseOffsetA(1000.0, 0.0, 0.0);

	/** Offset for tunnel noise B. */
	const FVector TunnelNoiseOffsetB(0.0, 2000.0, 0.0);

	/** Offset for the tunnel width noise. */
	const FVector TunnelWidthNoiseOffset(0.0, 0.0, 3000.0);

	/** Offset (X, Y, in cubes) for the biome noise, so biomes don't line up with the height noise. */
	const FVector2D BiomeNoiseOffset(5000.0, -7000.0);

	/**
	 * Width noise range mapped to TunnelWidthMin..Max. Perlin output sits mostly within ±0.5, so mapping
	 * that span (clamped) uses the whole width range instead of hovering in the middle.
	 */
	constexpr double TunnelWidthNoiseRange = 0.5;

	/**
	 * Cheese threshold at the inner edge of the surface fade. Perlin output stays below ~1, so a threshold of 1
	 * means "no cheese"; the fade blends from this down to the normal CaveCurve threshold.
	 */
	constexpr double CheeseThresholdAtMargin = 1.0;

	/** Lowest column height the terrain can produce: the top cube is never below Z 1, as in GenerateTerrain. */
	constexpr int32 MinColumnHeight = 1;

	/** Samples 3D Perlin at (Position + Offset) × Frequency, with Z frequency multiplied by ZScale. */
	float SampleCaveNoise(const FVector& Position, const FVector& Offset, const double Frequency, const double ZScale = 1.0)
	{
		return UVoxelNoiseLibrary::PerlinNoise3D(
			static_cast<float>((Position.X + Offset.X) * Frequency),
			static_cast<float>((Position.Y + Offset.Y) * Frequency),
			static_cast<float>((Position.Z + Offset.Z) * Frequency * ZScale));
	}

	/** 2D Perlin at ((A + OffsetA) × Frequency, (B + OffsetB) × Frequency), the pattern GenerateTerrain uses. */
	float SampleSurfaceNoise(const double A, const double OffsetA, const double B, const double OffsetB, const double Frequency)
	{
		return UVoxelNoiseLibrary::PerlinNoise2D(
			static_cast<float>((A + OffsetA) * Frequency),
			static_cast<float>((B + OffsetB) * Frequency));
	}
}

namespace VoxelGeneration
{
	int32 CubeIndex(const int32 X, const int32 Y, const int32 Z)
	{
		// Same layout as the Blueprint CellToIndex.
		return X + Y * Voxel::ChunkSize + Z * Voxel::ColumnCount;
	}

	int32 ColumnIndex(const int32 X, const int32 Y)
	{
		// Same layout as the Blueprint ColumnHeights index.
		return X + Y * Voxel::ChunkSize;
	}

	bool IsInsideColumns(const int32 X, const int32 Y)
	{
		// X/Y bounds only; Z has its own rules.
		return X >= 0 && X < Voxel::ChunkSize && Y >= 0 && Y < Voxel::ChunkSize;
	}

	uint8 GetTypeForCell(const int32 Z, const int32 SurfaceZ, const int32 TopDepth, const int32 SnowLine)
	{
		// Same rule as the Blueprint GetTypeForCell.
		if (Z > SurfaceZ - TopDepth)
		{
			return SurfaceZ >= SnowLine ? Voxel::CubeType::Snow : Voxel::CubeType::Grass;
		}
		return Voxel::CubeType::Rock;
	}

	double SampleHeightNoise(const double WorldX, const double WorldY, const FVoxelTerrainSettings& Terrain, const FVector& NoiseOffset)
	{
		// Fractal noise: each octave adds finer bumps (Freq × Lacunarity) that matter less (Amp × Persistence).
		// Dividing by the summed amplitudes keeps the result in about -1..1 whatever the octave count.
		// Same order of operations as the Blueprint, in double, so the float passed to Perlin is bit-identical.
		double Sum = 0.0;
		double Amp = 1.0;
		double Freq = Terrain.NoiseFrequency;
		double Norm = 0.0;

		for (int32 Octave = 0; Octave < Terrain.Octaves; ++Octave)
		{
			Sum += SampleSurfaceNoise(WorldX, NoiseOffset.X, WorldY, NoiseOffset.Y, Freq) * Amp;
			Norm += Amp;
			Amp *= Terrain.Persistence;
			Freq *= Terrain.Lacunarity;
		}

		return Norm > 0.0 ? Sum / Norm : 0.0;
	}

	double SampleMountainWeight(const int32 CellX, const int32 CellY, const FVoxelTerrainSettings& Terrain, const FVector& NoiseOffset)
	{
		// One slow noise per column, eased across the blend band so heights mix without a seam. Per column, not per
		// chunk: a per-chunk biome would put a cliff along every chunk border where two biomes meet.
		const double BiomeNoise = SampleSurfaceNoise(static_cast<double>(CellX), NoiseOffset.X + BiomeNoiseOffset.X,
			static_cast<double>(CellY), NoiseOffset.Y + BiomeNoiseOffset.Y, Terrain.BiomeFrequency);

		if (Terrain.BiomeBlend <= 0.0)
		{
			return BiomeNoise >= Terrain.BiomeThreshold ? 1.0 : 0.0;
		}
		return FMath::SmoothStep(Terrain.BiomeThreshold - Terrain.BiomeBlend, Terrain.BiomeThreshold + Terrain.BiomeBlend, BiomeNoise);
	}

	int32 SampleColumnHeight(const int32 CellX, const int32 CellY, const FVoxelTerrainSettings& Terrain, const FVector& NoiseOffset,
		const int32 WorldHeight)
	{
		// Noise → height curve(s) → fraction of WorldHeight, clamped so there's always a floor and a sky.
		// Both biome curves read the same height noise, so plains keep gentle bumps and rise smoothly into mountains.
		const double HeightNoise = SampleHeightNoise(static_cast<double>(CellX), static_cast<double>(CellY), Terrain, NoiseOffset);
		const float CurveInput = static_cast<float>(HeightNoise);
		const double MountainFraction = Terrain.HeightCurve ? static_cast<double>(Terrain.HeightCurve->GetFloatValue(CurveInput)) : 0.0;

		double HeightFraction = MountainFraction;
		if (Terrain.PlainsHeightCurve)
		{
			const double PlainsFraction = static_cast<double>(Terrain.PlainsHeightCurve->GetFloatValue(CurveInput));
			HeightFraction = FMath::Lerp(PlainsFraction, MountainFraction, SampleMountainWeight(CellX, CellY, Terrain, NoiseOffset));
		}

		return FMath::Clamp(UKismetMathLibrary::Round(HeightFraction * WorldHeight), MinColumnHeight, WorldHeight - 1);
	}

	FVoxelColumn SampleColumn(const int32 CellX, const int32 CellY, const FVoxelTerrainSettings& Terrain, const FVector& NoiseOffset,
		const int32 WorldHeight)
	{
		// Port of GenerateTerrain's per-column maths. The Kismet functions are the exact ones behind the Blueprint
		// Round and Map Range Clamped nodes, so the results match the Blueprint to the last bit.
		const double WorldX = static_cast<double>(CellX);
		const double WorldY = static_cast<double>(CellY);

		FVoxelColumn Column;
		Column.Height = SampleColumnHeight(CellX, CellY, Terrain, NoiseOffset, WorldHeight);

		// Surface layer depth: its own noise (offsets swapped to Y, X so it doesn't follow the height noise).
		const double DepthNoise = SampleSurfaceNoise(WorldX, NoiseOffset.Y, WorldY, NoiseOffset.X, Terrain.GrassDepthFrequency);
		Column.GrassDepth = UKismetMathLibrary::Round(UKismetMathLibrary::MapRangeClamped(DepthNoise,
			-Terrain.GrassNoiseRange, Terrain.GrassNoiseRange,
			static_cast<double>(Terrain.GrassDepthMin), static_cast<double>(Terrain.GrassDepthMax)));

		// Snow line: the average height plus a noisy ± variation (offsets Z, X).
		const double SnowNoise = SampleSurfaceNoise(WorldX, NoiseOffset.Z, WorldY, NoiseOffset.X, Terrain.SnowLineFrequency);
		Column.SnowLine = UKismetMathLibrary::Round(UKismetMathLibrary::MapRangeClamped(SnowNoise,
				-Terrain.GrassNoiseRange, Terrain.GrassNoiseRange,
				-static_cast<double>(Terrain.SnowLineVariation), static_cast<double>(Terrain.SnowLineVariation)))
			+ UKismetMathLibrary::Round(Terrain.SnowLineFraction * WorldHeight);

		return Column;
	}

	int32 GetColumnPadding(const FVoxelCaveSettings& Caves)
	{
		// The only pass that looks at other columns is the caves' surface margin. If another pass ever reads
		// neighbouring columns, take the max of its reach here too.
		return FMath::Max(Caves.SurfaceMargin, 0) + FMath::Max(Caves.SurfaceFade, 0);
	}

	void SampleColumns(const FIntPoint ChunkCoord, const int32 Padding, const FVoxelGenerationParams& Params, FVoxelChunkData& Data,
		TArray<int32>& OutPaddedHeights)
	{
		// One sample per column of a (ChunkSize + 2 × Padding)² grid. The border columns belong to neighbour
		// chunks, but they come from the same noise, so they're exactly what those chunks will compute themselves.
		TRACE_CPUPROFILER_EVENT_SCOPE(VoxelGeneration::SampleColumns);

		check(Data.IsInitialised());

		const int32 PaddedSize = Voxel::ChunkSize + 2 * Padding;
		OutPaddedHeights.SetNumUninitialized(PaddedSize * PaddedSize);

		for (int32 PaddedY = 0; PaddedY < PaddedSize; ++PaddedY)
		{
			for (int32 PaddedX = 0; PaddedX < PaddedSize; ++PaddedX)
			{
				const int32 LocalX = PaddedX - Padding;
				const int32 LocalY = PaddedY - Padding;
				const FVoxelColumn Column = SampleColumn(ChunkCoord.X * Voxel::ChunkSize + LocalX, ChunkCoord.Y * Voxel::ChunkSize + LocalY,
					Params.Terrain, Params.NoiseOffset, Params.WorldHeight);

				OutPaddedHeights[PaddedX + PaddedY * PaddedSize] = Column.Height;

				if (IsInsideColumns(LocalX, LocalY))
				{
					const int32 Index = ColumnIndex(LocalX, LocalY);
					Data.Heights[Index] = Column.Height;
					Data.GrassDepths[Index] = Column.GrassDepth;
					Data.SnowLines[Index] = Column.SnowLine;
				}
			}
		}
	}

	void FillColumns(FVoxelChunkData& Data)
	{
		// Z 0..Height inclusive, typed by GetTypeForCell.
		TRACE_CPUPROFILER_EVENT_SCOPE(VoxelGeneration::FillColumns);

		check(Data.IsInitialised());

		for (int32 X = 0; X < Voxel::ChunkSize; ++X)
		{
			for (int32 Y = 0; Y < Voxel::ChunkSize; ++Y)
			{
				const int32 Column = ColumnIndex(X, Y);
				const int32 Height = Data.Heights[Column];
				const int32 GrassDepth = Data.GrassDepths[Column];
				const int32 SnowLine = Data.SnowLines[Column];

				for (int32 Z = 0; Z <= Height; ++Z)
				{
					Data.Cubes[CubeIndex(X, Y, Z)] = GetTypeForCell(Z, Height, GrassDepth, SnowLine);
				}
			}
		}
	}

	void ComputeLowestNearbyHeights(const TArray<int32>& PaddedHeights, const int32 Padding, const int32 Radius, TArray<int32>& OutLowest)
	{
		// Minimum over a circle around each of the chunk's columns. The padded grid already holds every height
		// the circle can reach, so there are no lookups outside this array.
		const int32 PaddedSize = Voxel::ChunkSize + 2 * Padding;
		check(Radius <= Padding);
		check(PaddedHeights.Num() == PaddedSize * PaddedSize);

		OutLowest.SetNumUninitialized(Voxel::ColumnCount);
		const int32 RadiusSquared = Radius * Radius;

		for (int32 X = 0; X < Voxel::ChunkSize; ++X)
		{
			for (int32 Y = 0; Y < Voxel::ChunkSize; ++Y)
			{
				int32 Lowest = MissingHeight;

				for (int32 OffsetY = -Radius; OffsetY <= Radius; ++OffsetY)
				{
					for (int32 OffsetX = -Radius; OffsetX <= Radius; ++OffsetX)
					{
						if (OffsetX * OffsetX + OffsetY * OffsetY > RadiusSquared)
						{
							continue;
						}
						const int32 PaddedIndex = (X + Padding + OffsetX) + (Y + Padding + OffsetY) * PaddedSize;
						Lowest = FMath::Min(Lowest, PaddedHeights[PaddedIndex]);
					}
				}

				// The column itself is always inside the circle, so Lowest is never left as "missing".
				OutLowest[ColumnIndex(X, Y)] = Lowest;
			}
		}
	}

	void CarveCaves(const FIntPoint ChunkCoord, const FVector& NoiseOffset, const FVoxelCaveSettings& Caves, const int32 BedrockLayers,
		const int32 GrassDepthMax, const TArray<int32>& LowestNearby, FVoxelChunkData& Data)
	{
		// Cheese: one noise above a depth curve gives big blobs. Tunnels: two independent noises are each near zero
		// on a thin wavy sheet; where both sheets cross is a line, and "both within Width" thickens it into a tube.
		TRACE_CPUPROFILER_EVENT_SCOPE(VoxelGeneration::CarveCaves);

		check(Data.IsInitialised());
		check(LowestNearby.Num() == Voxel::ColumnCount);
		ensureMsgf(Caves.CaveCurve != nullptr, TEXT("CarveCaves: CaveCurve is not set; treating the cheese threshold as 0."));
		ensureMsgf(Caves.TunnelWidthMin <= Caves.TunnelWidthMax, TEXT("CarveCaves: TunnelWidthMin (%f) is above TunnelWidthMax (%f)."),
			Caves.TunnelWidthMin, Caves.TunnelWidthMax);

		for (int32 X = 0; X < Voxel::ChunkSize; ++X)
		{
			const double SampleX = static_cast<double>(Voxel::ChunkSize * ChunkCoord.X + X) + NoiseOffset.X;

			for (int32 Y = 0; Y < Voxel::ChunkSize; ++Y)
			{
				const double SampleY = static_cast<double>(Voxel::ChunkSize * ChunkCoord.Y + Y) + NoiseOffset.Y;
				const int32 Column = ColumnIndex(X, Y);
				const int32 Height = Data.Heights[Column];
				const int32 SurfaceZ = FMath::Min(Height, LowestNearby[Column]);
				const int32 LastZ = GetCaveTopZ(Height, SurfaceZ, Caves, GrassDepthMax);

				for (int32 Z = BedrockLayers; Z <= LastZ; ++Z)
				{
					const FVector Position(SampleX, SampleY, static_cast<double>(Z) + NoiseOffset.Z);
					if (ShouldCarveCaveCell(Position, Z, Height, SurfaceZ, Caves))
					{
						Data.Cubes[CubeIndex(X, Y, Z)] = Voxel::CubeType::Air;
					}
				}
			}
		}
	}

	int32 GetCaveTopZ(const int32 Height, const int32 SurfaceZ, const FVoxelCaveSettings& Caves, const int32 GrassDepthMax)
	{
		// Depth rule, plus: no cell closer than SurfaceMargin to the nearby surface.
		return FMath::Min(Height - GrassDepthMax, SurfaceZ - FMath::Max(Caves.SurfaceMargin, 0));
	}

	bool ShouldCarveCaveCell(const FVector& Position, const int32 Z, const int32 Height, const int32 SurfaceZ, const FVoxelCaveSettings& Caves)
	{
		// Cheese: one noise above a depth curve gives big blobs. Tunnels: two independent noises are each near zero
		// on a thin wavy sheet; where both sheets cross is a line, and "both within Width" thickens it into a tube.
		const int32 SurfaceMargin = FMath::Max(Caves.SurfaceMargin, 0);
		const int32 SurfaceFade = FMath::Max(Caves.SurfaceFade, 0);

		// No tunnel can be wider than this, so cells where |A| or |B| exceed it skip the remaining noise samples.
		const double WidestTunnel = FMath::Max(Caves.TunnelWidthMin, Caves.TunnelWidthMax);

		// Fade: 0 at the margin, 1 once the cell is SurfaceFade cubes deeper. Smoothstep avoids a visible step.
		const int32 Clearance = SurfaceZ - Z;
		const double FadeAlpha = SurfaceFade > 0
			? FMath::SmoothStep(0.0, 1.0, static_cast<double>(Clearance - SurfaceMargin) / static_cast<double>(SurfaceFade))
			: 1.0;

		// Cheese caverns: the threshold is raised near the surface and blends back to the curve.
		const float DepthFraction = static_cast<float>(static_cast<double>(Z) / static_cast<double>(Height));
		const double CurveThreshold = Caves.CaveCurve ? static_cast<double>(Caves.CaveCurve->GetFloatValue(DepthFraction)) : 0.0;
		const double CheeseThreshold = FMath::Lerp(FMath::Max(CheeseThresholdAtMargin, CurveThreshold), CurveThreshold, FadeAlpha);
		const double Cheese = SampleCaveNoise(Position, CheeseNoiseOffset, Caves.CheeseFrequency);
		if (Cheese > CheeseThreshold)
		{
			return true;
		}

		// Tunnels: both sheet noises must be near zero.
		const double TunnelA = FMath::Abs(SampleCaveNoise(Position, TunnelNoiseOffsetA, Caves.TunnelFrequency, Caves.TunnelVerticalScale));
		if (TunnelA >= WidestTunnel)
		{
			return false;
		}
		const double TunnelB = FMath::Abs(SampleCaveNoise(Position, TunnelNoiseOffsetB, Caves.TunnelFrequency, Caves.TunnelVerticalScale));
		if (TunnelB >= WidestTunnel)
		{
			return false;
		}

		// Width varies slowly along the tunnel, and shrinks to nothing towards the surface margin.
		const double WidthNoise = SampleCaveNoise(Position, TunnelWidthNoiseOffset, Caves.TunnelWidthFrequency);
		const double Width = FadeAlpha * FMath::GetMappedRangeValueClamped(
			FVector2D(-TunnelWidthNoiseRange, TunnelWidthNoiseRange),
			FVector2D(Caves.TunnelWidthMin, Caves.TunnelWidthMax),
			WidthNoise);

		return TunnelA < Width && TunnelB < Width;
	}

	FVoxelChunkData GenerateChunk(const FIntPoint ChunkCoord, const FVoxelGenerationParams& Params)
	{
		// The whole pipeline for one chunk with nothing but its inputs, so the result can't depend on which
		// neighbours exist or in which order chunks were made.
		TRACE_CPUPROFILER_EVENT_SCOPE(VoxelGeneration::GenerateChunk);

		FVoxelChunkData Data;
		Data.Init(Params.WorldHeight);
		if (!Data.IsInitialised())
		{
			return Data;
		}

		const int32 Padding = GetColumnPadding(Params.Caves);
		TArray<int32> PaddedHeights;
		SampleColumns(ChunkCoord, Padding, Params, Data, PaddedHeights);
		FillColumns(Data);

		if (Params.bEnableCaves)
		{
			TArray<int32> LowestNearby;
			ComputeLowestNearbyHeights(PaddedHeights, Padding, Padding, LowestNearby);
			CarveCaves(ChunkCoord, Params.NoiseOffset, Params.Caves, Params.BedrockLayers, Params.Terrain.GrassDepthMax, LowestNearby, Data);
		}

		return Data;
	}
}
