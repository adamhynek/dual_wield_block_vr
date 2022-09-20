#pragma once

#include <algorithm>
#include "skse64/NiTypes.h"


inline float VectorLengthSquared(const NiPoint3 &vec) { return vec.x * vec.x + vec.y * vec.y + vec.z * vec.z; }
inline float VectorLength(const NiPoint3 &vec) { return sqrtf(VectorLengthSquared(vec)); }
inline float DotProduct(const NiPoint3 &vec1, const NiPoint3 &vec2) { return vec1.x * vec2.x + vec1.y * vec2.y + vec1.z * vec2.z; }
inline float DotProduct(const NiQuaternion &q1, const NiQuaternion &q2) { return q1.m_fW * q2.m_fW + q1.m_fX * q2.m_fX + q1.m_fY * q2.m_fY + q1.m_fZ * q2.m_fZ; }
inline float DotProductSafe(const NiPoint3 &vec1, const NiPoint3 &vec2) { return std::clamp(DotProduct(vec1, vec2), -1.f, 1.f); }
inline float DotProductSafe(const NiQuaternion &q1, const NiQuaternion &q2) { return std::clamp(DotProduct(q1, q2), -1.f, 1.f); }
inline float QuaternionLength(const NiQuaternion &q) { return sqrtf(DotProduct(q, q)); }
NiPoint3 VectorNormalized(const NiPoint3 &vec);
NiPoint3 CrossProduct(const NiPoint3 &vec1, const NiPoint3 &vec2);

NiQuaternion QuaternionIdentity();
NiQuaternion QuaternionNormalized(const NiQuaternion &q);
NiQuaternion QuaternionMultiply(const NiQuaternion &qa, const NiQuaternion &qb);
NiQuaternion QuaternionMultiply(const NiQuaternion &q, float multiplier);
NiQuaternion QuaternionInverse(const NiQuaternion &q);

inline NiTransform InverseTransform(const NiTransform &t) { NiTransform inverse; t.Invert(inverse); return inverse; }
inline NiPoint3 RightVector(const NiMatrix33 &r) { return { r.data[0][0], r.data[1][0], r.data[2][0] }; }
inline NiPoint3 ForwardVector(const NiMatrix33 &r) { return { r.data[0][1], r.data[1][1], r.data[2][1] }; }
inline NiPoint3 UpVector(const NiMatrix33 &r) { return { r.data[0][2], r.data[1][2], r.data[2][2] }; }
