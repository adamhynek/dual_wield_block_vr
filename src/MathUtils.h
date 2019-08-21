#pragma once

#include "api/utils/OpenVRUtils.h"

namespace PapyrusVR
{

	class MathUtils
	{
	public:
		static Quaternion QuaternionInverse(const Quaternion& quat);
		static Quaternion QuaternionMultiply(const Quaternion& quatA, const Quaternion& quatB);
		static Vector3 QuaternionToMatrixZ(const Quaternion& quat);
		static Vector3 VectorDivide(const Vector3& vec, float div);
		static float VectorDotProduct(const Vector3& vecA, const Vector3& vecB);
		static Vector3 VectorNormalize(const Vector3& vec);
		static float VectorLength(const Vector3& vec);
	};

}
