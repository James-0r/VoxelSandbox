// Fill out your copyright notice in the Description page of Project Settings.

#include "VoxelChunkBase.h"

#include "ProceduralMeshComponent.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "VoxelTypes.h"
#include "VoxelWorldBase.h"

using VoxelGeneration::ColumnIndex;
using VoxelGeneration::CubeIndex;
using VoxelGeneration::IsInsideColumns;

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
	Data.Init(InWorldHeight);
	PaddedHeights.Reset();
	PaddedColumns = 0;
}

void AVoxelChunkBase::GenerateColumns(const FVoxelTerrainSettings& Terrain, const FVoxelCaveSettings& Caves, const FVector NoiseOffset)
{
	// The border is as wide as the caves look sideways, so CarveCaves never has to ask a neighbour chunk.
	TRACE_CPUPROFILER_EVENT_SCOPE(AVoxelChunkBase::GenerateColumns);

	if (!ensureMsgf(Data.IsInitialised(), TEXT("GenerateColumns: call InitChunkData first.")))
	{
		return;
	}

	FVoxelGenerationParams Params;
	Params.Terrain = Terrain;
	Params.NoiseOffset = NoiseOffset;
	Params.WorldHeight = Data.WorldHeight;

	PaddedColumns = VoxelGeneration::GetColumnPadding(Caves);
	VoxelGeneration::SampleColumns(ChunkCoord, PaddedColumns, Params, Data, PaddedHeights);
}

void AVoxelChunkBase::FillColumns()
{
	// Thin wrapper: the loop itself is shared with GenerateChunk.
	if (ensureMsgf(Data.IsInitialised(), TEXT("FillColumns: call InitChunkData first.")))
	{
		VoxelGeneration::FillColumns(Data);
	}
}

void AVoxelChunkBase::CarveCaves(const FVector NoiseOffset, const FVoxelCaveSettings& Caves, const int32 BedrockLayers,
	const int32 GrassDepthMax)
{
	// Clearance is measured down from the lowest surface within SurfaceMargin + SurfaceFade columns, so a cell
	// beside a cliff counts the open air next to it, not just the sky straight above.
	if (!ensureMsgf(Data.IsInitialised(), TEXT("CarveCaves: call InitChunkData first.")))
	{
		return;
	}

	// The border came from the noise in GenerateColumns, so no neighbour chunk is read here.
	const int32 Radius = VoxelGeneration::GetColumnPadding(Caves);
	if (!ensureMsgf(PaddedColumns >= Radius && !PaddedHeights.IsEmpty(),
		TEXT("CarveCaves: call GenerateColumns first (border %d columns, caves need %d)."), PaddedColumns, Radius))
	{
		return;
	}

	TArray<int32> LowestNearby;
	VoxelGeneration::ComputeLowestNearbyHeights(PaddedHeights, PaddedColumns, Radius, LowestNearby);
	VoxelGeneration::CarveCaves(ChunkCoord, NoiseOffset, Caves, BedrockLayers, GrassDepthMax, LowestNearby, Data);
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

	check(Data.IsInitialised());

	for (int32 X = 0; X < Voxel::ChunkSize; ++X)
	{
		for (int32 Y = 0; Y < Voxel::ChunkSize; ++Y)
		{
			const int32 Height = Data.Heights[ColumnIndex(X, Y)];

			for (int32 Z = 0; Z <= Height; ++Z)
			{
				if (Data.Cubes[CubeIndex(X, Y, Z)] != CubeType)
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

	check(Data.IsInitialised());

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
					const bool bVisible = Data.Cubes[CubeIndex(Cell[0], Cell[1], Cell[2])] == CubeType
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

bool AVoxelChunkBase::HasMesh() const
{
	// RebuildChunkMesh always creates at least one section (the bottom layer is solid), so 0 sections = not built.
	const UProceduralMeshComponent* Mesh = FindComponentByClass<UProceduralMeshComponent>();
	return Mesh && Mesh->GetNumSections() > 0;
}

uint8 AVoxelChunkBase::GetLocalCubeByte(const int32 X, const int32 Y, const int32 Z) const
{
	// Out-of-range reads are Air, like the Blueprint GetLocalCube.
	return IsInsideChunk(X, Y, Z) ? Data.Cubes[CubeIndex(X, Y, Z)] : Voxel::CubeType::Air;
}

bool AVoxelChunkBase::SetLocalCubeByte(const int32 X, const int32 Y, const int32 Z, const uint8 Type)
{
	// Keeps Heights valid for placing: meshing only loops up to each column's Height.
	if (!IsInsideChunk(X, Y, Z))
	{
		return false;
	}

	Data.Cubes[CubeIndex(X, Y, Z)] = Type;

	int32& Height = Data.Heights[ColumnIndex(X, Y)];
	if (Type != Voxel::CubeType::Air && Z > Height)
	{
		Height = Z;
	}
	return true;
}

int32 AVoxelChunkBase::GetColumnHeight(const int32 X, const int32 Y) const
{
	// INDEX_NONE makes a bad lookup obvious instead of silently returning 0.
	return (IsInsideColumns(X, Y) && Data.IsInitialised()) ? Data.Heights[ColumnIndex(X, Y)] : INDEX_NONE;
}

bool AVoxelChunkBase::IsInsideChunk(const int32 X, const int32 Y, const int32 Z) const
{
	// Full 3D bounds, also guarding against use before InitChunkData.
	return IsInsideColumns(X, Y) && Z >= 0 && Z < Data.WorldHeight && Data.IsInitialised();
}

bool AVoxelChunkBase::IsAirAt(const int32 X, const int32 Y, const int32 Z) const
{
	// Below the world is solid (no underside faces); above it is open sky.
	if (Z < 0)
	{
		return false;
	}
	if (Z >= Data.WorldHeight)
	{
		return true;
	}
	if (IsInsideColumns(X, Y))
	{
		return Data.Cubes[CubeIndex(X, Y, Z)] == Voxel::CubeType::Air;
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
	for (const int32 Height : Data.Heights)
	{
		TopZ = FMath::Max(TopZ, Height);
	}
	return TopZ;
}
