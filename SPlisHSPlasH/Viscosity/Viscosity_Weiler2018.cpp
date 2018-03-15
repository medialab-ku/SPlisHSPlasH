#include "Viscosity_Weiler2018.h"
#include "SPlisHSPlasH/TimeManager.h"
#include "Utilities/Timing.h"
#include "Utilities/Counting.h"

using namespace SPH;
using namespace GenParam;

int Viscosity_Weiler2018::ITERATIONS = -1;
int Viscosity_Weiler2018::MAX_ITERATIONS = -1;
int Viscosity_Weiler2018::MAX_ERROR = -1;
int Viscosity_Weiler2018::VISCOSITY_COEFFICIENT_BOUNDARY = -1;

Viscosity_Weiler2018::Viscosity_Weiler2018(FluidModel *model) :
	ViscosityBase(model), m_vDiff()
{
	m_maxIter = 100;
	m_maxError = 0.01;
	m_iterations = 0;

	m_vDiff.resize(model->numParticles(), Vector3r::Zero());
}

Viscosity_Weiler2018::~Viscosity_Weiler2018(void)
{
	m_vDiff.clear();
}

void Viscosity_Weiler2018::initParameters()
{
	ViscosityBase::initParameters();

	VISCOSITY_COEFFICIENT_BOUNDARY = createNumericParameter("viscosityBoundary", "Viscosity coefficient (Boundary)", &m_boundaryViscosity);
	setGroup(VISCOSITY_COEFFICIENT_BOUNDARY, "Viscosity");
	setDescription(VISCOSITY_COEFFICIENT_BOUNDARY, "Coefficient for the viscosity force computation at the boundary.");
	RealParameter* rparam = static_cast<RealParameter*>(getParameter(VISCOSITY_COEFFICIENT_BOUNDARY));
	rparam->setMinValue(0.0);

	ITERATIONS = createNumericParameter("viscoIterations", "Iterations", &m_iterations);
	setGroup(ITERATIONS, "Viscosity");
	setDescription(ITERATIONS, "Iterations required by the viscosity solver.");
	getParameter(ITERATIONS)->setReadOnly(true);

	MAX_ITERATIONS = createNumericParameter("viscoMaxIter", "Max. iterations (visco)", &m_maxIter);
	setGroup(MAX_ITERATIONS, "Viscosity");
	setDescription(MAX_ITERATIONS, "Coefficient for the viscosity force computation");
	static_cast<NumericParameter<unsigned int>*>(getParameter(MAX_ITERATIONS))->setMinValue(1);

	MAX_ERROR = createNumericParameter("viscoMaxError", "Max. visco error", &m_maxError);
	setGroup(MAX_ERROR, "Viscosity");
	setDescription(MAX_ERROR, "Coefficient for the viscosity force computation");
	rparam = static_cast<RealParameter*>(getParameter(MAX_ERROR));
	rparam->setMinValue(1e-6);
}

