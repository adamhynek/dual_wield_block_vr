#include "MathUtils.h"

namespace PapyrusVR
{

	Quaternion MathUtils::QuaternionInverse(const Quaternion& quat)
	{
		Quaternion inverse;
		float normSquared = quat.w*quat.w + quat.x*quat.x + quat.y*quat.y + quat.z*quat.z;
		if (!normSquared)
			normSquared = 1;
		inverse.w = quat.w / normSquared;
		inverse.x = -quat.x / normSquared;
		inverse.y = -quat.y / normSquared;
		inverse.z = -quat.z / normSquared;
		return inverse;
	}

	Quaternion MathUtils::QuaternionMultiply(const Quaternion& quatA, const Quaternion& quatB)
	{
		Quaternion multiple;
		multiple.w = quatA.w * quatB.w - quatA.x * quatB.x - quatA.y * quatB.y - quatA.z * quatB.z;
		multiple.x = quatA.w * quatB.x + quatA.x * quatB.w + quatA.y * quatB.z - quatA.z * quatB.y;
		multiple.y = quatA.w * quatB.y - quatA.x * quatB.z + quatA.y * quatB.w + quatA.z * quatB.x;
		multiple.z = quatA.w * quatB.z + quatA.x * quatB.y - quatA.y * quatB.x + quatA.z * quatB.w;
		return multiple;
	}

	Vector3 MathUtils::QuaternionToMatrixZ(const Quaternion& quat)
	{
		Vector3 columnZ;
		columnZ.x = 2 * quat.x * quat.z + 2 * quat.y * quat.w;
		columnZ.y = 2 * quat.y * quat.z - 2 * quat.x * quat.w;
		columnZ.z = 1 - 2 * quat.x*quat.x - 2 * quat.y*quat.y;
		return columnZ;
	}

	Vector3 MathUtils::VectorDivide(const Vector3& vec, float div)
	{
		Vector3 divided(0, 0, 0);
		if (div == 0) return divided;
		divided.x = vec.x / div;
		divided.y = vec.y / div;
		divided.z = vec.z / div;
		return divided;
	}

	float MathUtils::VectorDotProduct(const Vector3& vecA, const Vector3& vecB)
	{
		float dot = 0;
		dot += vecA.x * vecB.x;
		dot += vecA.y * vecB.y;
		dot += vecA.z * vecB.z;
		return dot;
	}

	Vector3 MathUtils::VectorNormalize(const Vector3& vec)
	{
		return VectorDivide(vec, VectorLength(vec));
	}

	float MathUtils::VectorLength(const Vector3& vec)
	{
		return sqrt(vec.x * vec.x + vec.y * vec.y + vec.z * vec.z);
	}

}
