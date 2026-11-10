// Fill out your copyright notice in the Description page of Project Settings.

#include "VoxelChunkBase.h"

#include "Curves/CurveFloat.h"
#include "ProceduralMeshComponent.h"
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

	/**
	 * Cheese threshold at the inner edge of the surface fade. Perlin output stays below ~1, so a threshold of 1
	 * means "no cheese"; the fade blends from this down to the normal CaveCurve threshold.
	 */
	constexpr double CheeseThresholdAtMargin = 1.0;

	/** Marks a padded-grid column whose chunk doesn't exist (world rim); such columns are skipped. */
	constexpr int32 MissingColumnHeight = TNumericLimits<int32>::Max();

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

	// Surface margin: clearance is measured down from the lowest surface within this many columns, so a
	// cell beside a cliff counts the open air next to it, not just the sky straight above.
	const int32 SurfaceMargin = FMath::Max(Caves.SurfaceMargin, 0);
	const int32 SurfaceFade = FMath::Max(Caves.SurfaceFade, 0);
	TArray<int32> LowestNearby;
	ComputeLowestNearbyHeights(SurfaceMargin + SurfaceFade, LowestNearby);

	for (int32 X = 0; X < Voxel::ChunkSize; ++X)
	{
		const double SampleX = static_cast<double>(Voxel::ChunkSize * ChunkCoord.X + X) + NoiseOffset.X;

		for (int32 Y = 0; Y < Voxel::ChunkSize; ++Y)
		{
			const double SampleY = static_cast<double>(Voxel::ChunkSize * ChunkCoord.Y + Y) + NoiseOffset.Y;
			const int32 Column = ColumnIndex(X, Y);
			const int32 Height = Heights[Column];
			const int32 SurfaceZ = FMath::Min(Height, LowestNearby[Column]);

			// Existing depth rule, plus: no cell closer than SurfaceMargin to the nearby surface.
			const int32 LastZ = FMath::Min(Height - GrassDepthMax, SurfaceZ - SurfaceMargin);

			for (int32 Z = BedrockLayers; Z <= LastZ; ++Z)
			{
				const FVector Position(SampleX, SampleY, static_cast<double>(Z) + NoiseOffset.Z);

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

				// Width varies slowly along the tunnel, and shrinks to nothing towards the surface margin.
				const double WidthNoise = SampleCaveNoise(Position, TunnelWidthNoiseOffset, Caves.TunnelWidthFrequency);
				const double Width = FadeAlpha * FMath::GetMappedRangeValueClamped(
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

namespace
{
	/** Axis (0 = X, 1 = Y, 2 = Z) a face points along: the one non-zero component of its normal. */
	int32 GetNormalAxis(const Voxel::FFaceDef& Face)
	{
		return Face.Normal[0] != 0 ? 0 : (Face.Normal[1] != 0 ? 1 : 2);
	}

	/** Axis along which two corners of a face differ. Used to find which world axis each UV direction runs along. */
	int32 GetChangingAxis(const int32 (&CornerA)[3], const int32 (&CornerB)[3])
	{
		return CornerA[0] != CornerB[0] ? 0 : (CornerA[1] != CornerB[1] ? 1 : 2);
	}

	/** True when Width mask cells starting at (StartU, RowV) are all still visible and unmerged. */
	bool IsMaskRowFull(const TArray<bool>& Mask, const int32 SizeU, const int32 StartU, const int32 RowV, const int32 Width)
	{
		for (int32 OffsetU = 0; OffsetU < Width; ++OffsetU)
		{
			if (!Mask[StartU + OffsetU + RowV * SizeU])
			{
				return false;
			}
		}
		return true;
	}

	/** Sets a Width × Height block of the mask to false so those faces can't be merged twice. */
	void ClearMaskRect(TArray<bool>& Mask, const int32 SizeU, const int32 StartU, const int32 StartV, const int32 Width, const int32 Height)
	{
		for (int32 OffsetV = 0; OffsetV < Height; ++OffsetV)
		{
			for (int32 OffsetU = 0; OffsetU < Width; ++OffsetU)
			{
				Mask[StartU + OffsetU + (StartV + OffsetV) * SizeU] = false;
			}
		}
	}

	/**
	 * Adds one merged face. It's the face table's quad with each axis stretched by Extent (in cells), so the
	 * corner order, winding and normal are exactly those of a single-cube face, just bigger.
	 * UVs are stretched the same way: 0..Extent along the axis each UV direction runs along.
	 */
	void AddMergedQuad(const Voxel::FFaceDef& Face, const int32 (&Origin)[3], const int32 (&Extent)[3],
		const int32 UVAxisU, const int32 UVAxisV, const double CubeSize,
		TArray<FVector>& Vertices, TArray<int32>& Triangles, TArray<FVector>& Normals, TArray<FVector2D>& UVs)
	{
		const int32 BaseIndex = Vertices.Num();
		const FVector FaceNormal(Face.Normal[0], Face.Normal[1], Face.Normal[2]);

		for (int32 Corner = 0; Corner < Voxel::CornersPerFace; ++Corner)
		{
			const int32 (&CornerUnits)[3] = Face.Corners[Corner];
			Vertices.Add(FVector(
				Origin[0] + CornerUnits[0] * Extent[0],
				Origin[1] + CornerUnits[1] * Extent[1],
				Origin[2] + CornerUnits[2] * Extent[2]) * CubeSize);
			Normals.Add(FaceNormal);
			UVs.Add(FVector2D(Voxel::FaceUVs[Corner][0] * Extent[UVAxisU], Voxel::FaceUVs[Corner][1] * Extent[UVAxisV]));
		}

		for (const int32 WindingCorner : Voxel::QuadWinding)
		{
			Triangles.Add(BaseIndex + WindingCorner);
		}
	}
}

void AVoxelChunkBase::BuildSectionDataGreedy(const uint8 CubeType, const double CubeSize, TArray<FVector>& Vertices,
	TArray<int32>& Triangles, TArray<FVector>& Normals, TArray<FVector2D>& UVs) const
{
	// Per direction: slice the chunk into 2D layers, mark which cells in the layer show a face of this type
	// (same test as BuildSectionData), then cover those marks with as few rectangles as the greedy rule finds.
	TRACE_CPUPROFILER_EVENT_SCOPE(AVoxelChunkBase::BuildSectionDataGreedy);

	Vertices.Reset();
	Triangles.Reset();
	Normals.Reset();
	UVs.Reset();

	check(Cubes.Num() == Voxel::ColumnCount * WorldHeight);
	check(Heights.Num() == Voxel::ColumnCount);

	// Cells per axis (X, Y, Z). Z stops at the highest filled cube, since everything above it is Air.
	const int32 AxisSize[3] = { Voxel::ChunkSize, Voxel::ChunkSize, GetTopFilledZ() + 1 };

	// Reused for every slice: one entry per cell of the slice, true = visible face not merged yet.
	TArray<bool> Mask;

	for (const Voxel::FFaceDef& Face : Voxel::FaceTable)
	{
		// N is the axis the face points along (slices are stacked along it); U and V span each slice.
		const int32 AxisN = GetNormalAxis(Face);
		const int32 AxisU = (AxisN + 1) % 3;
		const int32 AxisV = (AxisN + 2) % 3;
		const int32 SizeU = AxisSize[AxisU];
		const int32 SizeV = AxisSize[AxisV];

		// Read from the face table so UVs follow the same directions as BuildSectionData's: the UV U direction
		// is the axis between corners 0 and 1 (UV 0,0 → 1,0), the UV V direction the one between corners 1 and 2.
		const int32 UVAxisU = GetChangingAxis(Face.Corners[0], Face.Corners[1]);
		const int32 UVAxisV = GetChangingAxis(Face.Corners[1], Face.Corners[2]);

		Mask.SetNumUninitialized(SizeU * SizeV);

		for (int32 Slice = 0; Slice < AxisSize[AxisN]; ++Slice)
		{
			// 1. Build the mask for this slice.
			int32 Cell[3];
			Cell[AxisN] = Slice;
			bool bSliceHasFaces = false;

			for (int32 MaskV = 0; MaskV < SizeV; ++MaskV)
			{
				Cell[AxisV] = MaskV;
				for (int32 MaskU = 0; MaskU < SizeU; ++MaskU)
				{
					Cell[AxisU] = MaskU;
					const bool bVisible = Cubes[CubeIndex(Cell[0], Cell[1], Cell[2])] == CubeType
						&& IsAirAt(Cell[0] + Face.Normal[0], Cell[1] + Face.Normal[1], Cell[2] + Face.Normal[2]);
					Mask[MaskU + MaskV * SizeU] = bVisible;
					bSliceHasFaces |= bVisible;
				}
			}

			if (!bSliceHasFaces)
			{
				continue;
			}

			// 2. Merge: scan for the first unmerged face, grow it along U, then along V, clear it, emit one quad.
			for (int32 MaskV = 0; MaskV < SizeV; ++MaskV)
			{
				for (int32 MaskU = 0; MaskU < SizeU;)
				{
					if (!Mask[MaskU + MaskV * SizeU])
					{
						++MaskU;
						continue;
					}

					int32 Width = 1;
					while (MaskU + Width < SizeU && Mask[MaskU + Width + MaskV * SizeU])
					{
						++Width;
					}

					int32 Height = 1;
					while (MaskV + Height < SizeV && IsMaskRowFull(Mask, SizeU, MaskU, MaskV + Height, Width))
					{
						++Height;
					}

					ClearMaskRect(Mask, SizeU, MaskU, MaskV, Width, Height);

					int32 Origin[3];
					Origin[AxisN] = Slice;
					Origin[AxisU] = MaskU;
					Origin[AxisV] = MaskV;

					int32 Extent[3];
					Extent[AxisN] = 1;
					Extent[AxisU] = Width;
					Extent[AxisV] = Height;

					AddMergedQuad(Face, Origin, Extent, UVAxisU, UVAxisV, CubeSize, Vertices, Triangles, Normals, UVs);

					MaskU += Width;
				}
			}
		}
	}
}

void AVoxelChunkBase::GetMeshStats(int32& VertexCount, int32& TriangleCount) const
{
	// Reads the component rather than the build calls, so it measures whatever mesher actually ran.
	VertexCount = 0;
	TriangleCount = 0;

	UProceduralMeshComponent* Mesh = FindComponentByClass<UProceduralMeshComponent>();
	if (!Mesh)
	{
		return;
	}

	for (int32 SectionIndex = 0; SectionIndex < Mesh->GetNumSections(); ++SectionIndex)
	{
		if (const FProcMeshSection* Section = Mesh->GetProcMeshSection(SectionIndex))
		{
			VertexCount += Section->ProcVertexBuffer.Num();
			TriangleCount += Section->ProcIndexBuffer.Num() / 3;
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

int32 AVoxelChunkBase::GetTopFilledZ() const
{
	// Heights only ever go up (fill, placing) and carving only makes Air, so no solid cube sits above this.
	int32 TopZ = 0;
	for (const int32 Height : Heights)
	{
		TopZ = FMath::Max(TopZ, Height);
	}
	return TopZ;
}

void AVoxelChunkBase::ComputeLowestNearbyHeights(const int32 Radius, TArray<int32>& OutLowest) const
{
	// Two passes: gather heights once into a grid padded by Radius on every side (one world lookup per
	// column, not per cell), then take the minimum over a circle around each of this chunk's columns.
	check(Heights.Num() == Voxel::ColumnCount);

	const int32 PaddedSize = Voxel::ChunkSize + 2 * Radius;
	TArray<int32> PaddedHeights;
	PaddedHeights.SetNumUninitialized(PaddedSize * PaddedSize);

	for (int32 PaddedY = 0; PaddedY < PaddedSize; ++PaddedY)
	{
		for (int32 PaddedX = 0; PaddedX < PaddedSize; ++PaddedX)
		{
			const int32 LocalX = PaddedX - Radius;
			const int32 LocalY = PaddedY - Radius;
			int32 ColumnHeight = MissingColumnHeight;

			if (IsInsideColumns(LocalX, LocalY))
			{
				ColumnHeight = Heights[ColumnIndex(LocalX, LocalY)];
			}
			else if (VoxelWorld)
			{
				const FIntVector WorldCell(ChunkCoord.X * Voxel::ChunkSize + LocalX, ChunkCoord.Y * Voxel::ChunkSize + LocalY, 0);
				if (const AVoxelChunkBase* Neighbour = VoxelWorld->FindChunk(AVoxelWorldBase::CellToChunkCoord(WorldCell)))
				{
					const FIntVector NeighbourLocal = AVoxelWorldBase::CellToLocal(WorldCell);
					const int32 NeighbourHeight = Neighbour->GetColumnHeight(NeighbourLocal.X, NeighbourLocal.Y);
					// INDEX_NONE = neighbour not initialised yet; treat it like a missing chunk.
					if (NeighbourHeight != INDEX_NONE)
					{
						ColumnHeight = NeighbourHeight;
					}
				}
			}

			PaddedHeights[PaddedX + PaddedY * PaddedSize] = ColumnHeight;
		}
	}

	OutLowest.SetNumUninitialized(Voxel::ColumnCount);
	const int32 RadiusSquared = Radius * Radius;

	for (int32 X = 0; X < Voxel::ChunkSize; ++X)
	{
		for (int32 Y = 0; Y < Voxel::ChunkSize; ++Y)
		{
			int32 Lowest = MissingColumnHeight;

			for (int32 OffsetY = -Radius; OffsetY <= Radius; ++OffsetY)
			{
				for (int32 OffsetX = -Radius; OffsetX <= Radius; ++OffsetX)
				{
					if (OffsetX * OffsetX + OffsetY * OffsetY > RadiusSquared)
					{
						continue;
					}
					const int32 PaddedIndex = (X + Radius + OffsetX) + (Y + Radius + OffsetY) * PaddedSize;
					Lowest = FMath::Min(Lowest, PaddedHeights[PaddedIndex]);
				}
			}

			// The column itself is always inside the circle, so Lowest is never left as "missing".
			OutLowest[ColumnIndex(X, Y)] = Lowest;
		}
	}
}
