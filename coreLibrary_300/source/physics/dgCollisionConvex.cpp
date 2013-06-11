/* Copyright (c) <2003-2011> <Julio Jerez, Newton Game Dynamics>
* 
* This software is provided 'as-is', without any express or implied
* warranty. In no event will the authors be held liable for any damages
* arising from the use of this software.
* 
* Permission is granted to anyone to use this software for any purpose,
* including commercial applications, and to alter it and redistribute it
* freely, subject to the following restrictions:
* 
* 1. The origin of this software must not be misrepresented; you must not
* claim that you wrote the original software. If you use this software
* in a product, an acknowledgment in the product documentation would be
* appreciated but is not required.
* 
* 2. Altered source versions must be plainly marked as such, and must not be
* misrepresented as being the original software.
* 
* 3. This notice may not be removed or altered from any source distribution.
*/

#include "dgPhysicsStdafx.h"
#include "dgBody.h"
#include "dgWorld.h"
#include "dgContact.h"
#include "dgCollisionMesh.h"
#include "dgMinkowskiConv.h"
#include "dgCollisionConvex.h"
#include "dgCollisionInstance.h"
#include "dgCollisionConvexHull.h"
#include "dgCollisionConvexPolygon.h"

#define DG_MAX_EDGE_COUNT				2048
#define DG_MAX_CIRCLE_DISCRETE_STEPS	dgFloat32 (256.0f)

#define DG_CLIP_MAX_COUNT				512
#define DG_CLIP_MAX_POINT_COUNT			64
#define DG_MAX_VERTEX_CLIP_FACE			16
#define DG_MAX_SIMD_VERTEX_LIMIT		64
#define DG_CONNICS_CONTATS_ITERATIONS	32
#define DG_SEPARATION_PLANES_ITERATIONS	8
#define DG_MAX_MIN_VOLUME				dgFloat32 (1.0e-4f)

#define DG_MINK_VERTEX_ERR				(dgFloat32 (1.0e-3f))
#define DG_MINK_VERTEX_ERR2				(DG_MINK_VERTEX_ERR * DG_MINK_VERTEX_ERR)

#define DG_ROBUST_PLANE_CLIP			dgFloat32 (1.0f / 256.0f)


#define DG_CONVEX_MINK_STACK_SIZE		64
#define DG_CONVEX_MINK_MAX_FACES		512
#define DG_CONVEX_MINK_MAX_POINTS		256


dgInt32 dgCollisionConvex::m_rayCastSimplex[4][4] = 
{
	{0, 1, 2, 3},
	{0, 2, 3, 1},
	{2, 1, 3, 0},
	{1, 0, 3, 2},
};



dgVector dgCollisionConvex::m_hullDirs[] = 
{
	dgVector(dgFloat32 (0.577350f), dgFloat32 (-0.577350f), dgFloat32 (0.577350f), dgFloat32 (0.0f)),
	dgVector(dgFloat32 (-0.577350f), dgFloat32 (-0.577350f), dgFloat32 (-0.577350f), dgFloat32 (0.0f)),
	dgVector(dgFloat32 (0.577350f), dgFloat32 (-0.577350f), dgFloat32 (-0.577350f), dgFloat32 (0.0f)),
	dgVector(dgFloat32 (-0.577350f), dgFloat32 (0.577350f), dgFloat32 (0.577350f), dgFloat32 (0.0f)),
	dgVector(dgFloat32 (0.577350f), dgFloat32 (0.577350f), dgFloat32 (-0.577350f), dgFloat32 (0.0f)),
	dgVector(dgFloat32 (-0.577350f), dgFloat32 (0.577350f), dgFloat32 (-0.577350f), dgFloat32 (0.0f)),
	dgVector(dgFloat32 (-0.577350f), dgFloat32 (-0.577350f), dgFloat32 (0.577350f), dgFloat32 (0.0f)),
	dgVector(dgFloat32 (0.577350f), dgFloat32 (0.577350f), dgFloat32 (0.577350f), dgFloat32 (0.0f)),
	dgVector(dgFloat32 (0.000000f), dgFloat32 (-1.000000f), dgFloat32 (0.000000f), dgFloat32 (0.0f)),
	dgVector(dgFloat32 (0.000000f), dgFloat32 (1.000000f), dgFloat32 (0.000000f), dgFloat32 (0.0f)),
	dgVector(dgFloat32 (1.000000f), dgFloat32 (0.000000f), dgFloat32 (0.000000f), dgFloat32 (0.0f)),
	dgVector(dgFloat32 (-1.000000f), dgFloat32 (0.000000f), dgFloat32 (0.000000f), dgFloat32 (0.0f)),
	dgVector(dgFloat32 (0.000000f), dgFloat32 (0.000000f), dgFloat32 (1.000000f), dgFloat32 (0.0f)),
	dgVector(dgFloat32 (0.000000f), dgFloat32 (0.000000f), dgFloat32 (-1.000000f), dgFloat32 (0.0f)),
};


dgVector dgCollisionConvex::m_dummySum[4];


DG_MSC_VECTOR_ALIGMENT
class dgCollisionConvex::dgPerimenterEdge
{
	public:
	const dgVector* m_vertex;
	dgPerimenterEdge* m_next;
	dgPerimenterEdge* m_prev;
} DG_GCC_VECTOR_ALIGMENT;



DG_MSC_VECTOR_ALIGMENT
class dgCollisionConvex::dgMinkFace
{
	public:
	dgPlane m_plane;
	dgMinkFace* m_twin[3];
	dgInt16 m_vertex[3];	
	dgInt8 m_mark;
	dgInt8 m_alive;
} DG_GCC_VECTOR_ALIGMENT;


DG_MSC_VECTOR_ALIGMENT
class dgCollisionConvex::dgMinkHull: public dgDownHeap<dgMinkFace *, dgFloat32>  
{
	class dgFaceFreeList 
	{
		public:
		dgFaceFreeList* m_next;
	};

	public:
	dgMinkHull(dgCollisionParamProxy& proxy)
		:dgDownHeap<dgMinkFace*, dgFloat32> (heapBuffer, sizeof (heapBuffer))
		,m_matrix(proxy.m_matrix)
		,m_scale(proxy.m_floatingCollision->GetScale())
		,m_invScale(proxy.m_referenceCollision->GetInvScale())
		,m_faceIndex(0)
		,m_vertexIndex(0)
		,m_contactJoint(proxy.m_contactJoint)
		,m_myShape((dgCollisionConvex*)proxy.m_referenceCollision->GetChildShape())
		,m_otherShape((dgCollisionConvex*)proxy.m_floatingCollision->GetChildShape())
		,m_proxy(&proxy)
		,m_freeFace(NULL)
	{
		dgAssert (m_proxy->m_referenceCollision->IsType(dgCollision::dgCollisionConvexShape_RTTI));
		dgAssert (m_proxy->m_floatingCollision->IsType (dgCollision::dgCollisionConvexShape_RTTI));
		dgAssert (m_contactJoint && m_contactJoint->GetId() == dgConstraint::m_contactConstraint);
	}

	DG_INLINE dgMinkFace* NewFace()
	{
		dgMinkFace* face = (dgMinkFace*)m_freeFace;
		if (m_freeFace) {
			m_freeFace = m_freeFace->m_next;
		} else {
			face = &m_facePool[m_faceIndex];
			m_faceIndex ++;
			dgAssert (m_faceIndex < DG_CONVEX_MINK_MAX_FACES);
		}

		#ifdef _DEBUG
		memset (face, 0, sizeof (dgMinkFace));
		#endif

		return face;
	}

	void DG_INLINE DeleteFace (dgMinkFace* const face)
	{
		dgFaceFreeList* const freeFace = (dgFaceFreeList*) face;
		freeFace->m_next = m_freeFace;
		m_freeFace = freeFace;
	}

	DG_INLINE dgMinkFace* AddFace(dgInt32 v0, dgInt32 v1, dgInt32 v2)
	{
		dgMinkFace* const face = NewFace();
		face->m_mark = 0;
		face->m_vertex[0] = dgInt16(v0); 
		face->m_vertex[1] = dgInt16(v1); 
		face->m_vertex[2] = dgInt16(v2); 
		return face;
	}

