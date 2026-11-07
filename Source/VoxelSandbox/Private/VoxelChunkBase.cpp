// Fill out your copyright notice in the Description page of Project Settings.

#include "VoxelChunkBase.h"

#include "Curves/CurveFloat.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "VoxelNoiseLibrary.h"
#include "VoxelTypes.h"
#include "VoxelWorldBase.h"

AVoxelChunkBase::AVoxelChunkBase()
{
	// Chunks are driven by events (generation, mining, placing), never by Tick.
	PrimaryActorTick.bCanEverTick = false;
}

void AVoxelChunkBase::InitChunkData(const FIntPoint InChunkCoord, const int32 InWorldHeight)
{
	// Allocate once per chunk so the per-cell loops never resize; everything starts as Air.
	if (!ensureMsgf(InWorldHeight > 0, TEXT("InitChunkData: WorldHeight must be positive (got %d)."), InWorldHeight))
	{
		return;
	}

	ChunkCoord = InChunkCoord;
	WorldHeight = InWorldHeight;

	Cubes.Init(Voxel::CubeType::Air, Voxel::ColumnCount * WorldHeight);
	Heights.Init(0, Voxel::ColumnCount);
	GrassDepths.Init(0, Voxel::ColumnCount);
	SnowLines.Init(0, Voxel::ColumnCount);
}

void AVoxelChunkBase::SetColumn(const int32 X, const int32 Y, const int32 Height, const int32 GrassDepth, const int32 SnowLine)
{
	// Blueprint computes the per-column noise; C++ only stores it for the per-cell loops.
	if (!ensureMsgf(IsInsideColumns(X, Y), TEXT("SetColumn: column (%d, %d) is outside the chunk."), X, Y)
		|| !ensureMsgf(Heights.Num() == Voxel::ColumnCount, TEXT("SetColumn: call InitChunkData first.")))
	{
		return;
	}

	const int32 Column = ColumnIndex(X, Y);
	Heights[Column] = FMath::Clamp(Height, 0, WorldHeight - 1);
	GrassDepths[Column] = GrassDepth;
	SnowLines[Column] = SnowLine;
}

void AVoxelChunkBase::FillColumns()
{
	// Same loop as the old GenerateTerrain Z loop: Z 0..Height inclusive, typed by GetTypeForCell.
	TRACE_CPUPROFILER_EVENT_SCOPE(AVoxelChunkBase::FillColumns);

	check(Cubes.Num() == Voxel::ColumnCount * WorldHeight);
	check(Heights.Num() == Voxel::ColumnCount);

	for (int32 X = 0; X < Voxel::ChunkSize; ++X)
	{
		for (int32 Y = 0; Y < Voxel::ChunkSize; ++Y)
		{
			const int32 Column = ColumnIndex(X, Y);
			const int32 Height = Heights[Column];
			const int32 GrassDepth = GrassDepths[Column];
			const int32 SnowLine = SnowLines[Column];

			for (int32 Z = 0; Z <= Height; ++Z)
			{
				Cubes[CubeIndex(X, Y, Z)] = GetTypeForCell(Z, Height, GrassDepth, SnowLine);
			}
		}
	}
}

namespace
{
	// Per-noise offsets in cube units, added on top of the seed's NoiseOffset so the four noise fields are
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

	/**
	 * Width noise range mapped to TunnelWidthMin..Max. Perlin output sits mostly within ±0.5, so mapping
	 * that span (clamped) uses the whole width range instead of hovering in the middle.
	 */
	constexpr double TunnelWidthNoiseRange = 0.5;

	/** Samples 3D Perlin at (Position + Offset) × Frequency, with Z frequency multiplied by ZScale. */
	float SampleCaveNoise(const FVector& Position, const FVector& Offset, const double Frequency, const double ZScale = 1.0)
	{
		return UVoxelNoiseLibrary::PerlinNoise3D(
			static_cast<float>((Position.X + Offset.X) * Frequency),
			static_cast<float>((Position.Y + Offset.Y) * Frequency),
			static_cast<float>((Position.Z + Offset.Z) * Frequency * ZScale));
	}
}

