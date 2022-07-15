/* Copyright (c) <2003-2021> <Newton Game Dynamics>
* 
* This software is provided 'as-is', without any express or implied
* warranty. In no event will the authors be held liable for any damages
* arising from the use of this software.
* 
* Permission is granted to anyone to use this software for any purpose,
* including commercial applications, and to alter it and redistribute it
* freely
*/

#include "ndSandboxStdafx.h"
#include "ndSkyBox.h"
#include "ndTargaToOpenGl.h"
#include "ndDemoMesh.h"
#include "ndDemoCamera.h"
#include "ndLoadFbxMesh.h"
#include "ndPhysicsUtils.h"
#include "ndPhysicsWorld.h"
#include "ndMakeStaticMap.h"
#include "ndAnimationPose.h"
#include "ndAnimationSequence.h"
#include "ndDemoEntityManager.h"
#include "ndDemoInstanceEntity.h"
#include "ndAnimationSequencePlayer.h"

#define D_USE_FORWARD_DYNAMICS

class dAiBotTest_1 : public ndModel
{
	public:
	D_CLASS_REFLECTION(dAiBotTest_1);

	class ndEffectorInfo
	{
		public:
		ndEffectorInfo()
			:m_basePosition(ndVector::m_wOne)
			,m_effector(nullptr)
			,m_swivel(0.0f)
			,m_x(0.0f)
			,m_y(0.0f)
			,m_z(0.0f)
		{
		}

		ndEffectorInfo(ndIkSwivelPositionEffector* const effector)
			:m_basePosition(effector->GetPosition())
			,m_effector(effector)
			,m_swivel(0.0f)
			,m_x(0.0f)
			,m_y(0.0f)
			,m_z(0.0f)
		{
		}

		ndVector m_basePosition;
		ndIkSwivelPositionEffector* m_effector;
		ndReal m_swivel;
		ndReal m_x;
		ndReal m_y;
		ndReal m_z;
	};
	