void Viscosity_Weiler2018::matrixVecProd(const Real* vec, Real *result, void *userData)
{
	Viscosity_Weiler2018 *visco = (Viscosity_Weiler2018*)userData;
	FluidModel *model = visco->getModel();
	const unsigned int numParticles = model->numActiveParticles();

	const Real h = model->getSupportRadius();
	const Real h2 = h*h;
	const Real dt = TimeManager::getCurrent()->getTimeStepSize();
	const Real mu = visco->m_viscosity;
	const Real mub = visco->m_boundaryViscosity;

	#pragma omp parallel default(shared)
	{
		#pragma omp for schedule(static) 
		for (int i = 0; i < (int)numParticles; i++)
		{
			const Vector3r &xi = model->getPosition(0, i);
			Vector3r ai;
			ai.setZero();
			const Real density_i = model->getDensity(i);
			const Vector3r &vi = Eigen::Map<const Vector3r>(&vec[3 * i]);

			for (unsigned int j = 0; j < model->numberOfNeighbors(0, i); j++)
			{
				const unsigned int neighborIndex = model->getNeighbor(0, i, j);
				const Vector3r &xj = model->getPosition(0, neighborIndex);
				const Real density_j = model->getDensity(neighborIndex);
				const Vector3r gradW = model->gradW(xi - xj);

				const Vector3r &vj = Eigen::Map<const Vector3r>(&vec[3 * neighborIndex]);
				const Vector3r xixj = xi - xj;

				ai += 10.0 * mu * (model->getMass(neighborIndex) / density_j) * (vi - vj).dot(xixj) / (xixj.squaredNorm() + 0.01*h2) * gradW;
			}
			for (unsigned int pid = 1; pid < model->numberOfPointSets(); pid++)
			{
				for (unsigned int j = 0; j < model->numberOfNeighbors(pid, i); j++)
				{
					const unsigned int neighborIndex = model->getNeighbor(pid, i, j);
					const Vector3r &xj = model->getPosition(pid, neighborIndex);
					const Vector3r &vj = model->getVelocity(pid, neighborIndex);
					const Vector3r gradW = model->gradW(xi - xj);

					const Vector3r xixj = xi - xj;
					ai += 10.0 * mub * (model->getBoundaryPsi(pid, neighborIndex) / density_i) * (vi - vj).dot(xixj) / (xixj.squaredNorm() + 0.01*h2) * gradW;
				}
			}

			result[3 * i] = vec[3 * i] - dt / density_i*ai[0];
			result[3 * i + 1] = vec[3 * i + 1] - dt / density_i*ai[1];
			result[3 * i + 2] = vec[3 * i + 2] - dt / density_i*ai[2];
		}
	}
}

#ifdef USE_BLOCKDIAGONAL_PRECONDITIONER
void Viscosity_Weiler2018::diagonalMatrixElement(const unsigned int row, Matrix3r &result, void *userData)
{
	// Diagonal element
	Viscosity_Weiler2018 *visco = (Viscosity_Weiler2018*)userData;
	FluidModel *model = visco->getModel();

	const Real h = model->getSupportRadius();
	const Real h2 = h*h;
	const Real dt = TimeManager::getCurrent()->getTimeStepSize();
	const Real mu = visco->m_viscosity;
	const Real mub = visco->m_boundaryViscosity;

	const Real density_i = model->getDensity(row);

	result.setZero();
	
	const Vector3r &xi = model->getPosition(0, row);
	for (unsigned int j = 0; j < model->numberOfNeighbors(0, row); j++)
	{
		const unsigned int neighborIndex = model->getNeighbor(0, row, j);
		const Vector3r &xj = model->getPosition(0, neighborIndex);
		const Real density_j = model->getDensity(neighborIndex);
		const Vector3r gradW = model->gradW(xi - xj);
		const Vector3r xixj = xi - xj;
		result += 10.0 * mu * (model->getMass(neighborIndex) / density_j) / (xixj.squaredNorm() + 0.01*h2) * (gradW * xixj.transpose());
	}
	for (unsigned int pid = 1; pid < model->numberOfPointSets(); pid++)
	{
		for (unsigned int j = 0; j < model->numberOfNeighbors(pid, row); j++)
		{
			const unsigned int neighborIndex = model->getNeighbor(pid, row, j);
			const Vector3r &xj = model->getPosition(pid, neighborIndex);
			const Vector3r &vj = model->getVelocity(pid, neighborIndex);
			const Vector3r gradW = model->gradW(xi - xj);

			const Vector3r xixj = xi - xj;
			result += 10.0 * mub * (model->getBoundaryPsi(pid, neighborIndex) / density_i) / (xixj.squaredNorm() + 0.01*h2) * (gradW * xixj.transpose());
		}
	}
	result = Matrix3r::Identity() - (dt / density_i) * result;
}

#else