	DG_INLINE void PushFace(dgMinkFace* const face) 
	{
		dgInt32 i0 = face->m_vertex[0];
		dgInt32 i1 = face->m_vertex[1];
		dgInt32 i2 = face->m_vertex[2];

		dgPlane plane (m_hullDiff[i0], m_hullDiff[i1], m_hullDiff[i2]);
		dgFloat32 mag2 = plane % plane;
		face->m_alive = 1;
		if (mag2 > dgFloat32 (1.0e-16f)) {
			face->m_plane = plane.Scale(dgRsqrt (mag2));
			dgMinkFace* face1 = face;
			Push(face1, face->m_plane.m_w);
		} else {
			face->m_plane = dgPlane (dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f));
		}
	}

	void SaveOFF (const char* const fileName) const
	{
		FILE* const file = fopen (fileName, "wb");

		fprintf (file, "OFF\n");
		dgInt32 faceCount = 0;

		for (dgInt32 i = 0; i < m_faceIndex; i ++) {
			const dgMinkFace* const face = &m_facePool[i];
			if (face->m_alive) {
				faceCount ++;
			}
		}
		fprintf (file, "%d %d 0\n", m_vertexIndex, faceCount);
		for (dgInt32 i = 0; i < m_vertexIndex; i ++) {
			fprintf (file, "%f %f %f\n", m_hullDiff[i].m_x, m_hullDiff[i].m_y, m_hullDiff[i].m_z);
		}

		for (dgInt32 i = 0; i < m_faceIndex; i ++) {
			const dgMinkFace* const face = &m_facePool[i];
			if (face->m_alive) {
				fprintf (file, "3 %d %d %d\n", face->m_vertex[0], face->m_vertex[1], face->m_vertex[2]);
			}
		}
		fclose (file);
	}

	void SupportVertex (const dgVector& dir, dgInt32 vertexIndex)
	{
		dgAssert (dgAbsf (dir % dir - dgFloat32 (1.0f)) < dgFloat32 (1.0e-3f));
		dgVector p (m_myShape->ConvexConicSupporVertex(dir));

		dgVector dir1 (m_scale.CompProduct4(m_matrix.UnrotateVector (m_invScale.CompProduct4(dir.CompProduct4(dgVector::m_negOne)))));
		dgAssert (dir1.m_w == dgFloat32 (0.0f));

		//dir1 = dir1.Scale4 (dgRsqrt (dir1.DotProduct4(dir1).m_x));
		dir1 = dir1.CompProduct4(dir1.InvMagSqrt());
		dgAssert (dgAbsf(dir1 % dir1 - dgFloat32 (1.0f)) < dgFloat32 (1.0e-3f));

		dgVector q (m_invScale.CompProduct4(m_matrix.TransformVector (m_scale.CompProduct4 (m_otherShape->SupportVertex (dir1, &m_polygonFaceIndex[vertexIndex])))));

		m_hullDiff[vertexIndex] = p - q;
		m_hullSum[vertexIndex] = p + q;
	}


	static dgVector ReduceLine (dgInt32& indexOut, dgVector* const lineDiff, dgVector* const lineSum, dgInt32* const shapeFaceIndex)
	{
		const dgVector& p0 = lineDiff[0];
		const dgVector& p1 = lineDiff[1];
		dgVector dp (p1 - p0);
		dgVector v;

		dgFloat32 mag2 = dp % dp;
		if (mag2 < dgFloat32 (1.0e-24f)) {
			v = p0;
			indexOut = 1;
		} else {
			dgFloat32 alpha0 = - (p0 % dp) / mag2;
			if (alpha0 > dgFloat32 (1.0f)) {
				v = p1;
				indexOut = 1;
				lineSum[0] = lineSum[1];
				lineDiff[0] = lineDiff[1];
				shapeFaceIndex[0] = shapeFaceIndex[1];
			} else if (alpha0 < dgFloat32 (0.0f)) {
				v = p0;
				indexOut = 1;
			} else {
				v = p0 + dp.Scale3 (alpha0);
			}
		}
		return v;
	}

	static dgVector ReduceLineLarge (dgInt32& indexOut, dgVector* const lineDiff, dgVector* const lineSum, dgInt32* const shapeFaceIndex)
	{
		dgBigVector p0 (lineDiff[0]);
		dgBigVector p1 (lineDiff[1]);
		dgBigVector dp (p1 - p0);
		dgBigVector v;

		dgFloat64 mag2 = dp % dp;
		if (mag2 < dgFloat32 (1.0e-24f)) {
			v = p0;
			indexOut = 1;
		} else {
			dgFloat64 alpha0 = - (p0 % dp) / mag2;
			if (alpha0 > dgFloat32 (1.0f)) {
				v = p1;
				indexOut = 1;
				lineSum[0] = lineSum[1];
				lineDiff[0] = lineDiff[1];
				shapeFaceIndex[0] = shapeFaceIndex[1];
			} else if (alpha0 < dgFloat32 (0.0f)) {
				v = p0;
				indexOut = 1;
			} else {
				v = p0 + dp.Scale3 (alpha0);
			}
		}
		return v;
	}


	static void ReduceDegeneratedTriangle (dgVector& diff0, dgVector& sum0, dgVector& diff1, dgVector& sum1, const dgVector& diff2, const dgVector& sum2)
	{
		dgVector e10 (diff1 - diff0);
		if ((e10 % e10) < dgFloat32 (1.0e-12f)) {
			sum1 = sum2;
			diff1 = diff2;

			#ifdef _DEBUG
					} else {
						dgVector e21 (diff2 - diff1);
						dgVector e20 (diff2 - diff0);
						dgAssert (((e20 % e20) < dgFloat32 (1.0e-8f)) || ((e21 % e21) < dgFloat32 (1.0e-8f)));
			#endif
		}
	}

	
	static dgVector ReduceTriangle (dgInt32& indexOut, dgVector* const triangleDiff, dgVector* const triangleSum, dgInt32* const shapeFaceIndex)
	{
		dgVector e10 (triangleDiff[1] - triangleDiff[0]);
		dgVector e20 (triangleDiff[2] - triangleDiff[0]);
		dgVector normal (e10 * e20);
		if ((normal % normal) > dgFloat32 (1.0e-14f)) {
			dgInt32 i0 = 2;
			dgInt32 minIndex = -1;
			for (dgInt32 i1 = 0; i1 < 3; i1 ++) {
				const dgVector& p1p0 = triangleDiff[i0];
				const dgVector& p2p0 = triangleDiff[i1];

				dgFloat32 volume = (p1p0 * p2p0) % normal;
				if (volume < dgFloat32 (0.0f)) {
					dgVector segment (triangleDiff[i1] - triangleDiff[i0]);
					dgVector poinP0 (triangleDiff[i0].Scale3 (dgFloat32 (-1.0f)));
					dgFloat32 den = segment % segment;
					dgAssert (den > dgFloat32 (0.0f));
					dgFloat32 num = poinP0 % segment;
					if (num < dgFloat32 (0.0f)) {
						minIndex = i0;
					} else if (num > den){
						minIndex = i1;
					} else {
						indexOut = 2;
						dgVector tmp0 (triangleDiff[i0]);
						dgVector tmp1 (triangleDiff[i1]);
						triangleDiff[0] = tmp0;
						triangleDiff[1] = tmp1;

						tmp0 = triangleSum[i0];
						tmp1 = triangleSum[i1];
						triangleSum[0] = tmp0;
						triangleSum[1] = tmp1;

						i0 = shapeFaceIndex[i0];
						i1 = shapeFaceIndex[i1];
						shapeFaceIndex[0] = i0;
						shapeFaceIndex[1] = i1;
						return triangleDiff[0] + segment.Scale3 (num / den);
					}
				}
				i0 = i1;
			}
			if (minIndex != -1) {
				indexOut = 1;
				shapeFaceIndex[0] = shapeFaceIndex[minIndex];
				triangleSum[0] = triangleSum[minIndex];
				triangleDiff[0] = triangleDiff[minIndex];
				return triangleDiff[0];
			} else {
				indexOut = 3;
				return normal.Scale3((normal % triangleDiff[0]) / (normal % normal));
			}
		} else {
			indexOut = 2;
			ReduceDegeneratedTriangle (triangleDiff[0], triangleSum[0], triangleDiff[1], triangleSum[1], triangleDiff[2], triangleSum[2]);
			return ReduceLine (indexOut, &triangleDiff[0], &triangleSum[0], shapeFaceIndex);
		}
	}


	static dgVector ReduceTriangleLarge (dgInt32& indexOut, dgVector* const triangleDiff, dgVector* const triangleSum, dgInt32* const shapeFaceIndex)
	{
		dgBigVector triangleDiffExtended[3];
		triangleDiffExtended[0] = triangleDiff[0];
		triangleDiffExtended[1] = triangleDiff[1];
		triangleDiffExtended[2] = triangleDiff[2];

		dgBigVector e10 (triangleDiffExtended[1] - triangleDiffExtended[0]);
		dgBigVector e20 (triangleDiffExtended[2] - triangleDiffExtended[0]);
		dgBigVector normal (e10 * e20);
		if ((normal % normal) > dgFloat32 (1.0e-14f)) {
			dgInt32 i0 = 2;
			dgInt32 minIndex = -1;
			for (dgInt32 i1 = 0; i1 < 3; i1 ++) {
				dgBigVector p1p0 (triangleDiffExtended[i0]);
				dgBigVector p2p0 (triangleDiffExtended[i1]);

				dgFloat64 volume = (p1p0 * p2p0) % normal;
				if (volume < dgFloat32 (0.0f)) {
					dgBigVector segment (triangleDiffExtended[i1] - triangleDiffExtended[i0]);
					dgBigVector poinP0 (triangleDiffExtended[i0].Scale3 (dgFloat32 (-1.0f)));
					dgFloat64 den = segment % segment;
					dgAssert (den > dgFloat32 (0.0f));
					dgFloat64 num = poinP0 % segment;
					if (num < dgFloat32 (0.0f)) {
						minIndex = i0;
					} else if (num > den){
						minIndex = i1;
					} else {
						indexOut = 2;
						dgBigVector tmp0 (triangleDiffExtended[i0]);
						dgBigVector tmp1 (triangleDiffExtended[i1]);
						triangleDiffExtended[0] = tmp0;
						triangleDiffExtended[1] = tmp1;
						triangleDiff[0] = tmp0;
						triangleDiff[1] = tmp1;

						tmp0 = triangleSum[i0];
						tmp1 = triangleSum[i1];
						triangleSum[0] = tmp0;
						triangleSum[1] = tmp1;

						i0 = shapeFaceIndex[i0];
						i1 = shapeFaceIndex[i1];
						shapeFaceIndex[0] = i0;
						shapeFaceIndex[1] = i1;

						return dgVector (triangleDiffExtended[0] + segment.Scale3 (num / den));
					}
				}
				i0 = i1;
			}

			if (minIndex != -1) {
				indexOut = 1;
				shapeFaceIndex[0] = shapeFaceIndex[minIndex];
				triangleSum[0] = triangleSum[minIndex];
				triangleDiff[0] = dgVector (triangleDiff[minIndex]);
				//triangleDiffExtended[0] = triangleDiffExtended[minIndex];
				return triangleDiff[0];
			} else {
				indexOut = 3;
				return dgVector (normal.Scale3((normal % triangleDiffExtended[0]) / (normal % normal)));
			}
		} else {
			indexOut = 2;
			ReduceDegeneratedTriangle (triangleDiff[0], triangleSum[0], triangleDiff[1], triangleSum[1], triangleDiff[2], triangleSum[2]);
			return ReduceLineLarge (indexOut, &triangleDiff[0], &triangleSum[0], shapeFaceIndex);
		}
	}


	static dgVector ReduceTetrahedrum (dgInt32& indexOut, dgVector* const tetraDiff, dgVector* const tetraSum, dgInt32* const shapeFaceIndex)
	{
		dgInt32 i0 = m_rayCastSimplex[0][0];
		dgInt32 i1 = m_rayCastSimplex[0][1];
		dgInt32 i2 = m_rayCastSimplex[0][2];
		dgInt32 i3 = m_rayCastSimplex[0][3];
		const dgVector& p0 = tetraDiff[i0];
		const dgVector& p1 = tetraDiff[i1]; 
		const dgVector& p2 = tetraDiff[i2];
		const dgVector& p3 = tetraDiff[i3];

		dgVector p10 (p1 - p0);
		dgVector p20 (p2 - p0);
		dgVector p30 (p3 - p0);
		dgVector n (p10 * p20);
		dgFloat32 volume = p30 % n;
		if (volume < dgFloat32 (0.0f)) {
			volume = -volume;
			dgSwap (tetraSum[i2], tetraSum[i3]);
			dgSwap (tetraDiff[i2], tetraDiff[i3]);
			dgSwap (shapeFaceIndex[i2], shapeFaceIndex[i3]);
		}
		if (volume < dgFloat32 (1.0e-6f)) {
			dgTrace (("very import to finsh this\n"));
			//		dgAssert (0);
		}

		dgInt32 faceIndex = -1;
		dgFloat32 minDist = dgFloat32 (dgFloat32 (0.0f));
		const dgVector origin (dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f));
		for (dgInt32 i = 0; i < 4; i ++) {
			dgInt32 i0 = m_rayCastSimplex[i][0];
			dgInt32 i1 = m_rayCastSimplex[i][1];
			dgInt32 i2 = m_rayCastSimplex[i][2];

			const dgVector& p0 = tetraDiff[i0];
			const dgVector& p1 = tetraDiff[i1]; 
			const dgVector& p2 = tetraDiff[i2];

			dgVector p10 (p1 - p0);
			dgVector p20 (p2 - p0);
			dgVector normal (p10 * p20);

			dgFloat32 area = normal % normal;
			dgAssert (dgAbsf (area) > dgFloat32 (0.0f));
			normal = normal.Scale3 (dgRsqrt (area));
			dgFloat32 dist = normal % (origin - p0);
			if (dist <= minDist) {
				minDist = dist;
				faceIndex = i;
			}
		}

		if (faceIndex != -1) {
			dgVector tmp[3];
			dgInt32 i0 = m_rayCastSimplex[faceIndex][0];
			dgInt32 i1 = m_rayCastSimplex[faceIndex][1];
			dgInt32 i2 = m_rayCastSimplex[faceIndex][2];

			tmp[0] = tetraSum[i0];
			tmp[1] = tetraSum[i1];
			tmp[2] = tetraSum[i2];
			tetraSum[0] = tmp[0];
			tetraSum[1] = tmp[1];
			tetraSum[2] = tmp[2];

			tmp[0] = tetraDiff[i0];
			tmp[1] = tetraDiff[i1];
			tmp[2] = tetraDiff[i2];
			tetraDiff[0] = tmp[0];
			tetraDiff[1] = tmp[1];
			tetraDiff[2] = tmp[2];

			i0 = shapeFaceIndex[i0];
			i1 = shapeFaceIndex[i1];
			i2 = shapeFaceIndex[i2];
			shapeFaceIndex[0] = i0;
			shapeFaceIndex[1] = i1;
			shapeFaceIndex[2] = i2;

			indexOut = 3;
			return ReduceTriangle (indexOut, tetraDiff, tetraSum, shapeFaceIndex);
		}

		indexOut = 4;
		return origin;
	}

	static dgVector ReduceTetrahedrumLarge (dgInt32& indexOut, dgVector* const tetraDiff, dgVector* const tetraSum, dgInt32* const shapeFaceIndex)
	{
		dgBigVector tetraDiffExtended[4];
		tetraDiffExtended[0] = tetraDiff[0];
		tetraDiffExtended[1] = tetraDiff[1];
		tetraDiffExtended[2] = tetraDiff[2];
		tetraDiffExtended[3] = tetraDiff[3];

		dgInt32 i0 = m_rayCastSimplex[0][0];
		dgInt32 i1 = m_rayCastSimplex[0][1];
		dgInt32 i2 = m_rayCastSimplex[0][2];
		dgInt32 i3 = m_rayCastSimplex[0][3];
		const dgBigVector& p0 = tetraDiffExtended[i0];
		const dgBigVector& p1 = tetraDiffExtended[i1]; 
		const dgBigVector& p2 = tetraDiffExtended[i2];
		const dgBigVector& p3 = tetraDiffExtended[i3];

		dgBigVector p10 (p1 - p0);
		dgBigVector p20 (p2 - p0);
		dgBigVector p30 (p3 - p0);
		dgFloat64 volume = p30 % (p10 * p20);
		if (volume < dgFloat32 (0.0f)) {
			volume = -volume;
			dgSwap (tetraSum[i2], tetraSum[i3]);
			dgSwap (tetraDiff[i2], tetraDiff[i3]);
			dgSwap (shapeFaceIndex[i2], shapeFaceIndex[i3]);
			dgSwap (tetraDiffExtended[i2], tetraDiffExtended[i3]);
		}
		if (volume < dgFloat32 (1.0e-8f)) {
			dgAssert (0);
		}

		dgInt32 faceIndex = -1;
		dgFloat64 minDist = dgFloat32 (0.0f);
		const dgBigVector origin (dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f));
		for (dgInt32 i = 0; i < 4; i ++) {
			dgInt32 i0 = m_rayCastSimplex[i][0];
			dgInt32 i1 = m_rayCastSimplex[i][1];
			dgInt32 i2 = m_rayCastSimplex[i][2];

			const dgBigVector& p0 = tetraDiffExtended[i0];
			const dgBigVector& p1 = tetraDiffExtended[i1]; 
			const dgBigVector& p2 = tetraDiffExtended[i2];

			dgBigVector p10 (p1 - p0);
			dgBigVector p20 (p2 - p0);
			dgBigVector normal (p10 * p20);
			dgFloat64 area = normal % normal;
			dgAssert (fabs (area) > dgFloat32 (0.0f));
			normal = normal.Scale3 (dgFloat32 (1.0f) / sqrt (area));
			dgFloat64 dist = normal % (origin - p0);
			if (dist < minDist) {
				minDist = dist;
				faceIndex = i;
			}
		}

		if (faceIndex != -1) {
			dgVector tmp[3];
			dgInt32 i0 = m_rayCastSimplex[faceIndex][0];
			dgInt32 i1 = m_rayCastSimplex[faceIndex][1];
			dgInt32 i2 = m_rayCastSimplex[faceIndex][2];

			tmp[0] = tetraSum[i0];
			tmp[1] = tetraSum[i1];
			tmp[2] = tetraSum[i2];
			tetraSum[0] = tmp[0];
			tetraSum[1] = tmp[1];
			tetraSum[2] = tmp[2];

			tmp[0] = tetraDiff[i0];
			tmp[1] = tetraDiff[i1];
			tmp[2] = tetraDiff[i2];
			tetraDiff[0] = tmp[0];
			tetraDiff[1] = tmp[1];
			tetraDiff[2] = tmp[2];

			i0 = shapeFaceIndex[i0];
			i1 = shapeFaceIndex[i1];
			i2 = shapeFaceIndex[i2];
			shapeFaceIndex[0] = i0;
			shapeFaceIndex[1] = i1;
			shapeFaceIndex[2] = i2;

			indexOut = 3;
			return ReduceTriangleLarge (indexOut, tetraDiff, tetraSum, shapeFaceIndex);
		}

		indexOut = 4;
		return origin;
	}

	dgInt32 CalculateClosestSimplex ()
	{
		dgInt32 index = 1;
		SupportVertex (m_contactJoint->m_separtingVector, 0);
		dgVector v (m_hullDiff[0]);

		dgInt32 iter = 0;
		dgInt32 cycling = 0;
		dgFloat32 minDist = dgFloat32 (1.0e20f);
		do {

			dgFloat32 dist = v % v;
			if (dist < dgFloat32 (1.0e-9f)) {
				// very deep penetration, resolve with generic minkosky solver
				return -index; 
			}

			if (dist < minDist) {
				minDist = dist;
				cycling = -1;
			}
			cycling ++;
			if (cycling > 4) {
				// for now return -1
				return -index;
			}

			dgVector dir (v.Scale3 (-dgRsqrt(dist)));
			SupportVertex (dir, index);

			const dgVector& w = m_hullDiff[index];
			dgVector wv (w - v);
			dist = dir % wv;
			if (dist < dgFloat32 (1.0e-3f)) {
				m_normal = dir;
				break;
			}


			index ++;
			switch (index) 
			{
				case 2:
				{
					v = ReduceLine (index, m_hullDiff, m_hullSum, m_polygonFaceIndex);
					break;
				}

				case 3:
				{
					v = ReduceTriangle (index, m_hullDiff, m_hullSum, m_polygonFaceIndex);
					break;
				}

				case 4:
				{
					v = ReduceTetrahedrum (index, m_hullDiff, m_hullSum, m_polygonFaceIndex);
					break;
				}
			}

			iter ++;
			cycling ++;
		} while (iter < DG_CONNICS_CONTATS_ITERATIONS); 
		return (index < 4) ? index : -4;
	}

	dgInt32 CalculateClosestSimplexLarge ()
	{
		dgInt32 index = 1;
		SupportVertex (m_contactJoint->m_separtingVector, 0);
		dgVector v (m_hullDiff[0]);

		dgInt32 iter = 0;
		dgInt32 cycling = 0;
		dgFloat32 minDist = dgFloat32 (1.0e20f);
		do {

			dgFloat32 dist = v % v;
			if (dist < dgFloat32 (1.0e-9f)) {
				// very deep penetration, resolve with generic minkosky solver
				return -index; 
			}

			if (dist < minDist) {
				minDist = dist;
				cycling = -1;
			}
			cycling ++;
			if (cycling > 4) {
				// for now return -1
				return -index;
			}

			dgVector dir (v.Scale3 (-dgRsqrt(dist)));
			SupportVertex (dir, index);

			const dgVector& w = m_hullDiff[index];
			dgVector wv (w - v);
			dist = dir % wv;
			if (dist < dgFloat32 (1.0e-3f)) {
				m_normal = dir;
				break;
			}


			index ++;
			switch (index) 
			{
				case 2:
				{
					v = ReduceLineLarge (index, m_hullDiff, m_hullSum, m_polygonFaceIndex);
					break;
				}

				case 3:
				{
					v = ReduceTriangleLarge (index, m_hullDiff, m_hullSum, m_polygonFaceIndex);
					break;
				}

				case 4:
				{
					v = ReduceTetrahedrumLarge (index, m_hullDiff, m_hullSum, m_polygonFaceIndex);
					break;
				}
			}

			iter ++;
			cycling ++;
		} while (iter < DG_CONNICS_CONTATS_ITERATIONS); 
		return (index < 4) ? index : -4;
	}


	void CalculateContactFromFeacture(dgInt32 featureType)
	{
		dgVector d;
		dgVector s;
		switch (featureType)
		{
			case 1:
			{
				s = m_hullSum[0];
				d = m_hullDiff[0];
				break;
			}
			case 2:
			{
				const dgVector& p0 = m_hullDiff[0];
				const dgVector& p1 = m_hullDiff[1];
				dgVector dp (p1 - p0);
				dgAssert (dp % dp > dgFloat32 (0.0f));
				dgFloat32 alpha0 = - (p0 % dp) / (dp % dp);
				dgAssert (alpha0 <= dgFloat32 (1.01f));
				dgAssert (alpha0 >= dgFloat32 (-0.01f));
				d = p0 + dp.Scale3 (alpha0);
				s = m_hullSum[0] + (m_hullSum[1] - m_hullSum[0]).Scale3 (alpha0);
				break;
			}

			case 3:
			default:
			{
				dgVector e10 (m_hullDiff[1] - m_hullDiff[0]);
				dgVector e20 (m_hullDiff[2] - m_hullDiff[0]);
				dgVector normal (e10 * e20);
				dgAssert ((normal % normal) > dgFloat32 (0.0f));

				dgInt32 i0 = 2;
				dgFloat32 alphas[3];
				for (dgInt32 i1 = 0; i1 < 3; i1 ++) {
					const dgVector& p1p0 = m_hullDiff[i0];
					const dgVector& p2p0 = m_hullDiff[i1];
					alphas[i0] = (p1p0 * p2p0) % normal;
					i0 = i1;
				}

				dgFloat32 alphaDen = alphas[0] + alphas[1] + alphas[2];
				if (alphaDen > dgFloat32 (1.0e-16f)) {
					dgAssert (alphaDen > dgFloat32 (0.0f));

					alphaDen = dgFloat32 (1.0f / alphaDen);
					alphas[0] *= alphaDen;
					alphas[1] *= alphaDen;
					alphas[2] *= alphaDen;
					s = m_hullSum[0].Scale3(alphas[1]) + m_hullSum[1].Scale3(alphas[2]) + m_hullSum[2].Scale3 (alphas[0]);
					d = m_hullDiff[0].Scale3(alphas[1]) + m_hullDiff[1].Scale3(alphas[2]) + m_hullDiff[2].Scale3 (alphas[0]);
				} else {
					// this is a degenerated face that is so small that lose accuracy in 32 bit floats
					// get the closest point from the longest edge

					dgVector dir (((e10 % e10) > (e20 % e20)) ? e10 : e20);
					dgInt32 i0 = 0;
					dgInt32 i1 = 0;
					dgFloat32 dist0 = dir % m_hullDiff[0];
					dgFloat32 dist1 = -dist0;
					for (dgInt32 i = 1; i < 3; i ++) {
						dgFloat32 test = dir % m_hullDiff[i];
						if (test > dist0) {
							i0 = i;
							dist0 = test;
						}
						test *= dgFloat32 (-1.0f);					
						if (test > dist1) {
							i1 = i;
							dist1 = test;
						}
					}
					
					if (i0 != i1) {
						const dgVector& p0 = m_hullDiff[i0];
						const dgVector& p1 = m_hullDiff[i1];
						dgVector dp (p1 - p0);
						dgAssert (dp % dp > dgFloat32 (0.0f));
						dgFloat32 alpha0 = - (p0 % dp) / (dp % dp);
						dgAssert (alpha0 <= dgFloat32 (1.01f));
						dgAssert (alpha0 >= dgFloat32 (-0.01f));
						d = p0 + dp.Scale3 (alpha0);
						s = m_hullSum[0] + (m_hullSum[i1] - m_hullSum[i0]).Scale3 (alpha0);
					} else {
						s = m_hullSum[i0];
						d = m_hullDiff[i0];
					}

				}
			}
			break;
		}

		m_p = (s + d).Scale3 (dgFloat32 (0.5f));
		m_q = (s - d).Scale3 (dgFloat32 (0.5f));
		dgAssert (dgAbsf (m_normal % m_normal - dgFloat32 (1.0f)) < dgFloat32 (1.0e-4f)) ;
		m_contactJoint->m_separtingVector = m_normal;
	}

	void CalculateClosestPoints ()
	{
		dgCollisionInstance* const collConvexInstance = m_proxy->m_floatingCollision;
		dgCollisionInstance* const collConicConvexInstance = m_proxy->m_referenceCollision;
		dgAssert (collConvexInstance->m_childShape == m_otherShape);
		dgAssert (collConicConvexInstance->m_childShape == m_myShape);
		dgAssert (collConicConvexInstance->IsType (dgCollision::dgCollisionConvexShape_RTTI));

		dgInt32 simplexPointCount = 0;
		dgFloat32 radiusA = m_myShape->GetBoxMaxRadius() * collConicConvexInstance->m_maxScale.m_x;
		dgFloat32 radiusB = m_otherShape->GetBoxMaxRadius() * collConvexInstance->m_maxScale.m_x;
		if ((radiusA * dgFloat32 (8.0f) > radiusB) && (radiusB * dgFloat32 (8.0f) > radiusA)) {
			simplexPointCount = CalculateClosestSimplex ();
			if (simplexPointCount < 0) {
				simplexPointCount = CalculateIntersectingPlane (-simplexPointCount);
			}
		} else {
			simplexPointCount = CalculateClosestSimplexLarge();
			if (simplexPointCount < 0) {
				simplexPointCount = CalculateIntersectingPlane (-simplexPointCount);
			}
		}
		dgAssert ((simplexPointCount > 0) && (simplexPointCount <= 3));

		if (m_otherShape->GetCollisionPrimityType() == m_polygonCollision) {
			dgCollisionConvexPolygon* const polygonShape = (dgCollisionConvexPolygon*)m_otherShape;
			polygonShape->SetFeatureHit (simplexPointCount, m_polygonFaceIndex);
		}
		
		CalculateContactFromFeacture(simplexPointCount);
	}

	bool SanityCheck()
	{
		for (dgInt32 i = 0; i < m_faceIndex; i ++) {
			dgMinkFace* const face = &m_facePool[i];
			if (face->m_alive) {
				for (dgInt32 j = 0; j < 3; j ++) {
					dgMinkFace* const twin = face->m_twin[j];
					if (!twin) {
						return false;
					}

					if (!twin->m_alive) {
						return false;
					}
					
					bool pass = false;
					for (dgInt32 k = 0; k < 3; k ++) {
						if (twin->m_twin[k] == face) {
							pass = true;
							break;
						}
					}
					if (!pass) {
						return pass;
					}
				}
			}
		}
		return true;
	}

	dgInt32 CalculateIntersectingPlane (dgInt32 count)
	{
		dgAssert (count >= 1);
		if (count == 1) {
			SupportVertex (m_contactJoint->m_separtingVector.Scale3 (dgFloat32 (-1.0f)), 1);
			dgVector err (m_hullDiff[1] - m_hullDiff[0]);
			dgAssert ((err % err) > dgFloat32 (0.0f));
			count = 2;

		}
		if (count == 2) {
			dgVector e0 (m_hullDiff[1] - m_hullDiff[0]);
			dgAssert ((e0 % e0) > dgFloat32 (0.0f));
			dgMatrix matrix (e0.Scale3(dgRsqrt(e0 % e0)));
			dgMatrix rotation (dgPitchMatrix(dgFloat32 (45.0f * 3.141592f/180.0f)));
			dgFloat32 maxVolume = dgFloat32 (0.0f);
			for (dgInt32 i = 0; i < 8; i ++) {
				SupportVertex (matrix[1], 3);
				dgVector e1 (m_hullDiff[3] - m_hullDiff[0]);
				dgVector area (e0 * e1);
				dgFloat32 area2 = area % area;
				if (area2 > maxVolume) {
					m_hullSum[2] = m_hullSum[3];
					m_hullDiff[2] = m_hullDiff[3];
					maxVolume = area2;
				}
				matrix = rotation * matrix;
			}
			dgAssert (maxVolume > dgFloat32 (0.0f));
			count ++;
		}

		dgFloat32 volume = dgFloat32 (0.0f);
		if (count == 3) {
			dgVector e10 (m_hullDiff[1] - m_hullDiff[0]);
			dgVector e20 (m_hullDiff[2] - m_hullDiff[0]);
			dgVector normal (e10 * e20);
			dgFloat32 mag2 = normal % normal;
			dgAssert (mag2 > dgFloat32 (0.0f));
			normal = normal.Scale3(dgRsqrt(mag2));
			SupportVertex (normal, 3);
			volume = (m_hullDiff[3] - m_hullDiff[0]) % normal;
			if (dgAbsf(volume) < dgFloat32 (1.0e-10f)) {
				normal = normal.Scale3 (dgFloat32 (-1.0f));
				SupportVertex (normal, 3);
				volume = -((m_hullDiff[3] - m_hullDiff[0]) % normal);
				if (dgAbsf(volume) < dgFloat32 (1.0e-10f)) {
					// the simple is wrong, try building a simple by a different starting point, for now just assert
					dgAssert (0);
				}
			}
			count = 4;
		} else if (count == 4)  {
			dgVector e0 (m_hullDiff[1] - m_hullDiff[0]);
			dgVector e1 (m_hullDiff[2] - m_hullDiff[0]);
			dgVector e2 (m_hullDiff[3] - m_hullDiff[0]);
			dgVector n (e1 * e2);
			volume = e0 % n;
		}

		dgAssert (count == 4);
		if (volume > dgFloat32 (0.0f)) {
			dgSwap (m_hullSum[1], m_hullSum[0]);
			dgSwap (m_hullDiff[1], m_hullDiff[0]);
			dgSwap (m_polygonFaceIndex[1], m_polygonFaceIndex[0]);
		}


		if (dgAbsf(volume) < dgFloat32 (1e-15f)) {
			// this volume is unrealizable, let us build  a different tetrahedron using the method of core 200
			dgVector e1;
			dgVector e2;
			dgVector e3;
			dgVector normal (dgFloat32 (0.0f));

			const dgInt32 nCount = dgInt32(sizeof(dgCollisionConvex::m_hullDirs) / sizeof(dgCollisionConvex::m_hullDirs[0]));
			const dgFloat32 DG_CALCULATE_SEPARATING_PLANE_ERROR = dgFloat32 (1.0f / 1024.0f);

			dgFloat32 error2 = dgFloat32 (0.0f);
			SupportVertex (dgCollisionConvex::m_hullDirs[0], 0);

			dgInt32 i = 1;
			for (; i < nCount; i ++) {
				SupportVertex (dgCollisionConvex::m_hullDirs[i], 1);
				e1 = m_hullDiff[1] - m_hullDiff[0];
				error2 = e1 % e1;
				if (error2 > DG_CALCULATE_SEPARATING_PLANE_ERROR) {
					break;
				}
			}

			for (i ++; i < nCount; i ++) {
				SupportVertex (dgCollisionConvex::m_hullDirs[i], 2);
				e2 = m_hullDiff[2] - m_hullDiff[0];
				normal = e1 * e2;
				error2 = normal % normal;
				if (error2 > DG_CALCULATE_SEPARATING_PLANE_ERROR) {
					break;
				}
			}

			error2 = dgFloat32 (0.0f);
			for (i ++; i < nCount; i ++) {
				SupportVertex (dgCollisionConvex::m_hullDirs[i], 3);
				e3 = m_hullDiff[3] - m_hullDiff[0];
				error2 = normal % e3;
				if (dgAbsf (error2) > DG_CALCULATE_SEPARATING_PLANE_ERROR) {
					break;
				}
			}

			if (i >= nCount) {
				dgAssert (0);
				return -1;
			}

			if (error2 > dgFloat32 (0.0f)) {
				dgSwap (m_hullSum[1], m_hullSum[2]);
				dgSwap (m_hullDiff[1], m_hullDiff[2]);
				dgSwap (m_polygonFaceIndex[1], m_polygonFaceIndex[2]);
			}

			#ifdef _DEBUG
			{
				dgVector e0 (m_hullDiff[1] - m_hullDiff[0]);
				dgVector e1 (m_hullDiff[2] - m_hullDiff[0]);
				dgVector e2 (m_hullDiff[3] - m_hullDiff[0]);
				dgVector n (e1 * e2);
				dgFloat32 volume = e0 % n;
				dgAssert (volume < dgFloat32 (0.0f));
			}
			#endif

		}

		// clear the face cache!!
		m_faceIndex = 0;
		m_vertexIndex = 4;
		m_freeFace = NULL;

		dgMinkFace* const f0 = AddFace (0, 1, 2);
		dgMinkFace* const f1 = AddFace (0, 2, 3);
		dgMinkFace* const f2 = AddFace (2, 1, 3);
		dgMinkFace* const f3 = AddFace (1, 0, 3);

		f0->m_twin[0] = f3; 
		f0->m_twin[1] = f2; 
		f0->m_twin[2] = f1;

		f1->m_twin[0] = f0; 
		f1->m_twin[1] = f2; 
		f1->m_twin[2] = f3;

		f2->m_twin[0] = f0; 
		f2->m_twin[1] = f3; 
		f2->m_twin[2] = f1;

		f3->m_twin[0] = f0; 
		f3->m_twin[1] = f1; 
		f3->m_twin[2] = f2;

		PushFace(f0);
		PushFace(f1);
		PushFace(f2);
		PushFace(f3);

		dgFloat32 resolutionScale = dgFloat32 (0.125f);
		dgFloat32 minTolerance = dgFloat32 (DG_IMPULSIVE_CONTACT_PENETRATION / dgFloat32 (4.0f));
		while (GetCount()) {

			dgMinkFace* const faceNode = (*this)[0];
			Pop();	

			if (faceNode->m_alive) {

//SaveOFF ("xxx.off");

				SupportVertex (faceNode->m_plane, m_vertexIndex);
				const dgVector& p = m_hullDiff[m_vertexIndex];
				dgFloat32 dist = faceNode->m_plane.Evalue (p);
				dgFloat32 distTolerance = dgMax (dgAbsf(faceNode->m_plane.m_w) * resolutionScale, minTolerance);

				if (dist < distTolerance) {
//if (xxx > 1000){
//	SaveOFF ("xxx.off");
//}
					m_normal = faceNode->m_plane;
					dgVector sum[3];
					dgVector diff[3];
					dgInt32 index[3];
					for (dgInt32 i = 0; i < 3; i ++) {
						dgInt32 j = faceNode->m_vertex[i];
						sum[i] = m_hullSum[j];
						diff[i] = m_hullDiff[j];
						index[i] = m_polygonFaceIndex[j];
					}
					for (dgInt32 i = 0; i < 3; i ++) {
						m_hullSum[i] = sum[i];
						m_hullDiff[i] = diff[i];
						m_polygonFaceIndex[i] = index[i];
					}
					return 3;
				}

				//dgAssert (Sanity());
				m_faceStack[0] = faceNode;
				dgInt32 stackIndex = 1;
				dgInt32 deletedCount = 0;

				while (stackIndex) {
					stackIndex --;
					dgMinkFace* const face = m_faceStack[stackIndex];

					if (!face->m_mark && (face->m_plane.Evalue(p) > dgFloat32(0.0f))) { 
						#ifdef _DEBUG
							for (dgInt32 i = 0; i < deletedCount; i ++) {
								dgAssert (m_deletedFaceList[i] != face);
							}
						#endif

						m_deletedFaceList[deletedCount] = face;
						deletedCount ++;
						dgAssert (deletedCount < sizeof (m_deletedFaceList)/sizeof (m_deletedFaceList[0]));
						face->m_mark = 1;

						for (dgInt32 i = 0; i < 3; i ++) {
							dgMinkFace* const twinFace = face->m_twin[i];
							if (!twinFace->m_mark) {
								m_faceStack[stackIndex] = twinFace;
								stackIndex ++;
								dgAssert (stackIndex < sizeof (m_faceStack)/sizeof (m_faceStack[0]));
							}
						}
					}
				}

				//dgAssert (SanityCheck());
				dgInt32 newCount = 0;
				for (dgInt32 i = 0; i < deletedCount; i ++) {
					dgMinkFace* const face = m_deletedFaceList[i];
					face->m_alive = 0;
					dgAssert (face->m_mark == 1);
					dgInt32 j0 = 2;
					for (dgInt32 j1 = 0; j1 < 3; j1 ++) {
						dgMinkFace* const twinFace = face->m_twin[j0];
						if (!twinFace->m_mark) {
							dgMinkFace* const newFace = AddFace (m_vertexIndex, face->m_vertex[j0], face->m_vertex[j1]);
							PushFace(newFace);

							newFace->m_twin[1] = twinFace;
							dgInt32 index = (twinFace->m_twin[0] == face) ? 0 : ((twinFace->m_twin[1] == face) ? 1 : 2);
							twinFace->m_twin[index] = newFace;

							m_coneFaceList[newCount] = newFace;
							newCount ++;
							dgAssert (newCount < sizeof (m_coneFaceList)/sizeof (m_coneFaceList[0]));
						}
						j0 = j1;
					}
				}

				dgInt32 i0 = newCount - 1;
				for (dgInt32 i1 = 0; i1 < newCount; i1 ++) {
					dgMinkFace* const faceA = m_coneFaceList[i0];
					dgAssert (faceA->m_mark == 0);

					dgInt32 j0 = newCount - 1;
					for (dgInt32 j1 = 0; j1 < newCount; j1 ++) {
						if (i0 != j0) {
							dgMinkFace* const faceB = m_coneFaceList[j0];
							dgAssert (faceB->m_mark == 0);
							if (faceA->m_vertex[2] == faceB->m_vertex[1]) {
								faceA->m_twin[2] = faceB;
								faceB->m_twin[0] = faceA;
								break;
							}
						}
						j0 = j1;
					}
					i0 = i1;
				}

				m_vertexIndex ++;
				dgAssert (m_vertexIndex < sizeof (m_hullDiff)/sizeof (m_hullDiff[0]));

				dgAssert (SanityCheck());
			} else {
				DeleteFace(faceNode);
			}
		}
		return -1;
	}


	dgMatrix m_matrix;
	dgVector m_scale;
	dgVector m_invScale;
	dgVector m_p;
	dgVector m_q;
	dgVector m_normal;
	
	dgInt32 m_faceIndex;
	dgInt32 m_vertexIndex;
	dgContact* m_contactJoint;
	dgCollisionConvex* m_myShape;
	dgCollisionConvex* m_otherShape;
	dgCollisionParamProxy* m_proxy;
	dgFaceFreeList* m_freeFace; 

	dgMinkFace* m_faceStack[DG_CONVEX_MINK_STACK_SIZE];
	dgMinkFace* m_coneFaceList[DG_CONVEX_MINK_STACK_SIZE];
	dgMinkFace* m_deletedFaceList[DG_CONVEX_MINK_STACK_SIZE];
	dgInt32 m_polygonFaceIndex[DG_CONVEX_MINK_MAX_POINTS];
	dgVector m_hullDiff[DG_CONVEX_MINK_MAX_POINTS];
	dgVector m_hullSum[DG_CONVEX_MINK_MAX_POINTS];
	dgMinkFace m_facePool[DG_CONVEX_MINK_MAX_FACES];
	dgInt8 heapBuffer[DG_CONVEX_MINK_MAX_FACES * (sizeof (dgFloat32) + sizeof (dgMinkFace *))];
};