	dAiBotTest_1(ndDemoEntityManager* const scene, const ndMatrix& location)
		:ndModel()
		//,m_solver()
		//,m_bodies()
		//,m_effector(nullptr)
		//,m_contactSensor(nullptr)
		//,m_efectorLength(1.0f)
	{
		ndFloat32 mass = 10.0f;
		ndFloat32 radius = 0.25f;
		ndFloat32 limbMass = 0.5f;
		ndFloat32 limbLength = 0.4f;
		ndFloat32 limbRadios = 0.06f;

		ndPhysicsWorld* const world = scene->GetWorld();
		ndBodyKinematic* const torso = AddSphere(scene, location, mass, radius, "smilli.tga");
		m_rootBody = torso->GetAsBodyDynamic();

		ndDemoEntity* const entity = (ndDemoEntity*) torso->GetNotifyCallback()->GetUserData();
		entity->SetMeshMatrix(dYawMatrix(90.0f * ndDegreeToRad) * dPitchMatrix(90.0f * ndDegreeToRad));

		ndMatrix matrix(dRollMatrix(20.0f * ndDegreeToRad));
		matrix.m_posit.m_x = radius + limbLength * 0.5f;

		//ndFloat32 angles[] = { 60.0f, 120.0f, 240.0f, 300.0f };
		ndFloat32 angles[] = { 300.0f, 240.0f, 120.0f, 60.0f };

		for (ndInt32 i = 0; i < 4; i++)
		{
			ndMatrix limbLocation(matrix * dYawMatrix(angles[i] * ndDegreeToRad));

			// add leg thigh
			limbLocation.m_posit += torso->GetMatrix().m_posit;
			limbLocation.m_posit.m_w = 1.0f;
			ndBodyKinematic* const thigh = AddCapsule(scene, limbLocation, limbMass, limbRadios, limbRadios, limbLength);
			thigh->SetMatrix(limbLocation);
			ndVector thighPivot(limbLocation.m_posit - limbLocation.m_front.Scale(limbLength * 0.5f));
			ndMatrix thighFrame(limbLocation);
			thighFrame.m_posit = thighPivot;
			ndIkJointSpherical* const ball = new ndIkJointSpherical(thighFrame, thigh, torso);
			world->AddJoint(ball);

			// add calf
			ndVector caffPivot(limbLocation.m_posit + limbLocation.m_front.Scale(limbLength * 0.5f));
			limbLocation = dRollMatrix((-80.0f - 20.0f) * ndDegreeToRad) * limbLocation;
			caffPivot += limbLocation.m_front.Scale(limbLength * 0.5f);

			limbLocation.m_posit = caffPivot;
			ndBodyKinematic* const caff = AddCapsule(scene, limbLocation, limbMass, limbRadios, limbRadios, limbLength);
			caff->SetMatrix(limbLocation);

			ndMatrix caffPinAndPivotFrame(limbLocation.m_right);
			caffPinAndPivotFrame.m_posit = limbLocation.m_posit - limbLocation.m_front.Scale(limbLength * 0.5f);
			ndIkJointHinge* const hinge = new ndIkJointHinge(caffPinAndPivotFrame, caff, thigh);
			world->AddJoint(hinge);

			// add leg effector
			ndVector effectorToePosit(limbLocation.m_posit + limbLocation.m_front.Scale(limbLength * 0.5f));

			ndMatrix effectorToeFrame(dGetIdentityMatrix());
			ndMatrix effectorRefFrame(dGetIdentityMatrix());
			ndMatrix effectorSwivelFrame(dGetIdentityMatrix());

			effectorRefFrame.m_posit = thighPivot;
			effectorToeFrame.m_posit = effectorToePosit;
			effectorSwivelFrame.m_posit = thighPivot;
			effectorSwivelFrame.m_front = (effectorToeFrame.m_posit - effectorRefFrame.m_posit).Normalize();
			effectorSwivelFrame.m_up = ndVector(0.0f, 1.0f, 0.0f, 0.0f);
			effectorSwivelFrame.m_right = (effectorSwivelFrame.m_front.CrossProduct(effectorSwivelFrame.m_up)).Normalize();
			effectorSwivelFrame.m_up = effectorSwivelFrame.m_right.CrossProduct(effectorSwivelFrame.m_front);

			ndIkSwivelPositionEffector* const effector = new ndIkSwivelPositionEffector(effectorToeFrame, effectorRefFrame, effectorSwivelFrame, caff, torso);
			world->AddJoint(effector);
			m_effectors.PushBack(ndEffectorInfo(effector));
		}
	}

	dAiBotTest_1(const ndLoadSaveBase::ndLoadDescriptor& desc)
		:ndModel(ndLoadSaveBase::ndLoadDescriptor(desc))
	{
		dAssert(0);
	}

	~dAiBotTest_1()	
	{
	}

	//void Save(const ndLoadSaveBase::ndSaveDescriptor& desc) const
	void Save(const ndLoadSaveBase::ndSaveDescriptor&) const
	{
		dAssert(0);
	}

