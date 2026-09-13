# Lid-detach projection — implementation spec (DirectX 11 / HLSL)

## What this is NOT

Do not build a "projector camera" that has a view/projection matrix and looks
at the lid. Do not build a separate render pass with its own camera. Do not
warp the display texture so it always fills the lid corner-to-corner (that is
a bijective quad homography and it will never produce black borders — wrong
for this effect).

## What this actually is

A single full-screen pixel shader. The screen IS the lid. For every physical
pixel of the lid, in the lid's own straight-on pixel space, we:

1. Find that pixel's 3D position on the lid plane.
2. Cast a ray from a fixed head point through that 3D position.
3. Intersect the ray with the (separate, fixed) virtual display plane.
4. Convert the hit point to the display's own UV space.
5. If that UV is inside [0,1]×[0,1], sample the desktop texture there.
   If it's outside, output black. Do NOT clamp — the black is the point.

There is no camera object anywhere in this shader. "Capture" = the raster
itself, because the lid physically is the display panel.

## World setup (CPU side, per frame)

Work in a coordinate system where the display's hinge line is the X axis at
Y=0, Z=0, and the display plane is the Z=0 plane.

```
halfW        = displayWidthWorld / 2
dispHeight   = displayHeightWorld

// Display corners (world space) — display never moves
float3 dispBL = float3(-halfW, 0,            0);
float3 dispBR = float3( halfW, 0,            0);
float3 dispTL = float3(-halfW, dispHeight,   0);
float3 dispTR = float3( halfW, dispHeight,   0);

float3 dispRightAxis = float3(1, 0, 0);      // normalized, world space
float3 dispUpAxis    = float3(0, 1, 0);      // normalized, world space
float3 dispNormal    = float3(0, 0, 1);      // normalized, world space
```

`theta` = hinge tilt angle, 0 = lid coplanar with display (fully open, glass
flush with screen), increasing = lid swinging forward/closed. Derive this
from your two accelerometer readings — whatever your raw angle convention is,
convert it to this 0-at-open convention before using it here. Get the sign
right by testing: closing the lid must increase theta.

```
// Lid corners (world space) — recomputed every frame from theta
float3 lidBL = dispBL;   // SAME point as display corner, always
float3 lidBR = dispBR;   // SAME point as display corner, always

float lidTopY = dispHeight * cos(theta);
float lidTopZ = dispHeight * sin(theta);
float3 lidTL = float3(-halfW, lidTopY, lidTopZ);
float3 lidTR = float3( halfW, lidTopY, lidTopZ);
```

The bottom two lid corners being *literally identical* to the display's
bottom corners is what guarantees the bottom edge is always full-width and
always in-bounds — don't recompute them from theta, hardcode the equality.

```
// Head point — push this further back (larger Z) to make the intersection
// point travel further down the display as theta increases.
float3 headPos = float3(0, eyeHeightWorld, headDistanceWorld);
```

Upload per frame: `headPos`, `lidBL, lidBR, lidTL, lidTR`, `dispBL,
dispRightAxis, dispUpAxis, dispNormal, halfW*2 (dispWidth), dispHeight`. Pack
into one constant buffer.

## Vertex shader

Render a standard full-screen quad (2 triangles, 4 unique verts) directly in
clip space — this is a post-process pass, not a 3D-projected draw. Each
vertex carries its own `(s,t)` in `{0,1}` AND the corresponding lid world
corner. Rasterizer interpolation across the quad then gives every pixel the
correct bilinear-interpolated lid-plane world position for free, because the
lid is planar and the quad is a plain screen-space quad (no perspective
divide involved here — that divide happens explicitly in the pixel shader,
not via hardware interpolation).

```hlsl
struct VSOut {
    float4 pos      : SV_POSITION;
    float2 st       : TEXCOORD0;
    float3 lidWorld : TEXCOORD1; // per-vertex lid corner, bilinearly interpolated
};

VSOut VSMain(uint vid : SV_VertexID)
{
    // 4 verts: (s,t) = (0,0) (1,0) (0,1) (1,1) as a triangle strip
    float2 st = float2(vid & 1, vid >> 1);
    VSOut o;
    o.pos = float4(st * float2(2,-2) + float2(-1,1), 0, 1); // NDC full-screen
    o.st  = st;

    // Bilinear pick of the matching lid world-space corner
    float3 top = lerp(lidTL, lidTR, st.x);
    float3 bot = lerp(lidBL, lidBR, st.x);
    o.lidWorld = lerp(bot, top, st.y);
    return o;
}
```