//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

dgCollisionConvex::dgCollisionConvex (dgMemoryAllocator* const allocator, dgUnsigned32 signature, dgCollisionID id)
	:dgCollision(allocator, signature, id) 
	,m_size_x (dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f))
	,m_size_y (dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f))
	,m_size_z (dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f))
	,m_userData (NULL)
	,m_vertex (NULL)
	,m_simplex (NULL)
	,m_boxMinRadius (dgFloat32 (0.0f))
	,m_boxMaxRadius (dgFloat32 (0.0f))
	,m_simplexVolume (dgFloat32 (0.0f))
	,m_edgeCount (0)
	,m_vertexCount (0)
{
	m_rtti |= dgCollisionConvexShape_RTTI;
}


dgCollisionConvex::dgCollisionConvex (dgWorld* const world, dgDeserialize deserialization, void* const userData)
	:dgCollision (world, deserialization, userData)
	,m_size_x (dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f))
	,m_size_y (dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f))
	,m_size_z (dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f))
	,m_userData (userData)
	,m_vertex (NULL)
	,m_simplex (NULL)
	,m_boxMinRadius (dgFloat32 (0.0f))
	,m_boxMaxRadius (dgFloat32 (0.0f))
	,m_simplexVolume (dgFloat32 (0.0f))
	,m_edgeCount (0)
	,m_vertexCount (0)
{
	dgAssert (m_rtti | dgCollisionConvexShape_RTTI);
}