	void Debug(ndConstraintDebugCallback& context) const
	{
		for (ndInt32 i = 0; i < 4; i++)
		{
			const ndEffectorInfo& info = m_effectors[i];
			ndJointBilateralConstraint* const effector = info.m_effector;
			effector->DebugJoint(context);
			ndMatrix swivelMatrix0;
			ndMatrix swivelMatrix1;
			info.m_effector->CalculateSwivelMatrices(swivelMatrix0, swivelMatrix1);
			
			ndVector posit1(swivelMatrix1.m_posit);
			posit1.m_y += 1.0f;
			context.DrawLine(swivelMatrix1.m_posit, posit1, ndVector(ndFloat32(0.0f), ndFloat32(0.0f), ndFloat32(0.0f), ndFloat32(1.0f)));
			
			//ndVector upVector(0.0f, 1.0f, 0.0f, 0.0f);
			//const ndFloat32 angle2 = info.m_effector->CalculateAngle(upVector, swivelMatrix1[1], swivelMatrix1[0]);
			//swivelMatrix1 = dPitchMatrix(info.m_effector->GetSwivelAngle()) * swivelMatrix1;
			//const ndFloat32 angle = info.m_effector->CalculateAngle(swivelMatrix0[1], swivelMatrix1[1], swivelMatrix1[0]);
			//ndMatrix xxxx1(dGetIdentityMatrix());
			//ndMatrix xxxx0(swivelMatrix0 * swivelMatrix1.Inverse());
			//ndFloat32 angle1 = info.m_effector->CalculateAngle(xxxx0[1], xxxx1[1], xxxx1[0]);
			//dTrace(("%f %f %f\n", angle * ndRadToDegree, angle1 * ndRadToDegree, angle2 * ndRadToDegree));
		}
	}

	void PostUpdate(ndWorld* const world, ndFloat32 timestep)
	{
		ndModel::PostUpdate(world, timestep);
	}

	void PostTransformUpdate(ndWorld* const world, ndFloat32 timestep)
	{
		ndModel::PostTransformUpdate(world, timestep);
	}

	//ndVector CalculateCenterOfMass() const
	//{
	//	ndFloat32 toltalMass = 0.0f;
	//	ndVector com(ndVector::m_zero);
	//	//comVeloc = ndVector::m_zero;
	//	for (ndInt32 i = 0; i < m_bodies.GetCount(); ++i)
	//	{
	//		ndBodyDynamic* const body = m_bodies[i];
	//		ndFloat32 mass = body->GetMassMatrix().m_w;
	//		ndVector comMass(body->GetMatrix().TransformVector(body->GetCentreOfMass()));
	//		com += comMass.Scale(mass);
	//		//comVeloc += body->GetVelocity().Scale(mass);
	//		toltalMass += mass;
	//	}
	//	com = com.Scale(1.0f / toltalMass) & ndVector::m_triplexMask;
	//	//comVeloc = comVeloc.Scale(1.0f / toltalMass) & ndVector::m_triplexMask;;
	//	return com | ndVector::m_wOne;
	//}

	void Update(ndWorld* const world, ndFloat32 timestep)
	{
		ndModel::Update(world, timestep);

		ndVector upVector(m_rootBody->GetMatrix().m_up);
		for (ndInt32 i = 0; i < m_effectors.GetCount(); i++)
		{
			ndEffectorInfo& info = m_effectors[i];
			ndVector posit(info.m_basePosition);
			posit.m_x += info.m_x * 0.25f;
			posit.m_y += info.m_y * 0.25f;
			posit.m_z += info.m_z * 0.2f;
			info.m_effector->SetPosition(posit);

			#if 1
			ndMatrix swivelMatrix0;
			ndMatrix swivelMatrix1;
			info.m_effector->CalculateSwivelMatrices(swivelMatrix0, swivelMatrix1);
			const ndFloat32 angle = info.m_effector->CalculateAngle(upVector, swivelMatrix1[1], swivelMatrix1[0]);
			info.m_effector->SetSwivelAngle(info.m_swivel - angle);
			#else
			info.m_effector->SetSwivelAngle(info.m_swivel);
			#endif
		}
	}

	void ApplyControls(ndDemoEntityManager* const scene)
	{
		ndVector color(1.0f, 1.0f, 0.0f, 0.0f);
		scene->Print(color, "Control panel");

		ndEffectorInfo& info = m_effectors[0];

		bool change = false;
		ImGui::Text("position x");
		change = change | ImGui::SliderFloat("##x", &info.m_x, -1.0f, 1.0f);
		ImGui::Text("position y");
		change = change | ImGui::SliderFloat("##y", &info.m_y, -1.0f, 1.0f);
		ImGui::Text("position z");
		change = change | ImGui::SliderFloat("##z", &info.m_z, -1.0f, 1.0f);

		ImGui::Text("swivel");
		change = change | ImGui::SliderFloat("##swivel", &info.m_swivel, -1.0f, 1.0f);
		
		if (change)
		{
			m_rootBody->SetSleepState(false);

			for (ndInt32 i = 1; i < m_effectors.GetCount(); ++i)
			{
				m_effectors[i].m_x = info.m_x;
				m_effectors[i].m_y = info.m_y;
				m_effectors[i].m_z = info.m_z;
				m_effectors[i].m_swivel = info.m_swivel;
			}
		}
	}