void AVoxelChunkBase::CarveCaves(const FVector NoiseOffset, const FVoxelCaveSettings& Caves, const int32 BedrockLayers,
	const int32 GrassDepthMax)
{
	// Cheese: one noise above a depth curve gives big blobs. Tunnels: two independent noises are each near zero
	// on a thin wavy sheet; where both sheets cross is a line, and "both within Width" thickens it into a tube.
	TRACE_CPUPROFILER_EVENT_SCOPE(AVoxelChunkBase::CarveCaves);

	check(Cubes.Num() == Voxel::ColumnCount * WorldHeight);
	check(Heights.Num() == Voxel::ColumnCount);
	ensureMsgf(Caves.CaveCurve != nullptr, TEXT("CarveCaves: CaveCurve is not set; treating the cheese threshold as 0."));
	ensureMsgf(Caves.TunnelWidthMin <= Caves.TunnelWidthMax, TEXT("CarveCaves: TunnelWidthMin (%f) is above TunnelWidthMax (%f)."),
		Caves.TunnelWidthMin, Caves.TunnelWidthMax);

	// No tunnel can be wider than this, so cells where |A| or |B| exceed it skip the remaining noise samples.
	const double WidestTunnel = FMath::Max(Caves.TunnelWidthMin, Caves.TunnelWidthMax);

	for (int32 X = 0; X < Voxel::ChunkSize; ++X)
	{
		const double SampleX = static_cast<double>(Voxel::ChunkSize * ChunkCoord.X + X) + NoiseOffset.X;

		for (int32 Y = 0; Y < Voxel::ChunkSize; ++Y)
		{
			const double SampleY = static_cast<double>(Voxel::ChunkSize * ChunkCoord.Y + Y) + NoiseOffset.Y;
			const int32 Height = Heights[ColumnIndex(X, Y)];
			const int32 LastZ = Height - GrassDepthMax;

			for (int32 Z = BedrockLayers; Z <= LastZ; ++Z)
			{
				const FVector Position(SampleX, SampleY, static_cast<double>(Z) + NoiseOffset.Z);

				// Cheese caverns.
				const float DepthFraction = static_cast<float>(static_cast<double>(Z) / static_cast<double>(Height));
				const double CheeseThreshold = Caves.CaveCurve ? static_cast<double>(Caves.CaveCurve->GetFloatValue(DepthFraction)) : 0.0;
				const double Cheese = SampleCaveNoise(Position, CheeseNoiseOffset, Caves.CheeseFrequency);
				if (Cheese > CheeseThreshold)
				{
					Cubes[CubeIndex(X, Y, Z)] = Voxel::CubeType::Air;
					continue;
				}

				// Tunnels: both sheet noises must be near zero.
				const double TunnelA = FMath::Abs(SampleCaveNoise(Position, TunnelNoiseOffsetA, Caves.TunnelFrequency, Caves.TunnelVerticalScale));
				if (TunnelA >= WidestTunnel)
				{
					continue;
				}
				const double TunnelB = FMath::Abs(SampleCaveNoise(Position, TunnelNoiseOffsetB, Caves.TunnelFrequency, Caves.TunnelVerticalScale));
				if (TunnelB >= WidestTunnel)
				{
					continue;
				}

				// Width varies slowly along the tunnel.
				const double WidthNoise = SampleCaveNoise(Position, TunnelWidthNoiseOffset, Caves.TunnelWidthFrequency);
				const double Width = FMath::GetMappedRangeValueClamped(
					FVector2D(-TunnelWidthNoiseRange, TunnelWidthNoiseRange),
					FVector2D(Caves.TunnelWidthMin, Caves.TunnelWidthMax),
					WidthNoise);

				if (TunnelA < Width && TunnelB < Width)
				{
					Cubes[CubeIndex(X, Y, Z)] = Voxel::CubeType::Air;
				}
			}
		}
	}
}

void AVoxelChunkBase::BuildSectionData(const uint8 CubeType, const double CubeSize, TArray<FVector>& Vertices,
	TArray<int32>& Triangles, TArray<FVector>& Normals, TArray<FVector2D>& UVs) const
{
	// Same visit order as the Blueprint (X, Y, Z, then FaceTable order) so the mesh arrays match.
	TRACE_CPUPROFILER_EVENT_SCOPE(AVoxelChunkBase::BuildSectionData);

	Vertices.Reset();
	Triangles.Reset();
	Normals.Reset();
	UVs.Reset();

	check(Cubes.Num() == Voxel::ColumnCount * WorldHeight);
	check(Heights.Num() == Voxel::ColumnCount);

	for (int32 X = 0; X < Voxel::ChunkSize; ++X)
	{
		for (int32 Y = 0; Y < Voxel::ChunkSize; ++Y)
		{
			const int32 Height = Heights[ColumnIndex(X, Y)];

			for (int32 Z = 0; Z <= Height; ++Z)
			{
				if (Cubes[CubeIndex(X, Y, Z)] != CubeType)
				{
					continue;
				}

				const FVector CellOrigin = FVector(X, Y, Z) * CubeSize;

				for (const Voxel::FFaceDef& Face : Voxel::FaceTable)
				{
					if (!IsAirAt(X + Face.Normal[0], Y + Face.Normal[1], Z + Face.Normal[2]))
					{
						continue;
					}

					const int32 BaseIndex = Vertices.Num();
					const FVector FaceNormal(Face.Normal[0], Face.Normal[1], Face.Normal[2]);

					for (int32 Corner = 0; Corner < Voxel::CornersPerFace; ++Corner)
					{
						const FVector CornerUnits(Face.Corners[Corner][0], Face.Corners[Corner][1], Face.Corners[Corner][2]);
						Vertices.Add(CornerUnits * CubeSize + CellOrigin);
						Normals.Add(FaceNormal);
						UVs.Add(FVector2D(Voxel::FaceUVs[Corner][0], Voxel::FaceUVs[Corner][1]));
					}

					for (const int32 WindingCorner : Voxel::QuadWinding)
					{
						Triangles.Add(BaseIndex + WindingCorner);
					}
				}
			}
		}
	}
}