dgCollisionConvex::~dgCollisionConvex ()
{
	if (m_vertex) {
		m_allocator->Free (m_vertex);
	}

	if (m_simplex) {
		m_allocator->Free (m_simplex);
	}
}

void dgCollisionConvex::SerializeLow(dgSerialize callback, void* const userData) const
{
	dgCollision::SerializeLow(callback, userData);
}

void* dgCollisionConvex::GetUserData () const
{
	return m_userData; 
}

void dgCollisionConvex::SetUserData (void* const userData)
{
	m_userData = userData;
}


void dgCollisionConvex::SetVolumeAndCG ()
{
	dgVector faceVertex[64];
	dgStack<dgInt8> edgeMarks (m_edgeCount);
	memset (&edgeMarks[0], 0, sizeof (dgInt8) * m_edgeCount);

	dgPolyhedraMassProperties localData;
	for (dgInt32 i = 0; i < m_edgeCount; i ++) {
		dgConvexSimplexEdge* const face = &m_simplex[i];
		if (!edgeMarks[i]) {
			dgConvexSimplexEdge* edge = face;
			dgInt32 count = 0;
			do {
				dgAssert ((edge - m_simplex) >= 0);
				edgeMarks[dgInt32 (edge - m_simplex)] = '1';
				faceVertex[count] = m_vertex[edge->m_vertex];
				count ++;
				dgAssert (count < dgInt32 (sizeof (faceVertex) / sizeof (faceVertex[0])));
				edge = edge->m_next;
			} while (edge != face);

			localData.AddCGFace (count, faceVertex);
		}
	}

	dgVector origin;
	dgVector inertia;
	dgVector crossInertia;
	dgFloat32 volume = localData.MassProperties (origin, inertia, crossInertia);
	m_simplexVolume = volume;

	// calculate the origin of the bound box of this primitive
	dgVector p0; 
	dgVector p1;
	for (dgInt32 i = 0; i < 3; i ++) {
		dgVector dir (dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f)); 
		dir[i] = dgFloat32 (-1.0f);
		p0[i] = SupportVertex(dir, NULL)[i];

		dir[i] = dgFloat32 (1.0f);
		p1[i] = SupportVertex(dir, NULL)[i];
	}
	p0[3] = dgFloat32 (0.0f);
	p1[3] = dgFloat32 (0.0f);
	m_boxSize = (p1 - p0).Scale3 (dgFloat32 (0.5f)); 
	m_boxOrigin = (p1 + p0).Scale3 (dgFloat32 (0.5f)); 
	m_boxMinRadius = dgMin(m_boxSize.m_x, m_boxSize.m_y, m_boxSize.m_z);
	m_boxMaxRadius = dgSqrt (m_boxSize % m_boxSize);

	m_size_x.m_x = m_boxSize.m_x;
	m_size_x.m_y = m_boxSize.m_x;
	m_size_x.m_z = m_boxSize.m_x;
	m_size_x.m_w = dgFloat32 (0.0f); 

	m_size_y.m_x = m_boxSize.m_y;
	m_size_y.m_y = m_boxSize.m_y;
	m_size_y.m_z = m_boxSize.m_y;
	m_size_y.m_w = dgFloat32 (0.0f); 

	m_size_z.m_x = m_boxSize.m_z;
	m_size_z.m_y = m_boxSize.m_z;
	m_size_z.m_z = m_boxSize.m_z;
	m_size_z.m_w = dgFloat32 (0.0f); 

	MassProperties ();
}


bool dgCollisionConvex::SanityCheck (dgPolyhedra& hull) const
{
	dgPolyhedra::Iterator iter (hull);
	for (iter.Begin(); iter; iter ++) { 
		dgEdge* const edge = &(*iter);
		if (edge->m_incidentFace < 0) {
			return false;
		}
		dgEdge* ptr = edge;
		dgVector p0 (m_vertex[edge->m_incidentVertex]);
		ptr = ptr->m_next;
		dgVector p1 (m_vertex[ptr->m_incidentVertex]);
		dgVector e1 (p1 - p0);
		dgVector n0 (dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f));
		for (ptr = ptr->m_next; ptr != edge; ptr = ptr->m_next) {
			dgVector p2 (m_vertex[ptr->m_incidentVertex]);
			dgVector e2 (p2 - p0);
			n0 += e1 * e2;
			e1 = e2;
		} 

		ptr = edge;
		do {
			dgVector p0 (m_vertex[ptr->m_twin->m_incidentVertex]);
			for (dgEdge* neiborg = ptr->m_twin->m_next->m_next; neiborg != ptr->m_twin; neiborg = neiborg->m_next) { 
				dgVector p1 (m_vertex[neiborg->m_incidentVertex]);
				dgVector dp (p1 - p0);
				dgFloat32 project = dp % n0;
				if (project > dgFloat32 (0.0f)) {
					return false;
				}
			}

			ptr = ptr->m_next;
		} while (ptr != edge);
	}

	return true;
}


void dgCollisionConvex::DebugCollision (const dgMatrix& matrix, OnDebugCollisionMeshCallback callback, void* const userData) const
{
	dgInt8 mark[DG_MAX_EDGE_COUNT];
	dgVector tmp[DG_MAX_EDGE_COUNT];
	dgTriplex vertex[DG_MAX_EDGE_COUNT];

	matrix.TransformTriplex (&tmp[0].m_x, sizeof (dgVector), &m_vertex[0].m_x, sizeof (dgVector), m_vertexCount);

	memset (mark, 0, sizeof (mark));
	for (dgInt32 i = 0; i < m_edgeCount; i ++) {
		if (!mark[i]) {
			dgConvexSimplexEdge* const face = &m_simplex[i];
			dgConvexSimplexEdge* edge = face;
			dgInt32 count = 0;
			do {
				mark[edge - m_simplex] = '1';
				dgInt32 index = edge->m_vertex;
				vertex[count].m_x = tmp[index].m_x;
				vertex[count].m_y = tmp[index].m_y;
				vertex[count].m_z = tmp[index].m_z;
				count ++;
				edge = edge->m_next;
			} while (edge != face);
			callback (userData, count, &vertex[0].m_x, 0);
		}
	}
}

void dgCollisionConvex::CalcAABB (const dgMatrix &matrix, dgVector& p0, dgVector& p1) const
{
	dgVector origin (matrix.TransformVector(m_boxOrigin));
//	dgVector size (m_boxSize.m_x * dgAbsf(matrix[0][0]) + m_boxSize.m_y * dgAbsf(matrix[1][0]) + m_boxSize.m_z * dgAbsf(matrix[2][0]),  
//				   m_boxSize.m_x * dgAbsf(matrix[0][1]) + m_boxSize.m_y * dgAbsf(matrix[1][1]) + m_boxSize.m_z * dgAbsf(matrix[2][1]),  
//				   m_boxSize.m_x * dgAbsf(matrix[0][2]) + m_boxSize.m_y * dgAbsf(matrix[1][2]) + m_boxSize.m_z * dgAbsf(matrix[2][2]),
//				   dgFloat32 (0.0f));
	dgVector size (matrix.m_front.Abs().Scale4(m_boxSize.m_x) + matrix.m_up.Abs().Scale4(m_boxSize.m_y) + matrix.m_right.Abs().Scale4(m_boxSize.m_z));

	p0 = (origin - size) & dgVector::m_triplexMask;;
	p1 = (origin + size) & dgVector::m_triplexMask;;

	p0.m_w = dgFloat32 (0.0f);
	p1.m_w = dgFloat32 (0.0f);
	#ifdef DG_DEBUG_AABB
		dgInt32 i;
		dgVector q0;
		dgVector q1;
		dgMatrix trans (matrix.Transpose());
		for (i = 0; i < 3; i ++) {
			q0[i] = matrix.m_posit[i] + matrix.RotateVector (SupportVertex(trans[i].Scale3 (-dgFloat32 (1.0f))))[i];
			q1[i] = matrix.m_posit[i] + matrix.RotateVector (SupportVertex(trans[i]))[i];
		}

		dgVector err0 (p0 - q0);
		dgVector err1 (p1 - q1);
		dgFloat32 err; 
		err = dgMax (size.m_x, size.m_y, size.m_z) * 0.5f; 
		dgAssert ((err0 % err0) < err * err);
		dgAssert ((err1 % err1) < err * err);
	#endif
}



void dgCollisionConvex::CalculateInertia (void* userData, int indexCount, const dgFloat32* const faceVertex, int faceId)
{
	dgPolyhedraMassProperties& localData = *((dgPolyhedraMassProperties*) userData);
	localData.AddInertiaAndCrossFace(indexCount, faceVertex);
}


dgFloat32 dgCollisionConvex::CalculateMassProperties (const dgMatrix& offset, dgVector& inertia, dgVector& crossInertia, dgVector& centerOfMass) const
{
	dgPolyhedraMassProperties localData;
	DebugCollision (offset, CalculateInertia, &localData);
	return localData.MassProperties (centerOfMass, inertia, crossInertia);
}


void dgCollisionConvex::MassProperties ()
{
	m_centerOfMass.m_w = dgCollisionConvex::CalculateMassProperties (dgGetIdentityMatrix(), m_inertia, m_crossInertia, m_centerOfMass);
	if (m_centerOfMass.m_w < DG_MAX_MIN_VOLUME) {
		m_centerOfMass.m_w = DG_MAX_MIN_VOLUME;
	}
	dgFloat32 invVolume = dgFloat32 (1.0f) / m_centerOfMass.m_w;
	m_inertia = m_inertia.Scale3 (invVolume);
	m_crossInertia = m_crossInertia.Scale3 (invVolume);
	m_centerOfMass = m_centerOfMass.Scale3 (invVolume);

	// complete the calculation 
	dgCollision::MassProperties ();
}

dgMatrix dgCollisionConvex::CalculateInertiaAndCenterOfMass (const dgVector& localScale, const dgMatrix& matrix) const
{
 
	if ((dgAbsf (localScale.m_x - localScale.m_y) < dgFloat32 (1.0e-5f)) && (dgAbsf (localScale.m_x - localScale.m_z) < dgFloat32 (1.0e-5f))) {
		#ifdef _DEBUG
				// using divergence theorem
				dgVector inertiaII_;
				dgVector crossInertia_;
				dgVector centerOfMass_;
				dgMatrix matrix_(matrix);
				matrix_[0] = matrix_[0].Scale3(localScale.m_x);
				matrix_[1] = matrix_[1].Scale3(localScale.m_y);
				matrix_[2] = matrix_[2].Scale3(localScale.m_z);
				dgFloat32 volume_ = CalculateMassProperties (matrix_, inertiaII_, crossInertia_, centerOfMass_);
				if (volume_ < DG_MAX_MIN_VOLUME) {
					volume_ = DG_MAX_MIN_VOLUME;
				}

				dgFloat32 invVolume_ = dgFloat32 (1.0f) / volume_;
				centerOfMass_ = centerOfMass_.Scale3(invVolume_);
				inertiaII_ = inertiaII_.Scale3 (invVolume_);
				crossInertia_ = crossInertia_.Scale3 (invVolume_);
				dgMatrix inertia_ (dgGetIdentityMatrix());
				inertia_[0][0] = inertiaII_[0];
				inertia_[1][1] = inertiaII_[1];
				inertia_[2][2] = inertiaII_[2];
				inertia_[0][1] = crossInertia_[2];
				inertia_[1][0] = crossInertia_[2];
				inertia_[0][2] = crossInertia_[1];
				inertia_[2][0] = crossInertia_[1];
				inertia_[1][2] = crossInertia_[0];
				inertia_[2][1] = crossInertia_[0];
				inertia_[3] = centerOfMass_;
		#endif

		// using general central theorem, is much faster and more accurate;
		//IImatrix = IIorigin + mass * [(displacemnet % displacemnet) * identityMatrix - transpose(displacement) * displacement)];

		dgFloat32 mag2 = localScale.m_x * localScale.m_x;
		dgMatrix inertia (dgGetIdentityMatrix());
		inertia[0][0] = m_inertia[0] * mag2;
		inertia[1][1] = m_inertia[1] * mag2;
		inertia[2][2] = m_inertia[2] * mag2;
		inertia[0][1] = m_crossInertia[2] * mag2;
		inertia[1][0] = m_crossInertia[2] * mag2;
		inertia[0][2] = m_crossInertia[1] * mag2;
		inertia[2][0] = m_crossInertia[1] * mag2;
		inertia[1][2] = m_crossInertia[0] * mag2;
		inertia[2][1] = m_crossInertia[0] * mag2;
		inertia = matrix.Inverse() * inertia * matrix;
		
		dgVector origin (matrix.TransformVector (m_centerOfMass.CompProduct3(localScale)));
		dgFloat32 mag = origin % origin;

		dgFloat32 unitMass = dgFloat32 (1.0f);
		for (dgInt32 i = 0; i < 3; i ++) {
			inertia[i][i] += unitMass * (mag - origin[i] * origin[i]);
			for (dgInt32 j = i + 1; j < 3; j ++) {
				dgFloat32 crossIJ = - unitMass * origin[i] * origin[j];
				inertia[i][j] += crossIJ;
				inertia[j][i] += crossIJ;
			}
		}

		inertia.m_posit = origin;
		inertia.m_posit.m_w = 1.0f;
		return inertia;
	} else {
		// for non uniform scale we need to the general divergence theorem
		dgVector inertiaII;
		dgVector crossInertia;
		dgVector centerOfMass;
		dgMatrix scaledMatrix(matrix);
		scaledMatrix[0] = scaledMatrix[0].Scale3(localScale.m_x);
		scaledMatrix[1] = scaledMatrix[1].Scale3(localScale.m_y);
		scaledMatrix[2] = scaledMatrix[2].Scale3(localScale.m_z);
		dgFloat32 volume = CalculateMassProperties (scaledMatrix, inertiaII, crossInertia, centerOfMass);
		if (volume < DG_MAX_MIN_VOLUME) {
			volume = DG_MAX_MIN_VOLUME;
		}

		dgFloat32 invVolume = dgFloat32 (1.0f) / volume;
		centerOfMass = centerOfMass.Scale3(invVolume);
		inertiaII = inertiaII.Scale3 (invVolume);
		crossInertia = crossInertia.Scale3 (invVolume);
		dgMatrix inertia (dgGetIdentityMatrix());
		inertia[0][0] = inertiaII[0];
		inertia[1][1] = inertiaII[1];
		inertia[2][2] = inertiaII[2];
		inertia[0][1] = crossInertia[2];
		inertia[1][0] = crossInertia[2];
		inertia[0][2] = crossInertia[1];
		inertia[2][0] = crossInertia[1];
		inertia[1][2] = crossInertia[0];
		inertia[2][1] = crossInertia[0];
		inertia[3] = centerOfMass;
		return inertia;
	}
}


dgFloat32 dgCollisionConvex::GetVolume () const
{
	return m_centerOfMass.m_w;
}

dgFloat32 dgCollisionConvex::GetBoxMinRadius () const 
{
	return m_boxMinRadius;
} 

dgFloat32 dgCollisionConvex::GetBoxMaxRadius () const 
{
	return m_boxMaxRadius;
} 



dgVector dgCollisionConvex::CalculateVolumeIntegral (
	const dgMatrix& globalMatrix, 
	GetBuoyancyPlane buoyancyPlane, 
	void* context) const
{

	dgVector cg  (dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f));
	if (buoyancyPlane) {
		dgPlane globalPlane;
		if (buoyancyPlane (0, context, globalMatrix, globalPlane)) {
		//if (buoyancyPlane ((void*)SetUserDataID(), context, globalMatrix, globalPlane)) {
			globalPlane = globalMatrix.UntransformPlane (globalPlane);
			cg  = CalculateVolumeIntegral (globalPlane);
		}
	}
	
	dgFloat32 volume = cg.m_w;
	cg = globalMatrix.TransformVector (cg);
	cg.m_w = volume;

	return cg;

}