	static void ControlPanel(ndDemoEntityManager* const scene, void* const context)
	{
		dAiBotTest_1* const me = (dAiBotTest_1*)context;
		me->ApplyControls(scene);
	}
	
	ndBodyDynamic* m_rootBody;
	ndFixSizeArray<ndEffectorInfo, 4> m_effectors;
};
D_CLASS_REFLECTION_IMPLEMENT_LOADER(dAiBotTest_1);



class dInvertedPendulum : public ndModel
{
	public:
	D_CLASS_REFLECTION(dInvertedPendulum);

	dInvertedPendulum(ndDemoEntityManager* const scene, const ndMatrix& location)
		:ndModel()
		,m_gravityDir(0.0f, -1.0f, 0.0f, 0.0f)
		,m_solver()
		,m_bodies()
		,m_effector(nullptr)
		,m_contactSensor(nullptr)
		,m_efectorLength(1.0f)
	{
		ndFloat32 mass = 1.0f;
		ndFloat32 sizex = 0.5f;
		ndFloat32 sizey = 1.0f;
		ndFloat32 sizez = 0.5f;
		ndFloat32 radius = 0.125f * sizex;
		
		ndPhysicsWorld* const world = scene->GetWorld();
		ndVector floor(FindFloor(*world, location.m_posit + ndVector(0.0f, 100.0f, 0.0f, 0.0f), 200.0f));
		ndBodyKinematic* const box = AddBox(scene, location, mass, sizex, sizey, sizez);

		ndMatrix boxMatrix(location);
		boxMatrix.m_posit.m_y = floor.m_y;
		boxMatrix.m_posit.m_y += 0.5f;
		ndShapeInfo sizeInfo (box->GetCollisionShape().GetShape()->GetShapeInfo());
		boxMatrix.m_posit.m_y += sizeInfo.m_box.m_y * 0.5f + radius + m_efectorLength;
		box->SetMatrix(boxMatrix);
		box->GetNotifyCallback()->OnTransform(0, boxMatrix);
		box->GetNotifyCallback()->OnTransform(0, boxMatrix);
		
		//ndBodyKinematic* const leg = AddCapsule(scene, matrix, mass / 20.0f, radius, radius, 2.0f * size);
		ndBodyKinematic* const sph = AddSphere(scene, location, mass / 20.0f, radius);
		ndMatrix sphMatrix(box->GetMatrix());
		sphMatrix.m_posit.m_y -= sizeInfo.m_box.m_y * 0.5f + m_efectorLength;
		// try offsetting the effector.
		sphMatrix.m_posit.m_z -= (sizez * 0.5f) * 1.0f;
		sph->SetMatrix(sphMatrix);
		sph->GetNotifyCallback()->OnTransform(0, sphMatrix);
		sph->GetNotifyCallback()->OnTransform(0, sphMatrix);

// hack to show equilibrium can be dynamics.
//box->SetAngularDamping(ndVector(1.0f, 1.0f, 1.0f, 0.0f));

		////sph->GetCollisionShape().SetCollisionMode(false);
		////ndIkJointSpherical* const feetJoint = new ndIkJointSpherical(sphMatrix, sph, leg);
		////world->AddJoint(feetJoint);
		
		////ndMatrix legSocketMatrix(legMatrix);
		////legSocketMatrix.m_posit = matrix.m_posit;
		////ndIkJointSpherical* const socketJoint = new ndIkJointSpherical(legSocketMatrix, leg, box);
		////world->AddJoint(socketJoint);
		
		ndMatrix boxPivot(box->GetMatrix());
		boxPivot.m_posit.m_y -= sizeInfo.m_box.m_y * 0.5f;
		boxPivot.m_posit.m_z = sphMatrix.m_posit.m_z;
		m_effector = new ndIk6DofEffector(sphMatrix, boxPivot, sph, box);
		ndFloat32 regularizer = 1.0e-2f;
		m_effector->EnableRotationAxis(ndIk6DofEffector::m_shortestPath);
		m_effector->SetLinearSpringDamper(regularizer, 1500.0f, 100.0f);
		m_effector->SetAngularSpringDamper(regularizer, 1500.0f, 100.0f);
		m_effector->SetSolverModel(ndJointBilateralSolverModel::m_jointkinematicOpenLoop);
		
		//feetJoint->SetIkMode(false);
		//socketJoint->SetIkMode(false);
		//world->AddJoint(new ndJointPlane(matrix.m_posit, matrix.m_front, box, world->GetSentinelBody()));
		
		m_bodies.PushBack(box->GetAsBodyDynamic());
		m_bodies.PushBack(sph->GetAsBodyDynamic());
		m_contactSensor = sph->GetAsBodyDynamic();
		
		#ifdef D_USE_FORWARD_DYNAMICS
			world->AddJoint(m_effector);
		#endif
	}

