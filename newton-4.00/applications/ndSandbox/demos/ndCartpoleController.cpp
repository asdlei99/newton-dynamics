/* Copyright (c) <2003-2022> <Newton Game Dynamics>
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
#include "ndUIEntity.h"
#include "ndDemoMesh.h"
#include "ndDemoCamera.h"
#include "ndPhysicsUtils.h"
#include "ndPhysicsWorld.h"
#include "ndMakeStaticMap.h"
#include "ndDemoEntityManager.h"
#include "ndDemoInstanceEntity.h"

namespace ndController_0
{
	#define D_PUSH_ACCEL			ndFloat32 (1.0f)
	#define D_REWARD_MIN_ANGLE		(ndFloat32 (20.0f) * ndDegreeToRad)

	enum ndActionSpace
	{
		m_statePut,
		m_pushLeft,
		m_pushRight,
		m_actionsSize
	};

	enum ndStateSpace
	{
		m_poleOmega,
		m_poleAlpha,
		m_cartVelocity,
		m_cartAcceleration,
		m_stateSize
	};

	class ndCartpole: public ndModelArticulation
	{
		public:
		class ndDQNAgent : public ndBrainAgentDQN<m_stateSize, m_actionsSize>
		{
			public:
			ndDQNAgent(ndCartpole* const model, ndSharedPtr<ndBrain>& qValuePredictor)
				:ndBrainAgentDQN<m_stateSize, m_actionsSize>(qValuePredictor)
				,m_model(model)
				,m_framesAlive(0)
				,m_movingAverageCount(0)
			{
				for (ndInt32 i = 0; i < sizeof(m_movingAverage) / sizeof(m_movingAverage[0]); ++i)
				{
					m_movingAverage[i] = 0;
				}
			}

			ndReal GetReward() const
			{
				return m_model->GetReward();
			}

			void GetObservation(ndReal* const state) const
			{
				m_model->GetObservation(state);
			}

			bool IsTerminal() const
			{
				return m_model->IsTerminal();
			}

			void ResetModel() const
			{
				m_model->ResetModel();
			}

			void LearnStep()
			{
				ndBrainAgentDQN::LearnStep();

				if (m_currentTransition.m_terminalState)
				{
					m_movingAverage[m_movingAverageCount] = m_framesAlive;
					m_movingAverageCount = (m_movingAverageCount + 1) % ndInt32(sizeof(m_movingAverage) / sizeof(m_movingAverage[0]));

					m_framesAlive = 0;
					ndInt32 sum = 0;
					for (ndInt32 i = 0; i < sizeof(m_movingAverage) / sizeof(m_movingAverage[0]); ++i)
					{
						sum += m_movingAverage[i];
					}
					sum = sum / ndInt32(sizeof(m_movingAverage) / sizeof(m_movingAverage[0]));
					ndExpandTraceMessage("%d moving average alive frames:%d\n", m_frameCount - 1, sum);
				}

				static ndInt32 xxxxx = 0;
				if (m_framesAlive > xxxxx)
				{
					xxxxx = m_framesAlive;
					ndExpandTraceMessage("%d: episode:%d framesAlive:%d\n", m_frameCount - 1, m_eposideCount, m_framesAlive);
				}
				m_framesAlive++;
			}

			ndCartpole* m_model;
			ndInt32 m_framesAlive;
			ndInt32 m_movingAverageCount;
			ndInt32 m_movingAverage[32];
		};

		ndCartpole()
			:ndModelArticulation()
			,m_cartMatrix(ndGetIdentityMatrix())
			,m_poleMatrix(ndGetIdentityMatrix())
			,m_cart(nullptr)
			,m_pole(nullptr)
			,m_agent(nullptr)
		{
		}

		virtual bool IsTerminal() const
		{
			const ndMatrix& matrix = m_pole->GetMatrix();
			// agent dies if the angle is larger than D_REWARD_MIN_ANGLE * ndFloat32 (2.0f) degrees
			bool fail = ndAbs(matrix.m_front.m_x) > (D_REWARD_MIN_ANGLE * ndFloat32 (2.0f));
			return fail;
		}

		virtual ndReal GetReward() const
		{
			const ndMatrix& matrix = m_pole->GetMatrix();
			ndFloat32 angle = ndMin(ndAbs(matrix.m_front.m_x), D_REWARD_MIN_ANGLE);
			//ndFloat32 angle = ndAbs(matrix.m_front.m_x);
			ndFloat32 reward = ndFloat32(1.0f) - angle / D_REWARD_MIN_ANGLE;
			return ndReal(reward);
		}

		void GetObservation(ndReal* const state) const
		{
			ndVector alpha (m_pole->GetAlpha());
			ndVector omega (m_pole->GetOmega());

			ndVector accel(m_cart->GetAccel());
			ndVector veloc(m_cart->GetVelocity());

			state[m_poleAlpha] = ndReal(alpha.m_z);
			state[m_poleOmega] = ndReal(omega.m_z);
			state[m_cartVelocity] = ndReal(veloc.m_x);
			state[m_cartAcceleration] = ndReal(accel.m_x);
		}

		virtual void ResetModel() const
		{
			m_pole->SetOmega(ndVector::m_zero);
			m_pole->SetVelocity(ndVector::m_zero);

			m_cart->SetOmega(ndVector::m_zero);
			m_cart->SetVelocity(ndVector::m_zero);

			m_cart->SetMatrix(m_cartMatrix);
			m_pole->SetMatrix(m_poleMatrix);
		}

		void Update(ndWorld* const world, ndFloat32 timestep)
		{
			ndModelArticulation::Update(world, timestep);

			ndDQNAgent* const agent = (ndDQNAgent*)*m_agent;
			ndVector force(m_cart->GetForce());
			ndInt32 action = agent->GetTransition().m_action[0];
			if (action == m_pushLeft)
			{
				force.m_x = -m_cart->GetMassMatrix().m_w * D_PUSH_ACCEL;
			}
			else if (action == m_pushRight)
			{
				force.m_x = m_cart->GetMassMatrix().m_w * D_PUSH_ACCEL;
			}
			m_cart->SetForce(force);

			// add random impulse
			ndUnsigned32 perturbeProbability = 1024;
			ndUnsigned32 randFreq = ndRandInt() % perturbeProbability;
			if (randFreq > ndUnsigned32 (ndFloat32 (perturbeProbability) * ndFloat32(0.96f)))
			{
				ndVector impulse(ndVector::m_zero);
				impulse.m_x = m_cart->GetMassMatrix().m_w * ndGaussianRandom(0.0f, 0.05f);
				m_cart->ApplyImpulsePair(impulse, ndVector::m_zero, timestep);
			}
		}

		void PostUpdate(ndWorld* const world, ndFloat32 timestep)
		{
			ndModelArticulation::PostUpdate(world, timestep);
			m_agent->LearnStep();

			if (ndAbs(m_cart->GetMatrix().m_posit.m_x) > ndFloat32(40.0f))
			{
				ResetModel();
			}
		}

		ndMatrix m_cartMatrix;
		ndMatrix m_poleMatrix;
		ndBodyDynamic* m_cart;
		ndBodyDynamic* m_pole;
		ndSharedPtr<ndBrainAgent> m_agent;
	};

	void BuildModel(ndCartpole* const model, ndDemoEntityManager* const scene, const ndMatrix& location)
	{
		ndFloat32 xSize = 0.25f;
		ndFloat32 ySize = 0.125f;
		ndFloat32 zSize = 0.15f;
		ndFloat32 cartMass = 5.0f;
		ndFloat32 poleMass = 5.0f;
		ndFloat32 poleLength = 0.4f;
		ndFloat32 poleRadio = 0.05f;
		ndPhysicsWorld* const world = scene->GetWorld();
		
		// make cart
		ndSharedPtr<ndBody> cartBody(world->GetBody(AddBox(scene, location, cartMass, xSize, ySize, zSize, "smilli.tga")));
		ndModelArticulation::ndNode* const modelRoot = model->AddRootBody(cartBody);

		ndMatrix matrix(cartBody->GetMatrix());
		matrix.m_posit.m_y += ySize / 2.0f;

		// make pole leg
		ndSharedPtr<ndBody> poleBody(world->GetBody(AddCapsule(scene, ndGetIdentityMatrix(), poleMass, poleRadio, poleRadio, poleLength, "smilli.tga")));
		ndMatrix poleLocation(ndRollMatrix(90.0f * ndDegreeToRad) * matrix);
		poleLocation.m_posit.m_y += poleLength * 0.5f;
		poleBody->SetMatrix(poleLocation);

		// link cart and body with a hinge
		ndMatrix polePivot(ndYawMatrix(90.0f * ndDegreeToRad) * poleLocation);
		polePivot.m_posit.m_y -= poleLength * 0.5f;
		ndSharedPtr<ndJointBilateralConstraint> poleJoint(new ndJointHinge(polePivot, poleBody->GetAsBodyKinematic(), modelRoot->m_body->GetAsBodyKinematic()));

		// make the car move alone the z axis only (2d probpem)
		ndSharedPtr<ndJointBilateralConstraint> fixJoint(new ndJointSlider(cartBody->GetMatrix(), cartBody->GetAsBodyDynamic(), world->GetSentinelBody()));
		world->AddJoint(fixJoint);

		// add path to the model
		world->AddJoint(poleJoint);
		model->AddLimb(modelRoot, poleBody, poleJoint);

		// save some useful data
		model->m_cart = cartBody->GetAsBodyDynamic();
		model->m_pole = poleBody->GetAsBodyDynamic();
		model->m_cartMatrix = cartBody->GetMatrix();
		model->m_poleMatrix = poleBody->GetMatrix();

		// build newtral net controller
		ndSharedPtr<ndBrain> qValuePredictor(new ndBrain());
		ndBrainLayer* const inputLayer = new ndBrainLayer(m_stateSize, 128, m_relu);
		ndBrainLayer* const hiddenLayer0 = new ndBrainLayer(inputLayer->GetOuputSize(), 128, m_relu);
		ndBrainLayer* const hiddenLayer1 = new ndBrainLayer(hiddenLayer0->GetOuputSize(), 128, m_relu);
		ndBrainLayer* const ouputLayer = new ndBrainLayer(hiddenLayer1->GetOuputSize(), m_actionsSize, m_lineal);

		qValuePredictor->BeginAddLayer();
		qValuePredictor->AddLayer(inputLayer);
		qValuePredictor->AddLayer(hiddenLayer0);
		qValuePredictor->AddLayer(hiddenLayer1);
		qValuePredictor->AddLayer(ouputLayer);
		qValuePredictor->EndAddLayer(ndReal(0.25f));

		// add a reinforcement learning controler 
		model->m_agent = ndSharedPtr<ndBrainAgent>(new ndCartpole::ndDQNAgent(model, qValuePredictor));
	}

	ndModelArticulation* CreateModel(ndDemoEntityManager* const scene, const ndMatrix& location)
	{
		ndCartpole* const model = new ndCartpole();
		BuildModel(model, scene, location);
		return model;
	}
}

using namespace ndController_0;
void ndCartpoleController(ndDemoEntityManager* const scene)
{
	// build a floor
	//BuildFloorBox(scene, ndGetIdentityMatrix());
	BuildFlatPlane(scene, true);

	ndSetRandSeed(42);
	scene->SetAcceleratedUpdate();

	ndWorld* const world = scene->GetWorld();
	ndMatrix matrix(ndYawMatrix(-0.0f * ndDegreeToRad));
	ndSharedPtr<ndModel> model(CreateModel(scene, matrix));
	world->AddModel(model);
	
	matrix.m_posit.m_x -= 0.0f;
	matrix.m_posit.m_y += 0.5f;
	matrix.m_posit.m_z += 2.0f;
	ndQuaternion rotation(ndVector(0.0f, 1.0f, 0.0f, 0.0f), 90.0f * ndDegreeToRad);
	scene->SetCameraMatrix(rotation, matrix.m_posit);
}