dgVector dgCollisionConvex::CalculateVolumeIntegral (const dgPlane& plane) const 
{

	dgInt8 mark[DG_MAX_EDGE_COUNT];
	dgFloat32 test[DG_MAX_EDGE_COUNT];
	dgVector faceVertex[256];

	dgVector cg (dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f)); 

	dgInt32 positive = 0;
	dgInt32 negative = 0;
	for (dgInt32 i = 0; i < m_vertexCount; i ++) {
		test[i] = plane.Evalue (m_vertex[i]);
		if (test[i] > dgFloat32 (1.0e-5f)) {
			positive ++;
		} else if (test[i] < -dgFloat32 (1.0e-5f)) {
			negative ++;
		} else {
			test[i] = dgFloat32 (0.0f);
		}
	}

	if (positive == m_vertexCount) {
		return cg;
	}

	if (negative == m_vertexCount) {
		dgVector volume (this->GetVolume());
		volume.m_w = m_simplexVolume;
		return volume;
	}

	dgPolyhedraMassProperties localData;
	dgConvexSimplexEdge* capEdge = NULL;

	//	m_mark ++;
	memset (mark, 0, sizeof (mark));
	for (dgInt32 i = 0; i < m_edgeCount; i ++) {
		if (!mark[i]) {
			dgConvexSimplexEdge* const face = &m_simplex[i];
			dgConvexSimplexEdge* edge = face;
			dgInt32 count = 0;
			dgFloat32 size0 = test[edge->m_prev->m_vertex];
			do {
				//edge->m_mark = m_mark;
				mark[edge - m_simplex] = '1';
				dgFloat32 size1 = test[edge->m_vertex];
				if (size0 <= dgFloat32 (0.0f)) {
					faceVertex[count] = m_vertex[edge->m_prev->m_vertex];
					count ++;
					if (size1 > dgFloat32 (0.0f)) {
						dgVector dp (m_vertex[edge->m_vertex] - m_vertex[edge->m_prev->m_vertex]);
						faceVertex[count] = m_vertex[edge->m_prev->m_vertex] - dp.Scale3 (size0 / (plane % dp));
						count ++;
					}
				} else if (size1 < dgFloat32 (0.0f)) {
					dgVector dp (m_vertex[edge->m_vertex] - m_vertex[edge->m_prev->m_vertex]);
					faceVertex[count] = m_vertex[edge->m_prev->m_vertex] - dp.Scale3 (size0 / (plane % dp));
					count ++;
					dgAssert (count < dgInt32 (sizeof (faceVertex) / sizeof (faceVertex[0])));
				}

				if (!capEdge) {
					if ((size1 > dgFloat32 (0.0f)) && (size0 < dgFloat32 (0.0f))) {
						capEdge = edge->m_prev->m_twin;
					}
				}

				size0 = size1;
				edge = edge->m_next;
			} while (edge != face);

			if (count) {
				localData.AddCGFace(count, faceVertex);
			}
		}
	}


	if (capEdge) {
		dgInt32 count = 0;
		dgConvexSimplexEdge* edge = capEdge;
		dgConvexSimplexEdge *ptr = NULL;
		do {
			dgVector dp (m_vertex[edge->m_twin->m_vertex] - m_vertex[edge->m_vertex]);
			faceVertex[count] = m_vertex[edge->m_vertex] - dp.Scale3 (test[edge->m_vertex] / (plane % dp));
			count ++;
			if (count == 127) {
				// something is wrong return zero
				return dgVector (dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f));
			}

			for (ptr = edge->m_next; ptr != edge; ptr = ptr->m_next) {
				dgInt32 index0 = ptr->m_twin->m_vertex;
				if (test[index0] > dgFloat32 (0.0f)) {
					index0 = ptr->m_vertex;
					if (test[index0] < dgFloat32 (0.0f)) {
						break;
					}
				}
			}
			edge = ptr->m_twin;
		} while (edge != capEdge);
		localData.AddCGFace(count, faceVertex);
	}

	dgVector inertia;
	dgVector crossInertia;
	dgFloat32 volume = localData.MassProperties (cg, inertia, crossInertia);
	cg = cg.Scale3 (dgFloat32 (1.0f) / dgMax (volume, dgFloat32 (1.0e-4f)));
	cg.m_w = volume;
	return cg; 
}



dgVector dgCollisionConvex::SupportVertex (const dgVector& dir, dgInt32* const vertexIndex) const
{
	dgAssert (dgAbsf(dir % dir - dgFloat32 (1.0f)) < dgFloat32 (1.0e-3f));

	dgInt16 cache[16];
	memset (cache, -1, sizeof (cache));
	dgConvexSimplexEdge* edge = &m_simplex[0];
	
	dgInt32 index = edge->m_vertex;
	dgFloat32 side0 = m_vertex[index] % dir;

	cache [index & (sizeof (cache) / sizeof (cache[0]) - 1)] = dgInt16 (index);
	dgConvexSimplexEdge* ptr = edge;
	dgInt32 maxCount = 128;
	do {
		dgInt32 index1 = ptr->m_twin->m_vertex;
		if (cache [index1 & (sizeof (cache) / sizeof (cache[0]) - 1)] != index1) {
			cache [index1 & (sizeof (cache) / sizeof (cache[0]) - 1)] = dgInt16 (index1);
			dgFloat32 side1 = m_vertex[index1] % dir;
			if (side1 > side0) {
				index = index1;
				side0 = side1;
				edge = ptr->m_twin;
				ptr = edge;
			}
		}
		ptr = ptr->m_twin->m_next;
		maxCount --;
	} while ((ptr != edge) && maxCount);
	dgAssert (maxCount);

	dgAssert (index != -1);
	return m_vertex[index];
}



bool dgCollisionConvex::SanityCheck(dgInt32 count, const dgVector& normal, dgVector* const contactsOut) const
{
	if (count > 1) {
		dgInt32 j = count - 1;
		for (dgInt32 i = 0; i < count; i ++) {
			dgVector error (contactsOut[i] - contactsOut[j]);
			//			dgAssert ((error % error) > dgFloat32 (1.0e-20f));
			if ((error % error) <= dgFloat32 (1.0e-20f)) {
				return false;
			} 
			j = i;
		}

		if (count >= 3) {
			dgVector n (dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f));
			dgVector e0 (contactsOut[1] - contactsOut[0]);
			for (dgInt32 i = 2; i < count; i ++) {
				dgVector e1 (contactsOut[i] - contactsOut[0]);
				n += e0 * e1;
				e0 = e1;
			} 
			dgAssert ((n % n) > dgFloat32 (0.0f));
			n = n.Scale3 (dgRsqrt(n % n));
			dgFloat32 projection;
			projection = n % normal;
			dgAssert (projection > dgFloat32 (0.9f));
			if (projection < dgFloat32 (0.9f)) {
				return false;
			}

			e0 = contactsOut[count-1] - contactsOut[count-2];
			j = count - 1;
			for (dgInt32 i = 0; i < count; i ++) {
				dgVector e1 (contactsOut[i] - contactsOut[j]);
				dgVector n (e0 * e1);
				dgFloat32 error = n % normal;
				dgAssert (error >= dgFloat32 (-1.0e-4f));
				if (error < dgFloat32 (-1.0e-4f)) {
					return false;
				}
				j = i;
				e0 = e1;
			}
		}
	}
	return true;
}



dgInt32 dgCollisionConvex::SimplifyClipPolygon (dgInt32 count, const dgVector& normal, dgVector* const polygon) const
{
	dgInt8 mark[DG_MAX_VERTEX_CLIP_FACE * 8];
	dgInt8 buffer[8 * DG_MAX_VERTEX_CLIP_FACE * (sizeof (dgInt32) + sizeof (dgFloat32))];

	dgAssert (count < dgInt32 (sizeof (mark) / sizeof (mark[0])));
	dgUpHeap<dgInt32, dgFloat32> sortHeap (buffer, sizeof (buffer));	

	while (count > DG_MAX_VERTEX_CLIP_FACE) {
		sortHeap.Flush();

		dgInt32 i0 = count - 2;
		dgInt32 i1 = count - 1;
		for (dgInt32 i2 = 0; i2 < count; i2 ++) {
			mark[i2] = 0;

			dgVector e0 = polygon[i1] - polygon[i0];
			dgVector e1 = polygon[i2] - polygon[i0];
			dgFloat32 area = dgAbsf (normal % (e0 * e1));

			sortHeap.Push(i1, area);

			i0 = i1;
			i1 = i2;
		}

		dgInt32 removeCount = count - DG_MAX_VERTEX_CLIP_FACE;
		while (sortHeap.GetCount() && removeCount) {
			dgInt32 i1 = sortHeap[0];
			sortHeap.Pop();

			dgInt32 i0 = (i1 - 1) >= 0 ? i1 - 1 : count - 1;
			dgInt32 i2 = (i1 + 1) < count ? i1 + 1 : 0;

			if (!(mark[i0] || mark[i2])) {
				mark[i1] = 1;
				removeCount --;
			}
		}

		i0 = 0;
		for (dgInt32 i1 = 0; i1 < count; i1 ++) {
			if (!mark[i1]) {
				polygon[i0] = polygon[i1];
				i0 ++;
			}
		}
		count = i0;
	}

	return count;
}


dgInt32 dgCollisionConvex::RectifyConvexSlice (dgInt32 count, const dgVector& normal, dgVector* const contactsOut) const
{
	struct DG_CONVEX_FIXUP_FACE
	{
		dgInt32 m_vertex;
		DG_CONVEX_FIXUP_FACE* m_next;
	};

	DG_CONVEX_FIXUP_FACE linkFace[DG_CLIP_MAX_POINT_COUNT * 2];

	dgAssert (count > 2);

	DG_CONVEX_FIXUP_FACE* poly = &linkFace[0];
	for (dgInt32 i = 0; i < count; i ++) {
		contactsOut[i].m_w = dgFloat32 (1.0f);
		linkFace[i].m_vertex = i;
		linkFace[i].m_next = &linkFace[i + 1];
	}
	linkFace[count - 1].m_next = &linkFace[0];

	dgInt32 restart = 1;
	dgInt32 tmpCount = count;
	while (restart && (tmpCount >= 2)) {
		restart = 0;
		DG_CONVEX_FIXUP_FACE* ptr = poly; 
		dgInt32 loops = tmpCount;
		do {
			dgInt32 i0 = ptr->m_vertex;
			dgInt32 i1 = ptr->m_next->m_vertex;
			dgVector error (contactsOut[i1] - contactsOut[i0]);
			dgFloat32 dist2 = error % error;
			if (dist2 < dgFloat32 (0.003f * 0.003f)) {
				if (ptr->m_next == poly) {
					poly = ptr;
				} 
				restart = 1;
				tmpCount --;
				contactsOut[i1].m_w = dgFloat32 (0.0f);
				ptr->m_next = ptr->m_next->m_next;
			} else {
				ptr = ptr->m_next;
			}

			loops --;
		} while (loops);
	}

	restart = 1;
	while (restart && (tmpCount >= 3)) {
		restart = 0;
		DG_CONVEX_FIXUP_FACE* ptr = poly;
		dgInt32 loops = tmpCount;
		do {
			dgInt32 i0 = ptr->m_vertex;
			dgInt32 i1 = ptr->m_next->m_vertex;
			dgInt32 i2 = ptr->m_next->m_next->m_vertex;
			dgVector e0 (contactsOut[i2] - contactsOut[i1]);
			dgVector e1 (contactsOut[i0] - contactsOut[i1]);
			dgVector n (e0 * e1);
			dgFloat32 area = normal % n;
			if (area <= dgFloat32 (1.0e-5f)) {
				if (ptr->m_next == poly) {
					poly = ptr;
				}
				restart = 1;
				tmpCount --;
				contactsOut[i1].m_w = dgFloat32 (0.0f);
				ptr->m_next = ptr->m_next->m_next;
			} else {
				ptr = ptr->m_next;
			}
			loops --;
		} while (loops);
	}

	if (tmpCount < count) {
		dgInt32 newCount = 0;
		for (; newCount < count; newCount ++) {
			if (contactsOut[newCount].m_w == dgFloat32 (0.0f)) {
				break;
			}
		}

		for (dgInt32 i = newCount + 1; i < count; i ++) {
			if (contactsOut[i].m_w != dgFloat32 (0.0f)) {
				contactsOut[newCount] = contactsOut[i];
				newCount ++;
			}
		}
		count = newCount;
		dgAssert (tmpCount == count);
	}


	if (count > DG_MAX_VERTEX_CLIP_FACE) {
		count = SimplifyClipPolygon (count, normal, contactsOut);
	}

	dgAssert (SanityCheck(count, normal, contactsOut));
	return count;
}


dgCollisionConvex::dgPerimenterEdge* dgCollisionConvex::ReduceContacts (dgPerimenterEdge* poly, dgInt32 maxCount) const
{
	dgInt32 buffer [256];
	dgUpHeap<dgPerimenterEdge*, dgFloat32> heap (buffer, sizeof (buffer));	

	dgInt32 restart = 1;
	while (restart) {
		restart = 0;
		dgPerimenterEdge* ptr0 = poly; 
		poly = poly->m_next;
		if (poly->m_next != poly) {
			heap.Flush();
			dgPerimenterEdge* ptr = poly; 
			do {
				dgFloat32 dist2;
				dgVector error (*ptr->m_next->m_vertex - *ptr->m_vertex);
				dist2 = error % error;
				if (dist2 < DG_MINK_VERTEX_ERR2) {
					ptr0->m_next = ptr->m_next;
					if (ptr == poly) {
						poly = ptr0;
						restart = 1;
						break;
					} 
					ptr = ptr0;
				} else {
					heap.Push(ptr, dist2);
					ptr0 = ptr;
				}

				ptr = ptr->m_next;
			} while (ptr != poly);
		}
	}


	if (heap.GetCount()) {
		if (maxCount > 8) {
			maxCount = 8;
		}
		while (heap.GetCount() > maxCount) {
			//dgFloat32 dist2;
			dgPerimenterEdge* ptr = heap[0];
			heap.Pop();
			for (dgInt32 i = 0; i < heap.GetCount(); i ++) {
				if (heap[i] == ptr->m_next) {
					heap.Remove(i);
					break;
				}
			}

			ptr->m_next = ptr->m_next->m_next;
			dgVector error (*ptr->m_next->m_vertex - *ptr->m_vertex);
			dgFloat32 dist2 = error % error;
			heap.Push(ptr, dist2);
		}
		poly = heap[0];
	}

	return poly;
}


