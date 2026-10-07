#include "Particle.hlsli"

#define MAX_PARTICLES 65536

// u0 — из него забираем живые частицы, u1 — в него пишем обновлённые и новые.
// Оба буфера имеют скрытый счётчик; какой из них consume, а какой append, меняется каждый кадр
// (C++ просто отдаёт таблицу дескрипторов с нужным порядком).
ConsumeStructuredBuffer<Particle> gConsume : register(u0);
AppendStructuredBuffer<Particle>  gAppend  : register(u1);

cbuffer SimCB : register(b0)
{
    float  gDt;           // шаг времени, сек
    uint   gEmitCount;    // сколько частиц породить в этом кадре
    uint   gSeed;         // меняется каждый кадр (для случайных чисел)
    float  gFloorY;       // высота пола, от которого отскакиваем
    float3 gEmitterPos;
    float  gEmitSpeed;    // начальная скорость струи
    float3 gGravity;
    float  gRestitution;  // сколько скорости остаётся после отскока
};

// Сколько живых частиц лежит в consume-буфере (C++ копирует сюда его счётчик перед dispatch).
cbuffer CountCB : register(b1)
{
    uint gAliveCount;
};

// --- Простой хэш (PCG) для случайных чисел на GPU ---
uint Hash(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float Random01(inout uint state)
{
    state = Hash(state);
    return (state & 0x00FFFFFFu) / 16777216.0f;   // [0, 1)
}

// ---------------- Симуляция: Consume -> обновить -> Append ----------------
[numthreads(64, 1, 1)]
void SimulateCS(uint3 id : SV_DispatchThreadID)
{
    // Забирать можно ровно столько частиц, сколько их есть, иначе счётчик уйдёт в минус.
    if (id.x >= gAliveCount)
        return;

    Particle p = gConsume.Consume();

    p.Lifetime -= gDt;
    if (p.Lifetime <= 0.0f)
        return;                       // частица умерла: не пишем в append-буфер

    p.Velocity += gGravity * gDt;
    p.Position += p.Velocity * gDt;

    // Отскок от пола
    if (p.Position.y < gFloorY)
    {
        p.Position.y = gFloorY;
        p.Velocity.y = -p.Velocity.y * gRestitution;
        p.Velocity.xz *= 0.7f;        // трение
    }

    gAppend.Append(p);
}

// ---------------- Эмиттер: рождает частицы в append-буфер ----------------
[numthreads(64, 1, 1)]
void EmitCS(uint3 id : SV_DispatchThreadID)
{
    // Не переполняем буфер: свободное место считаем по числу живых на начало кадра (с запасом).
    uint room = MAX_PARTICLES - gAliveCount;
    if (id.x >= min(gEmitCount, room))
        return;

    uint rng = Hash(id.x + gSeed * 9781u);

    // Направление внутри конуса вокруг оси Y
    float azimuth = Random01(rng) * 6.2831853f;
    float tilt    = Random01(rng) * 0.45f;            // ~26 градусов
    float3 dir = float3(sin(tilt) * cos(azimuth), cos(tilt), sin(tilt) * sin(azimuth));

    Particle p;
    p.Position = gEmitterPos + float3(Random01(rng) - 0.5f, 0.0f, Random01(rng) - 0.5f) * 0.1f;
    p.Velocity = dir * gEmitSpeed * (0.6f + 0.4f * Random01(rng));
    p.Size     = 0.03f + 0.04f * Random01(rng);
    p.Lifetime = 2.5f + 2.0f * Random01(rng);
    gAppend.Append(p);
}