void Viscosity_Weiler2018::diagonalMatrixElement(const unsigned int row, Vector3r &result, void *userData)
{
	// Diagonal element
	Viscosity_Weiler2018 *visco = (Viscosity_Weiler2018*)userData;
	FluidModel *model = visco->getModel();

	const Real h = model->getSupportRadius();
	const Real h2 = h*h;
	const Real dt = TimeManager::getCurrent()->getTimeStepSize();
	const Real mu = visco->m_viscosity;
	const Real mub = visco->m_boundaryViscosity;

	const Real density_i = model->getDensity(row);

	result.setZero();

	const Vector3r &xi = model->getPosition(0, row);
	for (unsigned int j = 0; j < model->numberOfNeighbors(0, row); j++)
	{
		const unsigned int neighborIndex = model->getNeighbor(0, row, j);
		const Vector3r &xj = model->getPosition(0, neighborIndex);
		const Real density_j = model->getDensity(neighborIndex);
		const Vector3r gradW = model->gradW(xi - xj);
		const Vector3r xixj = xi - xj;
		Matrix3r r = 10.0 * mu * (model->getMass(neighborIndex) / density_j) / (xixj.squaredNorm() + 0.01*h2) * (gradW * xixj.transpose());
		result += r.diagonal();
	}
	for (unsigned int pid = 1; pid < model->numberOfPointSets(); pid++)
	{
		for (unsigned int j = 0; j < model->numberOfNeighbors(pid, row); j++)
		{
			const unsigned int neighborIndex = model->getNeighbor(pid, row, j);
			const Vector3r &xj = model->getPosition(pid, neighborIndex);
			const Vector3r &vj = model->getVelocity(pid, neighborIndex);
			const Vector3r gradW = model->gradW(xi - xj);

			const Vector3r xixj = xi - xj;
			Matrix3r r = 10.0 * mub * (model->getBoundaryPsi(pid, neighborIndex) / density_i) / (xixj.squaredNorm() + 0.01*h2) * (gradW * xixj.transpose());
			result += r.diagonal();
		}
	}
	result = Vector3r::Ones() - (dt / density_i) * result;
}

#endif


void Viscosity_Weiler2018::step()
{
	const int numParticles = (int) m_model->numActiveParticles();
	// prevent solver from running with a zero-length vector
	if (numParticles == 0)
		return;
	const Real density0 = m_model->getValue<Real>(FluidModel::DENSITY0);
	const Real h = TimeManager::getCurrent()->getTimeStepSize();

	//////////////////////////////////////////////////////////////////////////
	// Init linear system solver and preconditioner
	//////////////////////////////////////////////////////////////////////////
	MatrixReplacement A(3*m_model->numActiveParticles(), matrixVecProd, (void*) this);
	m_solver.preconditioner().init(m_model->numActiveParticles(), diagonalMatrixElement, (void*)this);

	m_solver.setTolerance(m_maxError);
	m_solver.setMaxIterations(m_maxIter);
	m_solver.compute(A);

	Eigen::VectorXd b(3*numParticles);
	Eigen::VectorXd x(3*numParticles);
	Eigen::VectorXd g(3*numParticles);

	//////////////////////////////////////////////////////////////////////////
	// Compute RHS
	//////////////////////////////////////////////////////////////////////////
	#pragma omp parallel default(shared)
	{
		#pragma omp for schedule(static) nowait 
		for (int i = 0; i < (int)numParticles; i++)
		{
			const Vector3r &vi = m_model->getVelocity(0, i);
			b[3*i] = vi[0];
			b[3*i+1] = vi[1];
			b[3*i+2] = vi[2];

			// Warmstart
			g[3 * i] = vi[0]+m_vDiff[i][0];
			g[3 * i + 1] = vi[1]+m_vDiff[i][1];
			g[3 * i + 2] = vi[2]+m_vDiff[i][2];
		}
	}

	//////////////////////////////////////////////////////////////////////////
	// Solve linear system 
	//////////////////////////////////////////////////////////////////////////
	START_TIMING("CG solve");
	x = m_solver.solveWithGuess(b, g);
	m_iterations = (int)m_solver.iterations();
	STOP_TIMING_AVG;
	INCREASE_COUNTER("Visco iterations", m_iterations);

	#pragma omp parallel default(shared)
	{
		#pragma omp for schedule(static)  
		for (int i = 0; i < (int)numParticles; i++)
		{
			Vector3r &ai = m_model->getAcceleration(i);
			const Vector3r newV(x[3 * i], x[3 * i + 1], x[3 * i + 2]);
			ai += (1.0 / h) * (newV - m_model->getVelocity(0, i));
			m_vDiff[i] = (newV - m_model->getVelocity(0, i));
		}
	}
}


void Viscosity_Weiler2018::reset()
{
}

void Viscosity_Weiler2018::performNeighborhoodSearchSort()
{
}