dgInt32 dgCollisionConvex::CalculatePlaneIntersection (const dgVector& normal, const dgVector& origin, dgVector* const contactsOut) const
{
	dgConvexSimplexEdge* edge = &m_simplex[0];
	dgPlane plane (normal, - (normal % origin));

	dgFloat32 side0 = plane.Evalue(m_vertex[edge->m_vertex]);
	dgFloat32 side1 = side0;
	dgConvexSimplexEdge *firstEdge = NULL;
	if (side0 > dgFloat32 (0.0f)) {
		dgConvexSimplexEdge* ptr = edge;
		do {
			dgAssert (m_vertex[ptr->m_twin->m_vertex].m_w == dgFloat32 (1.0f));
			side1 = plane.Evalue (m_vertex[ptr->m_twin->m_vertex]);
			if (side1 < side0) {
				if (side1 < dgFloat32 (0.0f)) {
					firstEdge = ptr;
					break;
				}

				side0 = side1;
				edge = ptr->m_twin;
				ptr = edge;
			}
			ptr = ptr->m_twin->m_next;
		} while (ptr != edge);


		if (!firstEdge) {
			// we may have a local minimal in the convex hull do to a big flat face
			for (dgInt32 i = 0; i < m_edgeCount; i ++) {
				ptr = &m_simplex[i];
				side0 = plane.Evalue (m_vertex[ptr->m_vertex]);
				side1 = plane.Evalue (m_vertex[ptr->m_twin->m_vertex]);
				if ((side1 < dgFloat32 (0.0f)) && (side0 > dgFloat32 (0.0f))){
					firstEdge = ptr;
					break;
				}
			}
		}

	} else if (side0 < dgFloat32 (0.0f)) {
		dgConvexSimplexEdge* ptr = edge;
		do {
			dgAssert (m_vertex[ptr->m_twin->m_vertex].m_w == dgFloat32 (1.0f));
			side1 = plane.Evalue (m_vertex[ptr->m_twin->m_vertex]);
			if (side1 > side0) {
				if (side1 >= dgFloat32 (0.0f)) {
					side0 = side1;
					firstEdge = ptr->m_twin;
					break;
				}

				side0 = side1;
				edge = ptr->m_twin;
				ptr = edge;
			}
			ptr = ptr->m_twin->m_next;
		} while (ptr != edge);

		if (!firstEdge) {
			// we may have a local minimal in the convex hull do to a big flat face
			for (dgInt32 i = 0; i < m_edgeCount; i ++) {
				ptr = &m_simplex[i];
				side0 = plane.Evalue (m_vertex[ptr->m_vertex]);
				dgFloat32 side1 = plane.Evalue (m_vertex[ptr->m_twin->m_vertex]);
				if ((side1 < dgFloat32 (0.0f)) && (side0 > dgFloat32 (0.0f))){
					firstEdge = ptr;
					break;
				}
			}
		}
	}

	dgInt32 count = 0;
	if (firstEdge) {
		dgAssert (side0 >= dgFloat32 (0.0f));
		dgAssert ((side1 = plane.Evalue (m_vertex[firstEdge->m_vertex])) >= dgFloat32 (0.0f));
		dgAssert ((side1 = plane.Evalue (m_vertex[firstEdge->m_twin->m_vertex])) < dgFloat32 (0.0f));
		dgAssert (dgAbsf (side0 - plane.Evalue (m_vertex[firstEdge->m_vertex])) < dgFloat32 (1.0e-5f));

		dgInt32 maxCount = 0;
		dgConvexSimplexEdge* ptr = firstEdge;
		do {
			if (side0 > dgFloat32 (0.0f)) {
				dgAssert (plane.Evalue (m_vertex[ptr->m_vertex]) > dgFloat32 (0.0f));
				dgAssert (plane.Evalue (m_vertex[ptr->m_twin->m_vertex]) < dgFloat32 (0.0f));

				dgVector dp (m_vertex[ptr->m_twin->m_vertex] - m_vertex[ptr->m_vertex]);
				dgFloat32 t = plane % dp;
				if (t >= dgFloat32 (-1.e-24f)) {
					t = dgFloat32 (0.0f);
				} else {
					t = side0 / t;
					if (t > dgFloat32 (0.0f)) {
						t = dgFloat32 (0.0f);
					}
					if (t < dgFloat32 (-1.0f)) {
						t = dgFloat32 (-1.0f);
					}
				}

				dgAssert (t <= dgFloat32 (0.01f));
				dgAssert (t >= dgFloat32 (-1.05f));
				contactsOut[count] = m_vertex[ptr->m_vertex] - dp.Scale3 (t);

				dgConvexSimplexEdge* ptr1 = ptr->m_next;
				for (; ptr1 != ptr; ptr1 = ptr1->m_next) {
					dgAssert (m_vertex[ptr->m_twin->m_vertex].m_w == dgFloat32 (1.0f));
					side0 = plane.Evalue (m_vertex[ptr1->m_twin->m_vertex]); 
					if (side0 >= dgFloat32 (0.0f)) {
						break;
					}
				}
				dgAssert (ptr1 != ptr);
				ptr = ptr1->m_twin;
			} else {
				contactsOut[count] = m_vertex[ptr->m_vertex];
				dgConvexSimplexEdge* ptr1 = ptr->m_next;
				for (; ptr1 != ptr; ptr1 = ptr1->m_next) {
					dgAssert (m_vertex[ptr1->m_twin->m_vertex].m_w == dgFloat32 (1.0f));
					side0 = plane.Evalue (m_vertex[ptr1->m_twin->m_vertex]); 
					if (side0 >= dgFloat32 (0.0f)) {
						break;
					}
				}

				if (ptr1 == ptr) {
					ptr = ptr1->m_prev->m_twin;
				} else {
					ptr = ptr1->m_twin;
				}
			}

			count ++;
			maxCount ++;
			if (count >= DG_CLIP_MAX_POINT_COUNT) {
				for (count = 0; count < (DG_CLIP_MAX_POINT_COUNT >> 1); count ++) {
					contactsOut[count] = contactsOut[count * 2];
				}
			}

		} while ((ptr != firstEdge) && (maxCount < DG_CLIP_MAX_COUNT));
		dgAssert (maxCount < DG_CLIP_MAX_COUNT);

		if (count > 2) {
			count = RectifyConvexSlice (count, normal, contactsOut);
		}
	}
	return count;
}

#if 1
dgInt32 dgCollisionConvex::RayCastClosestFace (dgVector* tetrahedrum, const dgVector& origin, dgFloat32& pointDist) const
{
	#define PLANE_MAX_ITERATION 128

	dgInt32 face = 0;
	dgInt32 plane = -1;
	dgFloat32 maxDist = dgFloat32 (1.0e10f);

	dgInt32 j = 0;
	for (; (face != -1) && (j < PLANE_MAX_ITERATION); j ++) {
		face = -1;

		// initialize distance to zero (very important)
		maxDist = dgFloat32 (0.0f);
		dgVector normal (dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f));
		for (dgInt32 i = 0; i < 4; i ++) {
			dgInt32 i0 = m_rayCastSimplex[i][0];
			dgInt32 i1 = m_rayCastSimplex[i][1];
			dgInt32 i2 = m_rayCastSimplex[i][2];
			const dgVector& p0 = tetrahedrum[i0];
			const dgVector& p1 = tetrahedrum[i1];
			const dgVector& p2 = tetrahedrum[i2];
			dgVector e0 (p1 - p0);
			dgVector e1 (p2 - p0);
			dgVector n (e0 * e1);

			dgFloat32 dist = n % n;
			if (dist > dgFloat32 (1.0e-24f)) {
				n = n.Scale3 (dgRsqrt (n % n));
				dist = n % (origin - p0);
				// find the plane farther away from the origin
				if (dist >= maxDist) {
					maxDist = dist;
					normal = n;
					face = i;
				}
			}
		}

		if (face != -1) {
			dgInt32 j0 = m_rayCastSimplex[face][0];
			dgVector p (SupportVertex (normal, NULL));
			dgFloat32 dist = normal % (p - tetrahedrum[j0]);
			if(dist < dgFloat32 (1.0e-5f)) {
				plane = face;
				break;
			}

			dgInt32 j1 = m_rayCastSimplex[face][1];
			dgInt32 j3 = m_rayCastSimplex[face][3];
			tetrahedrum[j3] = p;
			dgSwap (tetrahedrum[j0], tetrahedrum[j1]);

			dgInt32 i0 = m_rayCastSimplex[0][0];
			dgInt32 i1 = m_rayCastSimplex[0][1];
			dgInt32 i2 = m_rayCastSimplex[0][2];
			dgInt32 i3 = m_rayCastSimplex[0][3];

			dgVector e0 (tetrahedrum[i1] - tetrahedrum[i0]);
			dgVector e1 (tetrahedrum[i2] - tetrahedrum[i0]);
			dgVector e2 (tetrahedrum[i3] - tetrahedrum[i0]);

			dist = (e1 * e0) % e2;
			if (dgAbsf (dist) < dgFloat32 (1.0e-4)) {
				dgBigVector p0 (tetrahedrum[i0]);
				dgBigVector p1 (tetrahedrum[i1]);
				dgBigVector p2 (tetrahedrum[i2]);
				dgBigVector p3 (tetrahedrum[i3]);
				dgBigVector e0 (p1 - p0);
				dgBigVector e1 (p2 - p0);
				dgBigVector e2 (p3 - p0);
				dgFloat64 dist1 = (e1 * e0) % e2;
				if (dist1 <= dgFloat64 (0.0f)) {
					dgSwap (tetrahedrum[1], tetrahedrum[2]);
				}
			} else if (dist <= dgFloat32 (0.0f)) {
				dgSwap (tetrahedrum[1], tetrahedrum[2]);
			}

		}
	} 

	if (j >= PLANE_MAX_ITERATION) {
		plane = -1;
		if ((face != -1) && (maxDist >= dgFloat32 (0.0f))) {
			for (dgInt32 i = 0; i < 4; i ++) {
				dgInt32 i0 = m_rayCastSimplex[i][0];
				dgInt32 i1 = m_rayCastSimplex[i][1];
				dgInt32 i2 = m_rayCastSimplex[i][2];
				const dgVector& p0 = tetrahedrum[i0];
				const dgVector& p1 = tetrahedrum[i1];
				const dgVector& p2 = tetrahedrum[i2];
				dgVector e0 (p1 - p0);
				dgVector e1 (p2 - p0);
				dgVector n (e0 * e1);

				dgFloat32 dist = n % n;
				if (dist > dgFloat32 (1.0e-24f)) {
					n = n.Scale3 (dgRsqrt (n % n));
					dist = n % (origin - p0);
					if (dist >= maxDist) {
						maxDist = dist;
						plane = i;
					}
				}
			}
		}
	}
	pointDist = maxDist;
	return plane;
}


dgFloat32 dgCollisionConvex::RayCast (const dgVector& localP0, const dgVector& localP1, dgContactPoint& contactOut, const dgBody* const body, void* const userData) const
{
	#define DG_LEN  (0.01f)
	#define DG_AREA (DG_LEN * DG_LEN)
	#define DG_VOL  (DG_AREA * DG_LEN)

	dgFloat32 interset = dgFloat32 (1.2f);

	dgFloat32 tMin = dgFloat32 (0.0f);
	dgFloat32 tMax = dgFloat32 (1.0f);

	dgVector p0 (localP0 - m_boxOrigin); 
	dgVector p1 (localP1 - m_boxOrigin); 
	dgVector paddedBox (m_boxSize.Scale3 (1.1f));
	for (dgInt32 i = 0; i < 3; i ++) {
		dgFloat32 den = p1[i] - p0[i]; 
		if (dgAbsf (den) < dgFloat32 (1.0e-6f)) {
			if (p0[i] <  -paddedBox[i]) {
				tMin = dgFloat32 (1.2f);
				break;

			}
			if (p0[i] > paddedBox[i]) {
				tMin = dgFloat32 (1.2f);
				break;
			}
		} else {
			den = dgFloat32 (1.0f) / den;
			dgFloat32 t0 = (-paddedBox[i] - p0[i]) * den;
			dgFloat32 t1 = ( paddedBox[i] - p0[i]) * den;
			if (t0 > t1) {
				dgSwap(t0, t1);
			}

			if (tMin < t0) {
				tMin = t0;
			}

			if (tMax > t1) {
				tMax = t1;
			}

			if (tMin > tMax) {
				tMin = dgFloat32 (1.2f);
				break;
			}
		}
	}
	
	if (tMin < dgFloat32 (1.0f)) {
		dgVector tetrahedrum[4];

		dgVector localP0 (localP0 + (localP1 - localP0).Scale3(tMin));
		dgVector bestPoint (dgFloat32(0.0f), dgFloat32(0.0f), dgFloat32(0.0f), dgFloat32(0.0f));
		dgVector normal (dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f));
		dgVector step (localP1 - localP0); 
		dgFloat32 dist2 = step % step;
		if (dist2 > dgFloat32 (1.0e-8f)) {
			dgVector dir (step.Scale3 (dgRsqrt (dist2)));

			tetrahedrum[0] = SupportVertex(dir.Scale3 (-1.0f), NULL);
			tetrahedrum[1] = SupportVertex(dir, NULL);

			dgInt32 i = 0;
			dgInt32 normalsCount = sizeof (m_hullDirs) / sizeof (m_hullDirs[0]);

			dgVector e1 (tetrahedrum[1] - tetrahedrum[0]);
			dgFloat32 error2 = e1 % e1;
			if (error2 < dgFloat32 (1.0e-2f)) {
				dgFloat32 maxError2 = dgFloat32 (0.0f);
				for (i = 0; i < normalsCount; i ++) {
					tetrahedrum[1] = SupportVertex(m_hullDirs[i], NULL);
					e1 = tetrahedrum[1] - tetrahedrum[0];
					error2 = e1 % e1;
					if (error2 > DG_AREA) {
						break;
					}
					if (error2 > maxError2) {
						maxError2 = error2;
						bestPoint = tetrahedrum[1];
					}
				}
				if (i >= normalsCount) {
					tetrahedrum[1] = bestPoint;
					e1 = tetrahedrum[1] - tetrahedrum[0];
					i = 0;
				}
			}

			dgFloat32 maxError2 = dgFloat32 (0.0f);
			for (i ++; i < normalsCount; i ++) {
				tetrahedrum[2] = SupportVertex(m_hullDirs[i], NULL);
				dgVector e2 (tetrahedrum[2] - tetrahedrum[0]);
				normal = e1 * e2;
				error2 = normal % normal;
				if (error2 > DG_AREA) {
					break;
				}
				if (error2 > maxError2) {
					maxError2 = error2;
					bestPoint = tetrahedrum[2];
				}
			}
			if (i >= normalsCount) {
				tetrahedrum[2] = bestPoint;
				dgVector e2 (tetrahedrum[2] - tetrahedrum[0]);
				normal = e1 * e2;
				i = 0;
			}

			maxError2 = dgFloat32 (0.0f);
			for (i ++; i < normalsCount; i ++) {
				tetrahedrum[3] = SupportVertex(m_hullDirs[i], NULL);
				dgVector e3 (tetrahedrum[3] - tetrahedrum[0]);
				error2 = normal % e3;
				if (dgAbsf (error2) > DG_VOL) {
					break;
				}

				if (error2 > maxError2) {
					maxError2 = error2;
					bestPoint = tetrahedrum[3];
				}
			}

			if (i >= normalsCount) {
				tetrahedrum[3] = bestPoint;
				//dgVector e3 (tetrahedrum[3] - tetrahedrum[0]);
				error2 = maxError2;
			}

			if (dgAbsf(error2) > dgFloat32 (1.0e-12f)) {
				if (error2 < dgFloat32 (0.0f)) {
					dgSwap (tetrahedrum[0], tetrahedrum[1]);
				}

				dgInt32 passes = 0;
				dgFloat32 t0 = dgFloat32 (0.0f);
				dgInt32 face = RayCastClosestFace (tetrahedrum, localP0, error2);
				error2 = dgFloat32 (1.0e10f);
				for (dgInt32 outside = (face != -1); (passes < 128) && outside && (error2 > dgFloat32 (1.0e-5f)); outside = (face != -1)) {
					passes ++;
					dgInt32 i0 = m_rayCastSimplex[face][0];
					dgInt32 i1 = m_rayCastSimplex[face][1];
					dgInt32 i2 = m_rayCastSimplex[face][2];
					const dgVector& p0 = tetrahedrum[i0];
					const dgVector& p1 = tetrahedrum[i1];
					const dgVector& p2 = tetrahedrum[i2];
					dgVector e0 (p1 - p0);
					dgVector e1 (p2 - p0);
					normal = e0 * e1;

					face = -1;
					error2 = dgFloat32 (1.0e10f);
					dgFloat32 t = normal % step;
					if (dgAbsf (t) > dgFloat32 (0.0f)) {
						t = (normal % (p0 - localP0)) / t;
						if ((t >= t0) && (t <= dgFloat32 (1.0f))) {
							dgVector p (localP0 + step.Scale3 (t));
							face = RayCastClosestFace (tetrahedrum, p, error2);
							t0 = t;
						}
					}
				}

				if (error2 < dgFloat32 (1.0e-4f)) {
					dgVector numVector (localP0 + step.Scale3(t0) - localP0);
					dgVector denVector (localP1 - localP0);
					interset = (numVector % denVector) / (denVector % denVector);
					contactOut.m_normal = normal.Scale3 (dgRsqrt(normal % normal));
					//contactOut.m_userId = SetUserDataID();
				}
			}
		}
	}
	return interset;
}

#else 

dgFloat32 dgCollisionConvex::RayCast (const dgVector& localP0, const dgVector& localP1, dgContactPoint& contactOut, const dgBody* const body, void* const userData) const
{
	dgVector simplex[4];
	dgVector sum[4];
	dgInt32 indexArray[4];

	dgVector normal;
	dgVector point (localP0);
	dgVector p0p1 (localP0 - localP1);

	memset (sum, 0, sizeof (sum));
	dgVector dir (p0p1.CompProduct4 (p0p1.DotProduct4(p0p1).InvSqrt ()));
	simplex[0] = SupportVertex (dir, NULL) - localP0;
	do {

		dgInt32 index = 1;
		dgInt32 iter = 0;
		dgInt32 cycling = 0;
		dgFloat32 minDist = dgFloat32 (1.0e20f);
		dgVector v (simplex[0]);
		do {
			dgFloat32 dist = v % v;
			if (dist < dgFloat32 (1.0e-9f)) {
				dgAssert (0);
//				return -index; 
			}

			if (dist < minDist) {
				minDist = dist;
				cycling = -1;
			}
			cycling ++;
			if (cycling > 4) {
				dgAssert (0);
				// for now return -1
//				return -index;
			}

			dgVector dir (v.Scale3 (-dgRsqrt(dist)));
			simplex[index] = SupportVertex (dir, NULL) - localP0;
			const dgVector& w = simplex[index];
			dgVector wv (w - v);
			dist = dir % wv;
			if (dist < dgFloat32 (1.0e-3f)) {
				normal = dir;
				break;
			}

			index ++;
			switch (index) 
			{
				case 2:
				{
					v = dgMinkHull::ReduceLine (index, simplex, sum, indexArray);
					break;
				}

				case 3:
				{
					v = dgMinkHull::ReduceTriangle (index, simplex, sum, indexArray);
					break;
				}

				case 4:
				{
					_ASSERTE (0);
//					v = ReduceTetrahedrum (index, m_hullDiff, m_hullSum, m_polygonFaceIndex);
					break;
				}
			}

			iter ++;
			cycling ++;
		} while (iter < DG_CONNICS_CONTATS_ITERATIONS); 

		switch (index) 
		{
			case 2:
			{
				dgAssert(0);
				break;
			}

			case 3:
			{
				dgVector q (localP0 + v);
				dgAssert(0);
				break;
			}
			
			default:
				dgAssert(0);
		}

	} while (1);
	return 0;
}
#endif