	dInvertedPendulum(const ndLoadSaveBase::ndLoadDescriptor& desc)
		:ndModel(ndLoadSaveBase::ndLoadDescriptor(desc))
	{
		dAssert(0);
	}

	~dInvertedPendulum()
	{
		if (m_effector && !m_effector->IsInWorld())
		{
			delete m_effector;
		}
	}

	//void Save(const ndLoadSaveBase::ndSaveDescriptor& desc) const
	void Save(const ndLoadSaveBase::ndSaveDescriptor&) const
	{
		dAssert(0);
	}

	void Debug(ndConstraintDebugCallback& context) const
	{
		ndJointBilateralConstraint* const joint = m_effector;
		joint->DebugJoint(context);
		ndMatrix rootMatrix(dGetIdentityMatrix());

		ndVector com(CalculateCenterOfMass());
		rootMatrix.m_posit = com;
		//context.DrawFrame(rootMatrix);
		context.DrawPoint(com, ndVector(1.0f, 0.0f, 0.0f, 0.0f), 12.0f);

		ndVector p1(com + m_gravityDir.Scale (m_efectorLength * 2.0f));
		context.DrawLine(com, p1, ndVector(0.0f, 1.0f, 1.0f, 0.0f));
	}

	void PostUpdate(ndWorld* const world, ndFloat32 timestep)
	{
		ndModel::PostUpdate(world, timestep);
	}

	void PostTransformUpdate(ndWorld* const world, ndFloat32 timestep)
	{
		ndModel::PostTransformUpdate(world, timestep);
	}

	//void CalculateCenterOfMass(ndVector& com, ndVector& comVeloc) const
	//{
	//	ndFloat32 toltalMass = 0.0f;
	//	com = ndVector::m_zero;
	//	comVeloc = ndVector::m_zero;
	//	for (ndInt32 i = 0; i < m_bodies.GetCount(); ++i)
	//	{
	//		ndBodyDynamic* const body = m_bodies[i];
	//		ndFloat32 mass = body->GetMassMatrix().m_w;
	//		ndVector comMass(body->GetMatrix().TransformVector(body->GetCentreOfMass()));
	//		com += comMass.Scale(mass);
	//		comVeloc += body->GetVelocity().Scale(mass);
	//		toltalMass += mass;
	//	}
	//	com = com.Scale(1.0f / toltalMass) & ndVector::m_triplexMask;
	//	comVeloc = comVeloc.Scale(1.0f / toltalMass) & ndVector::m_triplexMask;;
	//}

