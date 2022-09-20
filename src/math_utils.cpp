#include "math_utils.h"


NiPoint3 VectorNormalized(const NiPoint3 &vec)
{
	float length = VectorLength(vec);
	return length ? vec / length : NiPoint3();
}

NiPoint3 CrossProduct(const NiPoint3 &vec1, const NiPoint3 &vec2)
{
	NiPoint3 result;
	result.x = vec1.y * vec2.z - vec1.z * vec2.y;
	result.y = vec1.z * vec2.x - vec1.x * vec2.z;
	result.z = vec1.x * vec2.y - vec1.y * vec2.x;
	return result;
}


NiQuaternion QuaternionIdentity()
{
	return { 1, 0, 0, 0 };
}

NiQuaternion QuaternionNormalized(const NiQuaternion &q)
{
	float length = QuaternionLength(q);
	if (length) {
		return QuaternionMultiply(q, 1.0f / length);
	}
	return QuaternionIdentity();
}

NiQuaternion QuaternionMultiply(const NiQuaternion &qa, const NiQuaternion &qb)
{
	NiQuaternion multiple;
	multiple.m_fW = qa.m_fW * qb.m_fW - qa.m_fX * qb.m_fX - qa.m_fY * qb.m_fY - qa.m_fZ * qb.m_fZ;
	multiple.m_fX = qa.m_fW * qb.m_fX + qa.m_fX * qb.m_fW + qa.m_fY * qb.m_fZ - qa.m_fZ * qb.m_fY;
	multiple.m_fY = qa.m_fW * qb.m_fY - qa.m_fX * qb.m_fZ + qa.m_fY * qb.m_fW + qa.m_fZ * qb.m_fX;
	multiple.m_fZ = qa.m_fW * qb.m_fZ + qa.m_fX * qb.m_fY - qa.m_fY * qb.m_fX + qa.m_fZ * qb.m_fW;
	return multiple;
}

NiQuaternion QuaternionMultiply(const NiQuaternion &q, float multiplier)
{
	NiQuaternion multiple;
	multiple.m_fW = q.m_fW * multiplier;
	multiple.m_fX = q.m_fX * multiplier;
	multiple.m_fY = q.m_fY * multiplier;
	multiple.m_fZ = q.m_fZ * multiplier;
	return multiple;
}

NiQuaternion QuaternionInverse(const NiQuaternion &q)
{
	NiQuaternion inverse;
	float normSquared = q.m_fW * q.m_fW + q.m_fX * q.m_fX + q.m_fY * q.m_fY + q.m_fZ * q.m_fZ;
	if (!normSquared)
		normSquared = 1;
	inverse.m_fW = q.m_fW / normSquared;
	inverse.m_fX = -q.m_fX / normSquared;
	inverse.m_fY = -q.m_fY / normSquared;
	inverse.m_fZ = -q.m_fZ / normSquared;
	return inverse;
}