dgVector dgCollisionConvex::ConvexConicSupporVertex (const dgVector& dir) const 
{
	return SupportVertex(dir, NULL);
}

dgVector dgCollisionConvex::ConvexConicSupporVertex (const dgVector& point, const dgVector& dir) const
 {
	return point;
 }

dgInt32 dgCollisionConvex::CalculateContacts (const dgVector& point, const dgVector& normal, dgCollisionParamProxy& proxy, dgVector* const contactsOut) const
{
	dgAssert (dgAbsf (normal % normal - dgFloat32 (1.0f)) < dgFloat32 (1.0e-3f));
	return CalculateContactsGeneric (point, normal, proxy, contactsOut);
}


bool dgCollisionConvex::PolygonOBBTest (const dgFloat32* const polygon, dgInt32 strideInBytes, const dgInt32* const indexArray, dgInt32 indexCount, const dgPolygonMeshDesc& descInfo) const
{
	dgInt32 stride = strideInBytes / sizeof (dgFloat32);
	if (descInfo.m_doContinuesCollisionTest) {
		dgInt32 normalIndex = descInfo.GetNormalIndex(indexArray, indexCount);
		dgVector normal (&polygon[normalIndex * stride]);
		if ((normal % descInfo.m_boxDistanceTravelInMeshSpace) >= dgFloat32 (0.0f)) {
			return false;
		}
	}
	
	dgVector box[2];
	dgVector face[64];
	dgVector simplex[4];
	dgInt32 polygonFaceIndex[4];

	// keep GCC happy
	face[0] = dgVector(dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f), dgFloat32 (0.0f));
	const dgMatrix& spaceMatrix = descInfo.m_meshToShapeMatrix;
	for (dgInt32 i = 0; i < indexCount; i ++) {
		face[i] = spaceMatrix.TransformVector(&polygon[indexArray[i] * stride]);
	}
	box[0] = m_boxOrigin - m_boxSize;
	box[1] = m_boxOrigin + m_boxSize;

	dgFloat32 skinThickness2 = descInfo.m_skinThickness + DG_MAX_COLLISION_AABB_PADDING;
	skinThickness2 *= skinThickness2;
	dgFloat32 faceSize = descInfo.GetFaceSize(indexArray, indexCount);
	if (faceSize > (dgFloat32 (3.0f) * dgMax(m_boxSize.m_x, m_boxSize.m_y, m_boxSize.m_z))) {
		dgInt32 index = 0;
		dgFloat32 maxValue = face[0].m_y;
		for (dgInt32 j = 1; j < indexCount; j ++) {
			dgFloat32 val = face[j].m_y;
			if (val > maxValue) {
				index = j;
				maxValue = val;
			}
		}
		const dgVector& p = face[index];
		const dgVector& q = box[0];        
		dgVector dir (q - p);

		simplex[0] = dir;
		dgInt32 iter = 0;
		dgInt32 simplexIndex = 1;
		do {
			dgInt32 index = 0;
			dgFloat32 maxValue = face[0] % dir;
			for (dgInt32 j = 1; j < indexCount; j ++) {
				dgFloat32 val = face[j] % dir;
				if (val > maxValue) {
					index = j;
					maxValue = val;
				}
			}
			const dgVector& p = face[index];
			dgVector q ((dir[0] <= dgFloat32 (0.0f)) ? box[1][0] : box[0][0], (dir[1] <= dgFloat32 (0.0f)) ? box[1][1] : box[0][1], (dir[2] <= dgFloat32 (0.0f)) ? box[1][2] : box[0][2], dgFloat32 (0.0f));        

			dgVector diff (q - p);
			simplex[simplexIndex] = diff;
			dgAssert ((diff % diff) > dgFloat32 (0.0f));
			dgFloat32 dist = diff % dir;
			if (dist >= dgFloat32 (-1.0e-5f)) {
				if (descInfo.m_doContinuesCollisionTest) {
					dgFloat32 dist1 = dir % (diff + descInfo.m_distanceTravelInCollidingShapeSpace);
					if (dist1 < dgFloat32 (0.0f)) {
						break;
					}
				}
				return false;
			}

			simplexIndex ++;
			switch (simplexIndex) 
			{
				case 2:
				{
					dir = dgMinkHull::ReduceLineLarge(simplexIndex, simplex, m_dummySum, polygonFaceIndex);
					break;
				}

				case 3:
				{
					dir = dgMinkHull::ReduceTriangleLarge(simplexIndex, simplex, m_dummySum, polygonFaceIndex);
					break;
				}

				case 4:
				{
					dir = dgMinkHull::ReduceTetrahedrumLarge(simplexIndex, simplex, m_dummySum, polygonFaceIndex);
					break;
				}
			}

			iter ++;
		} while ((iter < DG_SEPARATION_PLANES_ITERATIONS) && ((dir % dir) > skinThickness2));

	} else {


		dgInt32 index = 0;
		dgFloat32 maxValue = face[0].m_y;
		for (dgInt32 j = 1; j < indexCount; j ++) {
			dgFloat32 val = face[j].m_y;
			if (val > maxValue) {
				index = j;
				maxValue = val;
			}
		}
		const dgVector& p = face[index];
		const dgVector& q = box[0];        
		dgVector dir (q - p);

		simplex[0] = dir;
		dgInt32 iter = 0;
		dgInt32 simplexIndex = 1;
		do {
			dgInt32 index = 0;
			dgFloat32 maxValue = face[0] % dir;
			for (dgInt32 j = 1; j < indexCount; j ++) {
				dgFloat32 val = face[j] % dir;
				if (val > maxValue) {
					index = j;
					maxValue = val;
				}
			}
			const dgVector& p = face[index];
			dgVector q ((dir[0] <= dgFloat32 (0.0f)) ? box[1][0] : box[0][0], (dir[1] <= dgFloat32 (0.0f)) ? box[1][1] : box[0][1], (dir[2] <= dgFloat32 (0.0f)) ? box[1][2] : box[0][2], dgFloat32 (0.0f));        

			dgVector diff (q - p);
			simplex[simplexIndex] = diff;
			dgAssert ((diff % diff) > dgFloat32 (0.0f));
			dgFloat32 dist = diff % dir;
			if (dist >= dgFloat32 (-1.0e-5f)) {
				if (descInfo.m_doContinuesCollisionTest) {
					dgFloat32 dist1 = dir % (diff + descInfo.m_distanceTravelInCollidingShapeSpace);
					if (dist1 < dgFloat32 (0.0f)) {
						break;
					}
				}
				return false;
			}
		
			simplexIndex ++;
			switch (simplexIndex) 
			{
				case 2:
				{
					dir = dgMinkHull::ReduceLine (simplexIndex, simplex, m_dummySum, polygonFaceIndex);
					break;
				}

				case 3:
				{
					dir = dgMinkHull::ReduceTriangle (simplexIndex, simplex, m_dummySum, polygonFaceIndex);
					break;
				}

				case 4:
				{
					dir = dgMinkHull::ReduceTetrahedrum (simplexIndex, simplex, m_dummySum, polygonFaceIndex);
					break;
				}
			}

			iter ++;
		} while ((iter < DG_SEPARATION_PLANES_ITERATIONS) && ((dir % dir) > skinThickness2));
	}


	return true;
}


bool dgCollisionConvex::SeparetingVectorTest (dgCollisionParamProxy& proxy) const
{
#if 0
	// this is fine but no necessary
	dgCollisionInstance* const collConvexInstance = proxy.m_floatingCollision;
	dgCollisionInstance* const collConicConvexInstance = proxy.m_referenceCollision;

	dgAssert (collConicConvexInstance->m_childShape == this);
	dgAssert (collConicConvexInstance->IsType (dgCollision::dgCollisionConvexShape_RTTI));
	dgCollisionConvex* const convexShape = (dgCollisionConvex*)collConvexInstance->GetChildShape();
	dgContact* const contactJoint = proxy.m_contactJoint;

	dgVector sum;
	dgVector diff;
	ConvexSupportVertex (contactJoint->m_separtingVector, collConvexInstance->m_scale, proxy.m_localMatrixInv, collConicConvexInstance->m_invScale, convexShape, diff, sum);

	dgFloat32 mag2 = diff % diff;
	if (mag2 < dgFloat32 (1.0e-6f)) {
		return false;
	}
	dgVector dir (diff.Scale3 (-dgRsqrt (mag2)));
	dgInt32 iter = 0;	
	do {	
		ConvexSupportVertex (dir, collConvexInstance->m_scale, proxy.m_localMatrixInv, collConicConvexInstance->m_invScale, convexShape, diff, sum);
		dgAssert ((diff % diff) > dgFloat32 (0.0f));
		dgVector negDiff (diff.Scale3(dgFloat32 (-1.0f)));
		negDiff = negDiff.Scale3 (dgRsqrt (negDiff % negDiff));
		dgFloat32 dist = negDiff % dir;
		if (dist >= dgFloat32 (0.0f)) {
			contactJoint->m_separtingVector = dir;
			return true;
		}
		dir -= negDiff.Scale3 (dgFloat32 (2.0f) * (dist));
		dgAssert (dgAbsf(dir % dir - dgFloat32 (1.0f)) < dgFloat32 (1.0e-4f));
		iter ++;
	} while (iter < DG_SEPARATION_PLANES_ITERATIONS);
#endif

	return false;
}




//dgInt32 dgContactSolver::CalculateConvexShapeIntersectionLine (dgMatrix& matrix, const dgVector& normal, dgUnsigned32 id, dgFloat32 penetration,
//	dgInt32 count1, dgVector* const shape1, 
//	dgInt32 count2, dgVector* const shape2, dgContactPoint* const contactOut)
dgInt32 dgCollisionConvex::ConvexPolygonToLineIntersection (const dgVector& normal, dgInt32 count1, dgVector* const shape1, dgInt32 count2, dgVector* const shape2, dgVector* const contactOut, dgVector* const mem) const
{
	dgInt32 count = 0;
	dgVector* output = mem;

	dgAssert (count1 >= 3);
	dgAssert (count2 <= 2);

	dgVector* ptr = NULL;
	// face line intersection
	if (count2 == 2) {
		ptr = (dgVector*)&shape2[0];
		dgInt32 i0 = count1 - 1;
		for (dgInt32 i1 = 0; i1 < count1; i1 ++) {
			dgVector n (normal * (shape1[i1] - shape1[i0]));
			dgAssert ((n % n) > dgFloat32 (0.0f));
			dgPlane plane (n, - (n % shape1[i0]));

			dgFloat32 test0 = plane.Evalue (ptr[0]);
			dgFloat32 test1 = plane.Evalue (ptr[1]);
			if (test0 >= dgFloat32 (0.0f)) {
				if (test1 >= dgFloat32 (0.0f)) {
					output[count + 0] = ptr[0];
					output[count + 1] = ptr[1];
					count += 2;
				} else {
					dgVector dp (ptr[1] - ptr[0]);
					dgFloat32 den = plane % dp;
					if (dgAbsf(den) < 1.0e-10f) {
						den = 1.0e-10f;
					}
					output[count + 0] = ptr[0];
					output[count + 1] = ptr[0] - dp.Scale3 (test0 / den);
					count += 2;
				}
			} else if (test1 >= dgFloat32 (0.0f)) {
				dgVector dp (ptr[1] - ptr[0]);
				dgFloat32 den = plane % dp;
				if (dgAbsf(den) < 1.0e-10f) {
					den = 1.0e-10f;
				}
				output[count] = ptr[0] - dp.Scale3 (test0 / den);
				count ++;
				output[count] = ptr[1];
				count ++;
			} else {
				return 0;
			}


			count2 = count;
			ptr = output;
			output = &output[count];
			count = 0;
			i0 = i1;
			//dgAssert (output < &pool[sizeof (pool)/sizeof (pool[0])]);
		}

	} else if (count2 == 1){
		const dgVector& p = shape2[0]; 
		dgInt32 i0 = count1 - 1;
		for (dgInt32 i1 = 0; i1 < count1; i1 ++) {
			dgVector n (normal * (shape1[i1] - shape1[i0]));
			dgAssert ((n % n) > dgFloat32 (0.0f));
			dgPlane plane (n, - (n % shape1[i0]));
			dgFloat32 test0 = plane.Evalue (p);
			if (test0 < dgFloat32 (-1.e-3f)) {
				return 0;
			}
			i0 = i1;
		}
		ptr = output;
		output[count] = p;
		count ++;

	} else {
		count2 = 0;
	}

//	dgVector normal = matrix.RotateVector(normal);
	for (dgInt32 i0 = 0; i0 < count2; i0 ++) {
//		contactOut[i0].m_point = matrix.TransformVector(ptr[i0]);
//		contactOut[i0].m_normal = normal;
//		contactOut[i0].m_penetration = penetration;
//		contactOut[i0].m_userId = id;
//		contactOut[i0].m_isEdgeContact = 0;
		contactOut[i0] = ptr[i0];
	}
	return count2;
}




//dgInt32 ConvexShapesIntersection (const dgMatrix& matrix, const dgVector& normal, dgUnsigned32 id, dgFloat32 penetration,
//	dgInt32 count1, dgVector* const shape1, dgInt32 count2, 
//	dgVector* const shape2, dgContactPoint* const contactOut, dgInt32 maxContacts)
dgInt32 dgCollisionConvex::ConvexPolygonsIntersection (const dgVector& normal, dgInt32 count1, dgVector* const shape1, dgInt32 count2, dgVector* const shape2, dgVector* const contactOut, dgInt32 maxContacts) const
{
	dgInt32 count = 0;
	if (count2 <= 2) {
		count = ConvexPolygonToLineIntersection (normal, count1, shape1, count2, shape2, contactOut, &contactOut[count1 + count2 + maxContacts]);
	} else if (count1 <= 2) {
		count = ConvexPolygonToLineIntersection (normal, count2, shape2, count1, shape1, contactOut, &contactOut[count1 + count2 + maxContacts]);
	} else {
		dgAssert (count1 >= 3);
		dgAssert (count2 >= 3);

		dgPerimenterEdge subdivision[128];
		dgAssert ((2 * (count1 + count2)) < dgInt32 (sizeof (subdivision) / sizeof (subdivision[0])));
		for (dgInt32 i0 = 0; i0 < count2; i0 ++) {
			subdivision[i0].m_vertex = &shape2[i0];
			subdivision[i0].m_prev = &subdivision[i0 - 1];
			subdivision[i0].m_next = &subdivision[i0 + 1];
		}
		subdivision[0].m_prev = &subdivision[count2 - 1];
		subdivision[count2 - 1].m_next = &subdivision[0];

		dgPerimenterEdge* edgeClipped[2];
		dgVector* output = &contactOut[count1 + count2 + maxContacts];
		
		edgeClipped[0] = NULL;
		edgeClipped[1] = NULL;
		dgInt32 i0 = count1 - 1;
		dgInt32 edgeIndex = count2;
		dgPerimenterEdge* poly = &subdivision[0];
		for (dgInt32 i1 = 0; i1 < count1; i1 ++) {
			dgVector n (normal * (shape1[i1] - shape1[i0]));
			dgPlane plane (n, - (n % shape1[i0]));
			i0 = i1;
			count = 0;
			dgPerimenterEdge* tmp = poly;
			dgInt32 isInside = 0;
			dgFloat32 test0 = plane.Evalue (*tmp->m_vertex);
			do {
				dgFloat32 test1 = plane.Evalue (*tmp->m_next->m_vertex);

				if (test0 >= dgFloat32 (0.0f)) {
					isInside |= 1;
					if (test1 < dgFloat32 (0.0f)) {
						const dgVector& p0 = *tmp->m_vertex;
						const dgVector& p1 = *tmp->m_next->m_vertex;
						dgVector dp (p1 - p0); 
						dgFloat32 den = plane % dp;
						if (dgAbsf(den) < dgFloat32 (1.0e-24f)) {
							den = (den >= dgFloat32 (0.0f)) ?  dgFloat32 (1.0e-24f) :  dgFloat32 (-1.0e-24f);
						}

						den = test0 / den;
						if (den >= dgFloat32 (0.0f)) {
							den = dgFloat32 (0.0f);
						} else if (den <= -1.0f) {
							den = dgFloat32 (-1.0f);
						}
						output[0] = p0 - dp.Scale3 (den);
						edgeClipped[0] = tmp;
						count ++;
					}
				} else if (test1 >= dgFloat32 (0.0f)) {
					const dgVector& p0 = *tmp->m_vertex;
					const dgVector& p1 = *tmp->m_next->m_vertex;
					isInside |= 1;
					dgVector dp (p1 - p0); 
					dgFloat32 den = plane % dp;
					if (dgAbsf(den) < dgFloat32 (1.0e-24f)) {
						den = (den >= dgFloat32 (0.0f)) ?  dgFloat32 (1.0e-24f) :  dgFloat32 (-1.0e-24f);
					}
					den = test0 / den;
					if (den >= dgFloat32 (0.0f)) {
						den = dgFloat32 (0.0f);
					} else if (den <= -1.0f) {
						den = dgFloat32 (-1.0f);
					}
					output[1] = p0 - dp.Scale3 (den);
					edgeClipped[1] = tmp;
					count ++;
				}

				test0 = test1;
				tmp = tmp->m_next;
			} while (tmp != poly && (count < 2));

			if (!isInside) {
				return 0;
			}

			if (count == 2) {
				dgPerimenterEdge* const newEdge = &subdivision[edgeIndex];
				newEdge->m_next = edgeClipped[1];
				newEdge->m_prev = edgeClipped[0];
				edgeClipped[0]->m_next = newEdge;
				edgeClipped[1]->m_prev = newEdge;

				newEdge->m_vertex = &output[0];
				edgeClipped[1]->m_vertex = &output[1];
				poly = newEdge;

				output += 2;
				edgeIndex ++;
				//dgAssert (output < &pool[sizeof (pool)/sizeof (pool[0])]);
				dgAssert (edgeIndex < dgInt32 (sizeof (subdivision) / sizeof (subdivision[0])));
			}
		}

		dgAssert (poly);
		poly = ReduceContacts (poly, maxContacts);
		count = 0;
		dgPerimenterEdge* intersection = poly;
		//dgVector normal = matrix.RotateVector(normal);
		do {
			//contactOut[count].m_point = matrix.TransformVector(*intersection->m_vertex);
			//contactOut[count].m_normal = normal;
			//contactOut[count].m_penetration = penetration;
			//contactOut[count].m_isEdgeContact = 0;
			contactOut[count] = *intersection->m_vertex;
			count ++;
			intersection = intersection->m_next;
		} while (intersection != poly);
	}
	return count;
}