	ndVector CalculateCenterOfMass() const
	{
		ndFloat32 toltalMass = 0.0f;
		ndVector com (ndVector::m_zero);
		//comVeloc = ndVector::m_zero;
		for (ndInt32 i = 0; i < m_bodies.GetCount(); ++i)
		{
			ndBodyDynamic* const body = m_bodies[i];
			ndFloat32 mass = body->GetMassMatrix().m_w;
			ndVector comMass(body->GetMatrix().TransformVector(body->GetCentreOfMass()));
			com += comMass.Scale(mass);
			//comVeloc += body->GetVelocity().Scale(mass);
			toltalMass += mass;
		}
		com = com.Scale(1.0f / toltalMass) & ndVector::m_triplexMask;
		//comVeloc = comVeloc.Scale(1.0f / toltalMass) & ndVector::m_triplexMask;;
		return com | ndVector::m_wOne;
	}

	void Update(ndWorld* const world, ndFloat32 timestep)
	{
		ndModel::Update(world, timestep);
		if (m_contactSensor)
		{
			#ifdef D_USE_FORWARD_DYNAMICS
			UpdateFK(world, timestep);
			#else
			UpdateIK(world, timestep);
			#endif
		}
	}

	//void UpdateIK(ndWorld* const world, ndFloat32 timestep)
	void UpdateIK(ndWorld* const, ndFloat32)
	{
		//ndSkeletonContainer* const skeleton = m_rootBody->GetSkeleton();
		//dAssert(skeleton);
		//
		//m_rootBody->SetSleepState(false);
		//
		//ndJointBilateralConstraint* effector = m_effector;
		//m_solver.SolverBegin(skeleton, &effector, 1, world, timestep);
		//m_solver.Solve();
		//m_solver.SolverEnd();
	}

	//void UpdateFK(ndWorld* const world, ndFloat32 timestep)
	void UpdateFK(ndWorld* const, ndFloat32)
	{
		// step 1: see if we have a support contacts
		ndBodyKinematic::ndContactMap::Iterator it(m_contactSensor->GetContactMap());
		bool hasSupport = false;
		for (it.Begin(); !hasSupport && it; it++)
		{
			const ndContact* const contact = it.GetNode()->GetInfo();
			hasSupport = hasSupport | contact->IsActive();
		}

		// step 2: we apply correction only if there is a support contact
		if (!hasSupport)
		{
//			return;
		}

		// step 3: have support contacts, find the projection of the com over the ground
		//const ndVector com(CalculateCenterOfMass());

		//// step 4: with the com find the projection to the ground
		//class rayCaster : public ndConvexCastNotify
		//{
		//	public:
		//	rayCaster(dInvertedPendulum* const owner)
		//		:m_owner(owner)
		//	{
		//	}
		//
		//	ndUnsigned32 OnRayPrecastAction(const ndBody* const body, const ndShapeInstance* const)
		//	{
		//		for (ndInt32 i = 0; i < m_owner->m_bodies.GetCount(); ++i)
		//		{
		//			if (body == m_owner->m_bodies[i])
		//			{
		//				return 0;
		//			}
		//		}
		//		return 1;
		//	}
		//
		//	dInvertedPendulum* m_owner;
		//};
		//
		//rayCaster caster(this);
		//const ndShapeInstance& shape = m_contactSensor->GetCollisionShape();
		//ndMatrix matrix(m_bodies[0]->GetMatrix());
		//matrix.m_posit = com | ndVector::m_wOne;
		//bool hit = world->ConvexCast(caster, shape, matrix, matrix.m_posit - ndVector(0.0f, 1.0f, 0.0f, 0.0f));
		//if (!hit)
		//{
		//	return;
		//}
		
		// step 5: her we have a com support point, the com, com velocity
		// need calculate translation distance from current contact to the com support contact
		//ndSkeletonContainer* const skeleton = m_bodies[0]->GetSkeleton();
		
static int xxx;
xxx++;

		ndJointBilateralConstraint* joint = m_effector;

//if (xxx >= 17)
if (xxx == 200)
{
//m_bodies[0]->SetVelocity(ndVector(0.0f, 0.0f, 1.0f, 0.0f));
return;
}
return;

		ndMatrix matrix0;
		ndMatrix matrix1;
		joint->CalculateGlobalMatrix(matrix0, matrix1);

		ndVector localGravity(matrix1.UnrotateVector(m_gravityDir));
		ndMatrix targetMatrix(m_effector->GetOffsetMatrix());

		// specify foot orientation. 
		targetMatrix = dGetIdentityMatrix() * matrix1.Inverse();

		targetMatrix.m_posit = localGravity.Scale(m_efectorLength) | ndVector::m_wOne;
if (xxx > 200)
{
	xxx *= 1;
}

		m_effector->SetOffsetMatrix(targetMatrix);

		//m_solver.SolverBegin(skeleton, &joint, 1, world, timestep);
		//m_solver.Solve();
		//m_solver.SolverEnd();
	}