(`lidTL/lidTR/lidBL/lidBR` come from the constant buffer described above.)

## Pixel shader

```hlsl
Texture2D    displayTex : register(t0);
SamplerState borderSamp : register(s0); // see sampler setup below

cbuffer SceneCB : register(b0)
{
    float3 headPos;        float _pad0;
    float3 dispOrigin;     float _pad1; // = dispBL
    float3 dispRightAxis;  float _pad2;
    float3 dispUpAxis;     float _pad3;
    float3 dispNormal;     float _pad4;
    float  dispWidth;
    float  dispHeight;
    float  blurNear;       // tHit distance where blur = 0
    float  blurFar;        // tHit distance where blur = max
    float  maxBlurPx;
};

float4 PSMain(float4 pos : SV_POSITION, float2 st : TEXCOORD0, float3 lidWorld : TEXCOORD1) : SV_TARGET
{
    float3 D = normalize(lidWorld - headPos);

    float denom = dot(dispNormal, D);
    if (abs(denom) < 1e-5) return float4(0,0,0,1);

    float tHit = dot(dispNormal, dispOrigin - headPos) / denom;
    if (tHit < 0) return float4(0,0,0,1); // hit is behind the head, invalid

    float3 R = headPos + tHit * D;
    float3 local = R - dispOrigin;

    float u = dot(local, dispRightAxis) / dispWidth;
    float v = dot(local, dispUpAxis)    / dispHeight;

    if (u < 0 || u > 1 || v < 0 || v > 1)
        return float4(0,0,0,1); // outside display bounds -> black border

    // --- blur, reusing tHit as the perspective-depth interpolant ---
    float blurT = saturate((tHit - blurNear) / (blurFar - blurNear));
    float radiusPx = blurT * maxBlurPx;
    float2 texel = radiusPx / float2(1920, 1080); // swap for your actual res, or pass as CB field

    float4 color = displayTex.Sample(borderSamp, float2(u, v));
    if (radiusPx > 0.5)
    {
        float4 sum = color;
        int taps = 4;
        [unroll] for (int i = 1; i <= taps; i++)
        {
            float w = i / (float)taps;
            sum += displayTex.Sample(borderSamp, float2(u, v) + float2( texel.x, 0) * w);
            sum += displayTex.Sample(borderSamp, float2(u, v) + float2(-texel.x, 0) * w);
            sum += displayTex.Sample(borderSamp, float2(u, v) + float2(0,  texel.y) * w);
            sum += displayTex.Sample(borderSamp, float2(u, v) + float2(0, -texel.y) * w);
        }
        color = sum / (1 + 4*taps);
    }

    return color;
}
```

## Sampler setup (C++ / D3D11)

The black border is load-bearing — this must be `BORDER` addressing with a
black border color, not `CLAMP`:

```cpp
D3D11_SAMPLER_DESC sd = {};
sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
sd.AddressU       = D3D11_TEXTURE_ADDRESS_BORDER;
sd.AddressV       = D3D11_TEXTURE_ADDRESS_BORDER;
sd.AddressW       = D3D11_TEXTURE_ADDRESS_BORDER;
sd.BorderColor[0] = 0.0f;
sd.BorderColor[1] = 0.0f;
sd.BorderColor[2] = 0.0f;
sd.BorderColor[3] = 1.0f;
sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
sd.MinLOD         = 0;
sd.MaxLOD         = D3D11_FLOAT32_MAX;
device->CreateSamplerState(&sd, &borderSamplerState);
```

## Checklist for the coding model

- [ ] No camera, no view matrix, no projection matrix anywhere in this pass.
- [ ] Lid bottom corners are hardcoded equal to display bottom corners, not
      derived from theta.
- [ ] Head point is a plain `float3` in the same world space as the display
      and lid, positioned further in +Z than the lid ever swings.
- [ ] Sampler uses `BORDER` addressing with black, not `CLAMP` or `WRAP`.
- [ ] Out-of-bounds UV returns black explicitly — don't rely on the sampler
      alone if you also do the manual `if` check (belt and suspenders is
      fine, but the manual check must come first for the blur logic to skip
      correctly too).
- [ ] `tHit` is reused directly as the blur interpolant — don't compute a
      second, separate "distance" value.
- [ ] Test the direction of `theta` against a real accelerometer reading:
      closing the physical lid must increase `theta` toward the "closed"
      end, or every sign in `lidTopY`/`lidTopZ` will be backwards.