dgInt32 dgCollisionConvex::CalculateContactsGeneric (const dgVector& point, const dgVector& normal, dgCollisionParamProxy& proxy, dgVector* const contactsOut) const
{
	dgInt32 count = 0; 
	const dgInt32 baseCount = 8;

	const dgCollisionInstance* const myInstance = proxy.m_referenceCollision;
	const dgCollisionInstance* const convexInstance = proxy.m_floatingCollision;
	dgAssert (myInstance->m_childShape == this);
	const dgCollisionConvex* const otherShape = (dgCollisionConvex*)convexInstance->m_childShape;
	dgAssert (otherShape->IsType(dgCollisionConvexShape_RTTI));

	dgInt32 count1 = 0;
	dgVector* const shape1 = &contactsOut[baseCount];
	dgVector support0 (SupportVertex(normal.Scale3(dgFloat32(-1.0f)), NULL));
	dgFloat32 dist = normal % (support0 - point);
	if (dist >= (-DG_ROBUST_PLANE_CLIP)) {
		support0 += normal.Scale3 (DG_ROBUST_PLANE_CLIP);
		count1 = CalculatePlaneIntersection (normal, support0, shape1);
		dgVector err (normal.Scale3 (normal % (point - support0)));
		for (dgInt32 i = 0; i < count1; i ++) {
			shape1[i] += err;
		}
	} else {
		count1 = CalculatePlaneIntersection (normal, point, shape1);
		if (!count1) {
			dgVector support1 (SupportVertex(normal, NULL));
			dgVector center ((support0 + support1).Scale3 (dgFloat32 (0.5f)));
			count1 = CalculatePlaneIntersection (normal, center, shape1);
		}
	}
	if (count1) {
		const dgVector& myScale = myInstance->m_scale;
		const dgVector& myInvScale = myInstance->m_invScale;
		const dgVector& scale = convexInstance->m_scale;
		const dgVector& invScale = convexInstance->m_invScale;
		const dgMatrix& matrix = proxy.m_matrix;
		dgVector n (scale.CompProduct4(matrix.UnrotateVector(myInvScale.CompProduct4 (normal))));
		dgVector p (invScale.CompProduct4 (matrix.UntransformVector(myScale.CompProduct4(point))));
		n = n.Scale3 (dgRsqrt (n % n));

		dgInt32 count2 = 0;
		dgVector* const shape2 = &contactsOut[baseCount + count1];
		dgInt32 index;
		dgVector support0 (otherShape->SupportVertex(n, &index));
		dist = n % (support0 - p);
		if (dist < DG_ROBUST_PLANE_CLIP) {
			support0 += n.Scale3 (-DG_ROBUST_PLANE_CLIP);
			count2 = otherShape->CalculatePlaneIntersection (n, support0, shape2);
			dgVector err (n.Scale3 (n % (p - support0)));
			for (dgInt32 i = 0; i < count2; i ++) {
				shape2[i] += err;
			}
		} else {
			count2 = otherShape->CalculatePlaneIntersection (n, p, shape2);
			if (!count2) {
				dgVector support1 (otherShape->SupportVertex(n.Scale3 (dgFloat32 (-1.0f)), &index));
				dgVector center ((support0 + support1).Scale3 (dgFloat32 (0.5f)));
				count2 = otherShape->CalculatePlaneIntersection (n, center, shape2);
			}
		}
		if (count2) {
			dgAssert (count2);
			for (dgInt32 i = 0; i < count2; i ++) {
				shape2[i] = myInvScale.CompProduct4 (matrix.TransformVector (scale.CompProduct4(shape2[i])));
			}

			if (count1 == 1){
				count = 1;
				contactsOut[0] = shape1[0];
			} else if (count2 == 1) {
				count = 1;
				contactsOut[0] = shape2[0];
			} else if ((count1 == 2) && (count2 == 2)) {
				dgVector p0 (shape1[0]); 
				dgVector p1 (shape1[1]); 
				const dgVector& q0 = shape2[0]; 
				const dgVector& q1 = shape2[1]; 
				dgVector p10 (p1 - p0);
				dgVector q10 (q1 - q0);
				p10 = p10.Scale3 (dgRsqrt (p10 % p10 + dgFloat32 (1.0e-8f)));
				q10 = q10.Scale3 (dgRsqrt (q10 % q10 + dgFloat32 (1.0e-8f)));
				dgFloat32 dot = q10 % p10;
				if (dgAbsf (dot) > dgFloat32 (0.998f)) {
					dgFloat32 pl0 = p0 % p10;
					dgFloat32 pl1 = p1 % p10;
					dgFloat32 ql0 = q0 % p10;
					dgFloat32 ql1 = q1 % p10;
					if (pl0 > pl1) {
						dgSwap (pl0, pl1);
						dgSwap (p0, p1);
						p10 = p10.Scale3 (dgFloat32 (-1.0f));
					}
					if (ql0 > ql1) {
						dgSwap (ql0, ql1);
					}
					if ( !((ql0 > pl1) && (ql1 < pl0))) {
						dgFloat32 clip0 = (ql0 > pl0) ? ql0 : pl0;
						dgFloat32 clip1 = (ql1 < pl1) ? ql1 : pl1;

						count = 2;
						contactsOut[0] = p0 + p10.Scale3 (clip0 - pl0);
						contactsOut[1] = p0 + p10.Scale3 (clip1 - pl0);
					}
				} else {
					count = 1;
					dgVector c0;
					dgVector c1;
					dgRayToRayDistance (p0, p1, q0, q1, c0, c1);
					contactsOut[0] = (c0 + c1).Scale3 (dgFloat32 (0.5f));
				}

			} else {
				dgAssert ((count1 >= 2) && (count2 >= 2));
				count = ConvexPolygonsIntersection (normal, count1, shape1, count2, shape2, contactsOut, baseCount);
			}
		}
	}
	return count;
}





bool dgCollisionConvex::CalculateClosestPoints (dgCollisionParamProxy& proxy) const
{
	dgAssert (this == proxy.m_referenceCollision->m_childShape);
	dgMinkHull minkHull (proxy);
	minkHull.CalculateClosestPoints ();

	dgContactPoint* const contactOut = proxy.m_contacts;
	dgCollisionInstance* const collConicConvexInstance = proxy.m_referenceCollision;
	dgVector p (ConvexConicSupporVertex(minkHull.m_p, minkHull.m_normal));

	const dgVector& scale = collConicConvexInstance->m_scale;
	const dgVector& invScale = collConicConvexInstance->m_invScale;
	const dgMatrix& matrix = collConicConvexInstance->GetGlobalMatrix();
	dgVector normal (matrix.RotateVector(invScale.CompProduct4(minkHull.m_normal)));
	normal = normal.Scale3(dgRsqrt(normal % normal));

	contactOut[0].m_normal = normal;
	contactOut[0].m_point = matrix.TransformVector(scale.CompProduct4(p));

	contactOut[1].m_normal = normal.Scale4 (dgFloat32 (-1.0f));
	contactOut[1].m_point = matrix.TransformVector(scale.CompProduct4(minkHull.m_q));

	dgContact* const contactJoint = proxy.m_contactJoint;
	contactJoint->m_closestDistance = (contactOut[1].m_point - contactOut[0].m_point).DotProduct4 (normal).m_x;

	return true;
}


dgInt32 dgCollisionConvex::CalculateConvexToConvexContact (dgCollisionParamProxy& proxy) const
{
	dgInt32 count = 0;

	dgAssert (this == proxy.m_referenceCollision->m_childShape);
	dgMinkHull minkHull (proxy);

	minkHull.CalculateClosestPoints ();
	minkHull.m_p = ConvexConicSupporVertex(minkHull.m_p, minkHull.m_normal);
	dgFloat32 penetration = minkHull.m_normal % (minkHull.m_p - minkHull.m_q) + proxy.m_skinThickness;
	if (penetration >= dgFloat32 (0.0f)) {
		penetration = dgMax(penetration - DG_IMPULSIVE_CONTACT_PENETRATION, dgFloat32 (0.0f));
		dgVector contactPoint ((minkHull.m_p + minkHull.m_q).Scale3 (dgFloat32 (0.5f)));
		count = CalculateContacts (contactPoint, minkHull.m_normal.Scale3 (-1.0f), proxy, minkHull.m_hullDiff);
	}
	proxy.m_contactJoint->m_closestDistance = - penetration;
	
	if (count) {
		count = dgMin(proxy.m_maxContacts, count);
		dgContactPoint* const contactOut = proxy.m_contacts;
		dgCollisionInstance* const collConicConvexInstance = proxy.m_referenceCollision;

		const dgVector& scale = collConicConvexInstance->m_scale;
		const dgVector& invScale = collConicConvexInstance->m_invScale;
		const dgMatrix& matrix = collConicConvexInstance->GetGlobalMatrix();
		dgVector normal (matrix.RotateVector(invScale.CompProduct4(minkHull.m_normal.Scale3 (-1.0f))));
		normal = normal.Scale3(dgRsqrt(normal % normal));
		for (dgInt32 i = 0; i < count; i ++) {
			contactOut[i].m_point = matrix.TransformVector(scale.CompProduct3(minkHull.m_hullDiff[i]));
			contactOut[i].m_normal = normal;
			contactOut[i].m_penetration = penetration;
		}
	}

	dgCollisionInstance* const collConicConvexInstance = proxy.m_referenceCollision;
	const dgVector& scale = collConicConvexInstance->m_scale;
	const dgVector& invScale = collConicConvexInstance->m_invScale;
	const dgMatrix& matrix = collConicConvexInstance->GetGlobalMatrix();

	proxy.m_normal = matrix.RotateVector(invScale.CompProduct4(minkHull.m_normal.Scale3 (-1.0f)));
	proxy.m_normal = proxy.m_normal.Scale3(dgRsqrt(proxy.m_normal % proxy.m_normal ));
	proxy.m_closestPointBody0 = matrix.TransformVector(scale.CompProduct3(minkHull.m_p));
	proxy.m_closestPointBody1 = matrix.TransformVector(scale.CompProduct3(minkHull.m_q));
	return count;
}



dgInt32 dgCollisionConvex::CalculateConvexCastContacts(dgCollisionParamProxy& proxy) const
{
	dgBody* const floatingBody = proxy.m_floatingBody;
	dgBody* const referenceBody = proxy.m_referenceBody;

	dgVector floatingVeloc (floatingBody->m_veloc);
	dgVector referenceVeloc (referenceBody->m_veloc);

	dgCollisionInstance* const collConicConvexInstance = proxy.m_referenceCollision;
	const dgVector& scale = collConicConvexInstance->m_scale;
	const dgVector& invScale = collConicConvexInstance->m_invScale;
	const dgMatrix& matrix = collConicConvexInstance->GetGlobalMatrix();
	dgInt32 isNonUniformScale = !collConicConvexInstance->m_scaleIsUniform;

	dgVector veloc (matrix.UnrotateVector(floatingVeloc - referenceVeloc));
	//dgAssert ((veloc % veloc) > dgFloat32 (0.0f));

	dgInt32 iter = 0;
	dgInt32 count = 0;
	dgFloat32 tacc = dgFloat32 (0.0f);
	dgFloat32 timestep = proxy.m_timestep;
	dgMinkHull minkHull (proxy);
	do {
		minkHull.CalculateClosestPoints ();
		dgVector normal (minkHull.m_normal);
		if (isNonUniformScale) {
			normal = normal.CompProduct4(invScale);
			normal = normal.Scale3(dgRsqrt (normal % normal));
		}
		dgFloat32 den = normal % veloc;
		if (den >= dgFloat32 (0.0f)) {
			// bodies are residing form each other, even if they are touching they are not considered to be colliding because the motion will move them apart 
			// get the closet point and the normal at contact point
			count = 0;
			proxy.m_timestep = timestep;
			proxy.m_normal = matrix.RotateVector(invScale.CompProduct4(minkHull.m_normal.Scale3 (-1.0f)));
			proxy.m_normal = proxy.m_normal.Scale3(dgRsqrt(proxy.m_normal % proxy.m_normal ));
			proxy.m_closestPointBody0 = matrix.TransformVector(scale.CompProduct3(minkHull.m_p));
			proxy.m_closestPointBody1 = matrix.TransformVector(scale.CompProduct3(minkHull.m_q));
			break;
		}

		minkHull.m_p = ConvexConicSupporVertex(minkHull.m_p, minkHull.m_normal);
		dgVector diff (scale.CompProduct3(minkHull.m_q - minkHull.m_p));
		dgFloat32 num = minkHull.m_normal % diff;
		if (num <= dgFloat32 (0.0f)) {
			// bodies collide at time tacc, but we do not set it yet
			//proxy.m_normal = matrix.RotateVector(invScale.CompProduct4(minkHull.m_normal.Scale3 (-1.0f)));
			//proxy.m_normal = proxy.m_normal.Scale3(dgRsqrt(proxy.m_normal % proxy.m_normal ));
			proxy.m_normal = matrix.RotateVector(normal.Scale3 (-1.0f));
			dgVector step (veloc.Scale3(tacc));
			proxy.m_closestPointBody0 = matrix.TransformVector(scale.CompProduct3(minkHull.m_p));
			proxy.m_closestPointBody1 = matrix.TransformVector(scale.CompProduct3(minkHull.m_q - step));

			if (proxy.m_contacts) {
				dgFloat32 penetration = dgMax(num * dgFloat32 (-1.0f) - DG_IMPULSIVE_CONTACT_PENETRATION, dgFloat32 (0.0f));
				dgVector contactPoint ((minkHull.m_p + minkHull.m_q).Scale3 (dgFloat32 (0.5f)));
				count = CalculateContacts (contactPoint, minkHull.m_normal.Scale3 (-1.0f), proxy, minkHull.m_hullDiff);
				if (count) {
					proxy.m_timestep = tacc;
					count = dgMin(proxy.m_maxContacts, count);
					dgContactPoint* const contactOut = proxy.m_contacts;
					for (dgInt32 i = 0; i < count; i ++) {
						contactOut[i].m_point = matrix.TransformVector(scale.CompProduct3(minkHull.m_hullDiff[i] - step));
						contactOut[i].m_normal = proxy.m_normal;
						contactOut[i].m_penetration = penetration;
						contactOut[i].m_shapeId0 = collConicConvexInstance->GetUserDataID();
						contactOut[i].m_shapeId1 = proxy.m_shapeFaceID;
					}
				}
			} else {
				proxy.m_timestep = tacc;
			}
			break;
		}

		num += DG_RESTING_CONTACT_PENETRATION; 
		tacc -= (num / den); 
		if (tacc >= timestep) {
			// object do not collide on this timestep
			count = 0;
			proxy.m_timestep = timestep;
			proxy.m_normal = matrix.RotateVector(invScale.CompProduct4(minkHull.m_normal.Scale3 (-1.0f)));
			proxy.m_normal = proxy.m_normal.Scale3(dgRsqrt(proxy.m_normal % proxy.m_normal ));
			proxy.m_closestPointBody0 = matrix.TransformVector(scale.CompProduct3(minkHull.m_p));
			proxy.m_closestPointBody1 = matrix.TransformVector(scale.CompProduct3(minkHull.m_q));
			break;
		}
		minkHull.m_matrix.m_posit = proxy.m_matrix.m_posit + veloc.Scale3(tacc);

		iter ++;
	} while (iter < DG_SEPARATION_PLANES_ITERATIONS);

	return count;
}
