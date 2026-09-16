# Source Verification: Unity Shader Graph "Metal Reflectance" F0 Values

**Task:** Verify whether a claimed table of metal reflectance (F0) linear RGB values
originates from the Unity Shader Graph "Metal Reflectance Node" documentation.

## (a) Fetch results — all three URLs succeeded

| # | URL | Result |
|---|-----|--------|
| 1 | https://docs.unity3d.com/Packages/com.unity.shadergraph@15.0/manual/Metal-Reflectance-Node.html | SUCCESS (HTTP 200, package version **15.0.7**) |
| 2 | https://raw.githubusercontent.com/Unity-Technologies/Graphics/master/Packages/com.unity.shadergraph/Documentation~/Metal-Reflectance-Node.md | SUCCESS (source markdown) |
| 3 | https://docs.unity3d.com/Packages/com.unity.shadergraph@12.1/manual/Metal-Reflectance-Node.html | SUCCESS (HTTP 200, package version **12.1.16**) |

All three sources are byte-for-byte consistent in the numeric values.

## (b) EXACT table as it appears on the page (verbatim)

The page presents these NOT as a table of "reflectance" values, but as a
**"Generated Code Example"** — a list of `float3` literals, one per material:

```
Iron      float3 _MetalReflectance_Out = float3(0.560, 0.570, 0.580);
Silver    float3 _MetalReflectance_Out = float3(0.972, 0.960, 0.915);
Aluminium float3 _MetalReflectance_Out = float3(0.913, 0.921, 0.925);
Gold      float3 _MetalReflectance_Out = float3(1.000, 0.766, 0.336);
Copper    float3 _MetalReflectance_Out = float3(0.955, 0.637, 0.538);
Chromium  float3 _MetalReflectance_Out = float3(0.550, 0.556, 0.554);
Nickel    float3 _MetalReflectance_Out = float3(0.660, 0.609, 0.526);
Titanium  float3 _MetalReflectance_Out = float3(0.542, 0.497, 0.449);
Cobalt    float3 _MetalReflectance_Out = float3(0.662, 0.655, 0.634);
Platinum  float3 _MetalReflectance_Out = float3(0.672, 0.637, 0.585);
```

The "Controls" section lists the dropdown **Options** as:
`Iron, Silver, Aluminium, Gold, Copper, Chromium, Nickel, Titanium, Cobalt, Platform`

Note the spelling: the page uses **"Aluminium"** (British), not "Aluminum".

## (c) Comparison against the claimed values

| Metal | Claimed | On page | Match? |
|-------|---------|---------|--------|
| Gold | (1.000, 0.766, 0.336) | (1.000, 0.766, 0.336) | ✅ exact |
| Copper | (0.955, 0.637, 0.538) | (0.955, 0.637, 0.538) | ✅ exact |
| Silver | (0.972, 0.960, 0.915) | (0.972, 0.960, 0.915) | ✅ exact |
| Aluminum | (0.913, 0.921, 0.925) | (0.913, 0.921, 0.925) [as "Aluminium"] | ✅ exact values; name spelling differs |
| Iron | (0.560, 0.570, 0.580) | (0.560, 0.570, 0.580) | ✅ exact |
| Chromium/Chrome | (0.550, 0.556, 0.554) | (0.550, 0.556, 0.554) [as "Chromium"] | ✅ exact values; name "Chrome" not used |
| Brass | (0.910, 0.778, 0.423) | **NOT PRESENT** | ❌ NOT on the page |

### Key discrepancy
**Brass is NOT a material option in the Unity Shader Graph Metal Reflectance
node.** The string "brass" appears nowhere in the page (HTML 15.0, HTML 12.1, or
the master markdown source). The node's materials are exactly:
Iron, Silver, Aluminium, Gold, Copper, Chromium, Nickel, Titanium, Cobalt, and
"Platform" (a platform-specific passthrough). Therefore the claimed Brass value
(0.910, 0.778, 0.423) does NOT originate from this documentation page.

## (d) Statements on the page about what the values represent

The page's exact wording (Description section):

> "Returns a **Metal Reflectance** value for a physically based material. The
> material to use can be selected with the **Material** dropdown parameter on
> the Node.
>
> When using **Specular** **Workflow** on a PBR Master Node this value should be
> supplied to the **Specular** Port. When using **Metallic** **Workflow** this
> value should be supplied to the **Albedo** Port."

Observations:
- The page calls them "Metal Reflectance" values for "a physically based
  material" and directs them to the **Specular** port (specular workflow) or the
  **Albedo** port (metallic workflow). This is consistent with F0 (normal-
  incidence specular reflectance) semantics.
- The page does **NOT** use the word "linear". The string "linear" does not
  appear anywhere in the page. It also does not present a table titled
  "reflectance values for common metals".
- There is no explicit statement that these are linear-space values, no units,
  and no cited source/measurement reference.

## Conclusion

The values for Gold, Copper, Silver, Aluminum(Aluminium), Iron, and
Chromium/Chrome match the Unity Shader Graph Metal Reflectance Node documentation
**exactly**. However:
1. **Brass is not on the page at all** — its claimed F0 is NOT from this source.
2. The documentation labels them as "Generated Code Example" `float3` literals,
   not as a formal table of "reflectance values".
3. The page never states the values are "linear", nor does it describe them as
   "reflectance values for common metals".

Therefore the blanket claim that "these exact linear RGB values come from the
Unity Shader Graph Metal Reflectance node documentation" is only **partially
true**: 6 of the 7 metals match, Brass does not appear, and the "linear" /
"reflectance values for common metals" phrasing is not from this page.

## Source citations
- Unity Shader Graph 15.0.7 — Metal Reflectance Node:
  https://docs.unity3d.com/Packages/com.unity.shadergraph@15.0/manual/Metal-Reflectance-Node.html
- Unity Shader Graph 12.1.16 — Metal Reflectance Node:
  https://docs.unity3d.com/Packages/com.unity.shadergraph@12.1/manual/Metal-Reflectance-Node.html
- Unity Graphics repo (source markdown):
  https://raw.githubusercontent.com/Unity-Technologies/Graphics/master/Packages/com.unity.shadergraph/Documentation~/Metal-Reflectance-Node.md