uint8 AVoxelChunkBase::GetLocalCubeByte(const int32 X, const int32 Y, const int32 Z) const
{
	// Out-of-range reads are Air, like the Blueprint GetLocalCube.
	return IsInsideChunk(X, Y, Z) ? Cubes[CubeIndex(X, Y, Z)] : Voxel::CubeType::Air;
}

bool AVoxelChunkBase::SetLocalCubeByte(const int32 X, const int32 Y, const int32 Z, const uint8 Type)
{
	// Keeps Heights valid for placing: meshing only loops up to each column's Height.
	if (!IsInsideChunk(X, Y, Z))
	{
		return false;
	}

	Cubes[CubeIndex(X, Y, Z)] = Type;

	int32& Height = Heights[ColumnIndex(X, Y)];
	if (Type != Voxel::CubeType::Air && Z > Height)
	{
		Height = Z;
	}
	return true;
}

int32 AVoxelChunkBase::GetColumnHeight(const int32 X, const int32 Y) const
{
	// INDEX_NONE makes a bad lookup obvious instead of silently returning 0.
	return (IsInsideColumns(X, Y) && Heights.Num() == Voxel::ColumnCount) ? Heights[ColumnIndex(X, Y)] : INDEX_NONE;
}

int32 AVoxelChunkBase::CubeIndex(const int32 X, const int32 Y, const int32 Z)
{
	// Same layout as the Blueprint CellToIndex.
	return X + Y * Voxel::ChunkSize + Z * Voxel::ColumnCount;
}

int32 AVoxelChunkBase::ColumnIndex(const int32 X, const int32 Y)
{
	// Same layout as the Blueprint ColumnHeights index.
	return X + Y * Voxel::ChunkSize;
}

bool AVoxelChunkBase::IsInsideColumns(const int32 X, const int32 Y)
{
	// X/Y bounds only; Z is checked separately because it has its own rules.
	return X >= 0 && X < Voxel::ChunkSize && Y >= 0 && Y < Voxel::ChunkSize;
}

bool AVoxelChunkBase::IsInsideChunk(const int32 X, const int32 Y, const int32 Z) const
{
	// Full 3D bounds, also guarding against use before InitChunkData.
	return IsInsideColumns(X, Y) && Z >= 0 && Z < WorldHeight && Cubes.Num() == Voxel::ColumnCount * WorldHeight;
}

uint8 AVoxelChunkBase::GetTypeForCell(const int32 Z, const int32 SurfaceZ, const int32 TopDepth, const int32 SnowLine)
{
	// Same rule as the Blueprint GetTypeForCell.
	if (Z > SurfaceZ - TopDepth)
	{
		return SurfaceZ >= SnowLine ? Voxel::CubeType::Snow : Voxel::CubeType::Grass;
	}
	return Voxel::CubeType::Rock;
}

bool AVoxelChunkBase::IsAirAt(const int32 X, const int32 Y, const int32 Z) const
{
	// Below the world is solid (no underside faces); above it is open sky.
	if (Z < 0)
	{
		return false;
	}
	if (Z >= WorldHeight)
	{
		return true;
	}
	if (IsInsideColumns(X, Y))
	{
		return Cubes[CubeIndex(X, Y, Z)] == Voxel::CubeType::Air;
	}
	if (VoxelWorld)
	{
		const FIntVector WorldCell(ChunkCoord.X * Voxel::ChunkSize + X, ChunkCoord.Y * Voxel::ChunkSize + Y, Z);
		return VoxelWorld->GetCubeByte(WorldCell) == Voxel::CubeType::Air;
	}
	return true;
}
