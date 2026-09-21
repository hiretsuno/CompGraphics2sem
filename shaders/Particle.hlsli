// Общая структура частицы для Compute / Vertex шейдеров.
// ДОЛЖНА совпадать с struct Particle в include/ParticleSystem.h (32 байта).
struct Particle
{
    float3 Position;
    float  Size;
    float3 Velocity;
    float  Lifetime;   // оставшееся время жизни, сек; <= 0 = мертва
};