	ndVector m_gravityDir;
	ndIkSolver m_solver;
	ndFixSizeArray<ndBodyDynamic*, 16> m_bodies;
	ndBodyDynamic* m_contactSensor;
	ndIk6DofEffector* m_effector;
	ndFloat32 m_efectorLength;
};
D_CLASS_REFLECTION_IMPLEMENT_LOADER(dInvertedPendulum);

void ndInvertedPendulum(ndDemoEntityManager* const scene)
{
	// build a floor
	BuildFloorBox(scene, dGetIdentityMatrix());
	//BuildFlatPlane(scene, true);

	ndVector origin1(0.0f, 0.0f, 0.0f, 0.0f);
	ndWorld* const world = scene->GetWorld();
	ndMatrix matrix(dYawMatrix(-0.0f * ndDegreeToRad));
	
	dAiBotTest_1* const aiBot_1 = new dAiBotTest_1(scene, matrix);
	scene->SetSelectedModel(aiBot_1);
	world->AddModel(aiBot_1);
	scene->Set2DDisplayRenderFunction(dAiBotTest_1::ControlPanel, nullptr, aiBot_1);

	//ndBodyDynamic* const root = aiBot_1->m_rootBody;
	//world->AddJoint(new ndJointFix6dof(root->GetMatrix(), root, world->GetSentinelBody()));


	//dInvertedPendulum* const robot0 = new dInvertedPendulum(scene, matrix);
	//matrix.m_posit.m_x += 2.0f;
	//matrix.m_posit.m_z -= 2.0f;
	//dInvertedPendulum* const robot1 = new dInvertedPendulum(scene, robotEntity, matrix);
	//world->AddModel(robot1);

	//ndVector posit(matrix.m_posit);
	//posit.m_x += 1.5f;
	//posit.m_z += 1.5f;
	//AddBox(scene, posit, 2.0f, 0.3f, 0.4f, 0.7f);
	//AddBox(scene, posit, 1.0f, 0.3f, 0.4f, 0.7f);

	//posit.m_x += 0.6f;
	//posit.m_z += 0.2f;
	//AddBox(scene, posit, 8.0f, 0.3f, 0.4f, 0.7f);
	//AddBox(scene, posit, 4.0f, 0.3f, 0.4f, 0.7f);

	//world->AddJoint(new ndJointFix6dof(robot0->GetRoot()->GetMatrix(), robot0->GetRoot(), world->GetSentinelBody()));

	matrix.m_posit.m_x -= 4.0f;
	matrix.m_posit.m_y += 1.5f;
	matrix.m_posit.m_z += 0.25f;
	ndQuaternion rotation(ndVector(0.0f, 1.0f, 0.0f, 0.0f), 0.0f * ndDegreeToRad);
	scene->SetCameraMatrix(rotation, matrix.m_posit);
}
